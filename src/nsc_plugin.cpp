// nsc_plugin.cpp — NSC Installer & Compiler (pure C++ MO2 plugin, uibase v2.5.2 SDK)
//
// 设计要点：
//   * 这是一个「薄编排层」插件。重活（解包 / 编译合并）全部委托给已经验证过的独立
//     Python 脚本（nsc_unpack.py / nsc_to_datawin32.py），避免把 Python 插件框架带进
//     进程、也避免重新实现归档/合并逻辑。
//   * 不链接、不编整套 uibase：只把 versioninfo.cpp 编成极小静态库链进来，运行时直接
//     复用 MO2 自带的 uibase.dll 接口（ABI 同版本，纯虚接口只通过指针虚调用，不会
//     产生第二份 uibase.dll，也不会污染其他游戏）。
//   * 隔离：只在 NARUTO X BORUTO / Storm Connections / NSC 游戏实例认领与操作。
//   * 编译触发挂在 onAboutToRun（启动游戏前、MO2 完全空闲），彻底避开安装期竞争
//     —— 这正是此前 Python 安装器「安装并编译时闪退」的根因（它用 sys.executable
//     拉起了第二个 MO2 实例抢 usvfs）。

#include "nsc_plugin.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QStringList>
#include <QDebug>

#include "versioninfo.h"

namespace {

// 支持的安装器扩展名（自定义 NSC 容器）。
const QStringList kNscExtensions = {
    QStringLiteral("nsc"),  QStringLiteral("ensc"), QStringLiteral("uns"),
    QStringLiteral("unse"), QStringLiteral("nus4")};

// 解析 Python 解释器路径。优先级：插件设置 > PATH(python/python3) > WorkBuddy 自带 >
// py.exe。返回可执行文件路径（若是 py.exe，调用方需自行补 "-3"）。
QString resolvePython(const MOBase::IOrganizer* organizer)
{
  if (organizer) {
    QVariant v = organizer->pluginSetting(QStringLiteral("NscInstaller"),
                                         QStringLiteral("pythonPath"));
    if (v.isValid() && !v.toString().isEmpty() &&
        QFileInfo(v.toString()).exists()) {
      return v.toString();
    }
  }

  for (const QString& cand :
       {QStringLiteral("python"), QStringLiteral("python3")}) {
    QString found = QStandardPaths::findExecutable(cand);
    if (!found.isEmpty()) {
      return found;
    }
  }

  // WorkBuddy 自带 Python（本机路径，作为后备）。
  QString wb = QStringLiteral(
      "C:/Users/Icarus/.workbuddy/binaries/python/versions/3.13.12/python.exe");
  if (QFileInfo(wb).exists()) {
    return wb;
  }

  QString py = QStandardPaths::findExecutable(QStringLiteral("py"));
  if (!py.isEmpty()) {
    return py;
  }
  return QStringLiteral("python");
}

// 解析 Python 工具链目录（nsc_unpack.py / nsc_to_datawin32.py 所在）。
// 优先级：插件设置 > <实例根>/../mod-tools/nsc-mo2 > <MO2>/plugins/nsc_tools。
QString resolveToolchainDir(const MOBase::IOrganizer* organizer,
                            const QString& basePath)
{
  if (organizer) {
    QVariant v = organizer->pluginSetting(QStringLiteral("NscInstaller"),
                                         QStringLiteral("toolchainDir"));
    if (v.isValid() && !v.toString().isEmpty() &&
        QFileInfo(v.toString()).isDir()) {
      return v.toString();
    }
  }

  // 默认：实例根的上一级里的 mod-tools/nsc-mo2（与本机布局一致）。
  QDir baseDir(basePath);
  if (baseDir.cdUp()) {
    QString candidate = baseDir.filePath(QStringLiteral("mod-tools/nsc-mo2"));
    if (QFileInfo(candidate).isDir()) {
      return candidate;
    }
  }

  // 备选：MO2 可执行文件所在目录里的 mod-tools/nsc-mo2（mod-tools 与 ModOrganizer.exe
  // 同级时，即使实例目录就是 MO2 根目录也能命中，无需手动配置）。
  QString appDirCandidate = QCoreApplication::applicationDirPath() +
                            QStringLiteral("/mod-tools/nsc-mo2");
  if (QFileInfo(appDirCandidate).isDir()) {
    return appDirCandidate;
  }

  // 兜底：插件旁边自带的 nsc_tools。
  QString pluginsDir =
      QCoreApplication::applicationDirPath() + QStringLiteral("/plugins");
  QString fallback = pluginsDir + QStringLiteral("/nsc_tools");
  if (QFileInfo(fallback).isDir()) {
    return fallback;
  }

  return baseDir.filePath(QStringLiteral("mod-tools/nsc-mo2"));
}

// 给 QProcess 调用补上 py.exe 需要的 "-3"。
QStringList withPython(const QString& python, QStringList args)
{
  if (QFileInfo(python).fileName().compare(QStringLiteral("py.exe"),
                                           Qt::CaseInsensitive) == 0) {
    args.prepend(QStringLiteral("-3"));
  }
  return args;
}

bool isPyExe(const QString& python)
{
  return QFileInfo(python).fileName().compare(QStringLiteral("py.exe"),
                                             Qt::CaseInsensitive) == 0;
}

}  // namespace

NscInstaller::NscInstaller([[maybe_unused]] QObject* parent)
    : IPluginInstallerCustom(), m_Organizer(nullptr)
{
  // IPluginInstallerCustom 直接继承自 QObject，父对象交由插件容器管理，
  // 此处不重复转发 parent（无接受 QObject* 的基类构造函数可用）。
}

bool NscInstaller::init(MOBase::IOrganizer* organizer)
{
  m_Organizer = organizer;
  if (!m_Organizer) {
    return false;
  }

  // 启动游戏前（MO2 空闲、VFS 尚未计算）触发一次编译合并。回调里会先做游戏隔离判断。
  m_Organizer->onAboutToRun([this](const QString&) -> bool {
    runCompileBeforeRun();
    return true;  // 永远允许启动
  });

  return true;
}

QString NscInstaller::name() const
{
  return QStringLiteral("NscInstaller");
}

QString NscInstaller::author() const
{
  return QStringLiteral("Icarus");
}

QString NscInstaller::description() const
{
  return QStringLiteral(
      "Install NSC/ENSC mods and auto-compile character/parameter merges for "
      "NARUTO X BORUTO (Storm Connections). Only active for NSC game instances.");
}

MOBase::VersionInfo NscInstaller::version() const
{
  return MOBase::VersionInfo(QStringLiteral("1.0.0"));
}

QList<MOBase::PluginSetting> NscInstaller::settings() const
{
  return {
      MOBase::PluginSetting(QStringLiteral("pythonPath"),
                            QStringLiteral("Path to the python interpreter used to "
                                          "run the unpack/compile scripts (empty = "
                                          "auto-detect from PATH)."),
                            QStringLiteral("")),
      MOBase::PluginSetting(
          QStringLiteral("toolchainDir"),
          QStringLiteral("Directory containing nsc_unpack.py and "
                        "nsc_to_datawin32.py (empty = auto-detect)."),
          QStringLiteral(""))};
}

unsigned int NscInstaller::priority() const
{
  // 高于默认安装器，确保我们的扩展名被我们认领。
  return 100;
}

bool NscInstaller::isSupportedGame() const
{
  if (!m_Organizer) {
    return false;
  }
  const MOBase::IPluginGame* game = m_Organizer->managedGame();
  if (!game) {
    return false;
  }
  const QString name = game->gameName().toUpper();
  const QString shortName = game->gameShortName();

  // NSC —— NARUTO X BORUTO / Storm Connections / NSUNSC
  if (name.contains(QStringLiteral("STORM CONNECTIONS")) ||
      name.contains(QStringLiteral("NARUTO X BORUTO")) ||
      shortName.compare(QStringLiteral("NSC"), Qt::CaseInsensitive) == 0) {
    return true;
  }
  // NS4 —— NARUTO SHIPPUDEN Ultimate Ninja STORM 4 / NSUNS4
  if (name.contains(QStringLiteral("STORM 4")) ||
      name.contains(QStringLiteral("STORM4")) ||
      shortName.compare(QStringLiteral("NS4"), Qt::CaseInsensitive) == 0) {
    return true;
  }
  return false;
}

QString NscInstaller::gameShort() const
{
  if (!m_Organizer) {
    return QStringLiteral("NSC");
  }
  const MOBase::IPluginGame* game = m_Organizer->managedGame();
  if (!game) {
    return QStringLiteral("NSC");
  }
  const QString name = game->gameName().toUpper();
  if (name.contains(QStringLiteral("STORM 4")) ||
      name.contains(QStringLiteral("STORM4")) ||
      game->gameShortName().compare(QStringLiteral("NS4"),
                                    Qt::CaseInsensitive) == 0) {
    return QStringLiteral("NS4");
  }
  return QStringLiteral("NSC");
}

std::set<QString> NscInstaller::supportedExtensions() const
{
  std::set<QString> exts;
  for (const QString& e : kNscExtensions) {
    exts.insert(e);
  }
  return exts;
}

bool NscInstaller::isArchiveSupported(const QString& archiveName) const
{
  const QString ext = QFileInfo(archiveName).suffix().toLower();
  return kNscExtensions.contains(ext);
}

bool NscInstaller::isArchiveSupported(
    std::shared_ptr<const MOBase::IFileTree> /*tree*/) const
{
  // 自定义 .nsc 容器一般不是标准 zip，MO2 打不开 → 会走上面的 filename 变体（我们认领）。
  // 万一 MO2 能打开，退回默认安装器即可（编译阶段靠扫描全局表仍会纳入），故这里保守返回
  // false，避免误吞其它归档。
  return false;
}

MOBase::IPluginInstaller::EInstallResult NscInstaller::install(
    MOBase::GuessedValue<QString>& modName, QString /*gameName*/,
    const QString& archiveName, const QString& version, int nexusID)
{
  if (!isSupportedGame()) {
    // 不是 NSC/NS4 实例：明确放弃，让 MO2 交给其它安装器，绝不影响其它游戏。
    return MOBase::IPluginInstaller::RESULT_NOTATTEMPTED;
  }

  // 给一个默认 mod 名（去掉扩展名）。
  if (modName->isEmpty()) {
    modName.update(QFileInfo(archiveName).completeBaseName(),
                   MOBase::GUESS_GOOD);
  }

  // 创建 MO2 mod（已存在会弹合并/重命名对话框；必须在主线程 → install() 本就在主线程）。
  MOBase::IModInterface* mod = m_Organizer->createMod(modName);
  if (!mod) {
    return MOBase::IPluginInstaller::RESULT_CANCELED;
  }

  const QString modDir = mod->absolutePath();
  if (!version.isEmpty()) {
    mod->setVersion(MOBase::VersionInfo(version));
  }
  if (nexusID > 0) {
    mod->setNexusID(nexusID);
  }

  // 用 Python 脚本解包。
  const QString python = resolvePython(m_Organizer);
  const QString toolchain =
      resolveToolchainDir(m_Organizer, m_Organizer->basePath());
  const QString script = toolchain + QStringLiteral("/nsc_unpack.py");

  QStringList args;
  args << script << archiveName << modDir;

  qDebug() << "[NscInstaller] unpacking" << archiveName << "->" << modDir;
  const int rc = QProcess::execute(python, withPython(python, args));
  if (rc != 0) {
    qWarning() << "[NscInstaller] nsc_unpack.py failed, rc =" << rc;
    return MOBase::IPluginInstaller::RESULT_FAILED;
  }

  // 刷新 mod 列表，让新解包出来的文件立即可见。
  m_Organizer->refresh(false);
  return MOBase::IPluginInstaller::RESULT_SUCCESS;
}

void NscInstaller::runCompileBeforeRun()
{
  if (!isSupportedGame()) {
    return;
  }

  const QString python = resolvePython(m_Organizer);
  const QString base = m_Organizer->basePath();
  const QString toolchain = resolveToolchainDir(m_Organizer, base);
  const QString script = toolchain + QStringLiteral("/nsc_to_datawin32.py");
  if (!QFileInfo(script).exists()) {
    qWarning() << "[NscInstaller] compile script not found:" << script;
    return;
  }

  QStringList args;
  args << script << QStringLiteral("--base") << base
       << QStringLiteral("--profile") << m_Organizer->profileName()
       << QStringLiteral("--all-fragments") << QStringLiteral("--merge")
       << QStringLiteral("--swap")
       << QStringLiteral("--game") << gameShort();

  qDebug() << "[NscInstaller] compiling NSC mods before launch (base =" << base
           << ", profile =" << m_Organizer->profileName() << ")";
  const int rc = QProcess::execute(python, withPython(python, args));
  qDebug() << "[NscInstaller] compile finished, rc =" << rc;
}
