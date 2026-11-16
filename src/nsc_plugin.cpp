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
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QStringList>
#include <QTextStream>
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

// 原样解包的 NSC 包形状：mod_config.ini / Characters/ / Resources/Files/ / moddingapi/
// （服装、改色这类散装 mod 是 spc/ ui/ sound/ 布局，不需要编译，别去碰它们。）
bool dirLooksLikeNscPackage(const QString& modDir)
{
  const QDir d(modDir);
  if (!d.exists()) {
    return false;
  }
  return QFileInfo(d.filePath(QStringLiteral("mod_config.ini"))).isFile()
      || QFileInfo(d.filePath(QStringLiteral("Characters"))).isDir()
      || QFileInfo(d.filePath(QStringLiteral("Resources/Files"))).isDir()
      || QFileInfo(d.filePath(QStringLiteral("moddingapi"))).isDir();
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

  logLine(QStringLiteral("init: game=%1 short=%2 supported=%3 base=%4")
              .arg(m_Organizer->managedGame() ? m_Organizer->managedGame()->gameName()
                                              : QStringLiteral("<none>"))
              .arg(gameShort())
              .arg(isSupportedGame() ? QStringLiteral("yes") : QStringLiteral("no"))
              .arg(m_Organizer->basePath()));

  // 启动游戏前（MO2 空闲、VFS 尚未计算）跑一遍：把还没编译的 NSC 包就地编译成成品，
  // 再预建 USVFS 目标目录。回调里会先做游戏隔离判断。
  m_Organizer->onAboutToRun([this](const QString&) -> bool {
    runCompileBeforeRun();
    return true;  // 永远允许启动
  });

  return true;
}

void NscInstaller::logLine(const QString& message)
{
  qDebug() << "[NscInstaller]" << message;

  const QString path = QCoreApplication::applicationDirPath() +
                       QStringLiteral("/plugins/nsc_plugin.log");
  QFileInfo fi(path);
  if (fi.exists() && fi.size() > (1 << 20)) {   // 1 MB 上限，超了就重开
    QFile::remove(path);
  }
  QFile f(path);
  if (!f.open(QIODevice::Append | QIODevice::Text)) {
    return;
  }
  QTextStream out(&f);
  out << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))
      << "  " << message << "\n";
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
  // 只在「归档打不开、只拿到文件名」的那条路径上起作用（那时我们会认领扩展名）。
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

// 归档树版本的认领判定：**故意返回 false**。
//
// 想按「包内结构」认领（Characters/、mod_config.ini……）就必须遍历 IFileTree，而
// IFileTree::size()/at()/begin() 全部内联转发到 `entries()`——它是**非虚**函数、
// 定义在 uibase.dll 里、没有导出到我们手上（本插件不链接 uibase.lib，见 CMakeLists）。
// 一调用就是 LNK2019（实测：unresolved external ... entries@IFileTree@MOBase）。
//
// 所以不跟 Simple Installer 抢归档：让 MO2 自己解开（.nsc 就是 zip），装完由
// onInstallationEnd 就地编译；万一 MO2 没通知到，启动前的 sweep 还会兜一次。
// 字符串重载（能拿到归档文件名）仍然认领扩展名，覆盖「不是 zip、MO2 打不开」的包。
bool NscInstaller::isArchiveSupported(
    std::shared_ptr<const MOBase::IFileTree> tree) const
{
  Q_UNUSED(tree);
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
  logLine(QStringLiteral("install: %1 -> %2").arg(archiveName, modDir));
  const int rc = QProcess::execute(python, withPython(python, args));
  if (rc != 0) {
    logLine(QStringLiteral("nsc_unpack.py failed, rc=%1").arg(rc));
    qWarning() << "[NscInstaller] nsc_unpack.py failed, rc =" << rc;
    return MOBase::IPluginInstaller::RESULT_FAILED;
  }

  // 安装时即编译：把需要编译的 mod 就地编译成「成品」——
  //   合并后的整表 + 该 mod 自己的资产，按 data_win32 相对布局铺好。
  //   这个 mod 在 MO2 里就是成品，启用即可玩；不做全局合并包，也不禁用源 mod。
  compileModInPlace(modDir);

  // 刷新 mod 列表，让新解包出来的文件立即可见。
  m_Organizer->refresh(false);
  return MOBase::IPluginInstaller::RESULT_SUCCESS;
}

void NscInstaller::onInstallationEnd(MOBase::IPluginInstaller::EInstallResult result,
                                     MOBase::IModInterface* newMod)
{
  // 不管归档最后是哪个安装器处理的（我们的 / Simple / BAIN / FOMOD……），装完都在这儿收尾：
  // 只要是原样解包的 NSC 包，就地编译成成品。这是「装完即编译」最可靠的挂点。
  if (result != MOBase::IPluginInstaller::RESULT_SUCCESS || !newMod) {
    return;
  }
  if (!isSupportedGame()) {
    return;
  }
  const QString modDir = newMod->absolutePath();
  logLine(QStringLiteral("installation finished: %1 -> %2").arg(newMod->name(), modDir));
  compileModInPlace(modDir);
}

bool NscInstaller::compileModInPlace(const QString& modDir)
{
  if (!m_Organizer) {
    return false;
  }
  if (QFileInfo(QDir(modDir).filePath(QStringLiteral(".nsc_compiled"))).isFile()) {
    logLine(QStringLiteral("already compiled: ") + modDir);
    return true;
  }
  if (!dirLooksLikeNscPackage(modDir)) {
    logLine(QStringLiteral("skip (not an NSC package): ") + modDir);
    return false;
  }

  const QString python = resolvePython(m_Organizer);
  const QString toolchain =
      resolveToolchainDir(m_Organizer, m_Organizer->basePath());
  const QString buildScript = toolchain + QStringLiteral("/nsc_mod_build.py");
  if (!QFileInfo(buildScript).isFile()) {
    logLine(QStringLiteral("build script not found: ") + buildScript);
    return false;
  }

  QStringList bargs;
  bargs << buildScript << modDir
        << QStringLiteral("--base") << m_Organizer->basePath()
        << QStringLiteral("--profile") << m_Organizer->profileName()
        << QStringLiteral("--game") << gameShort();
  logLine(QStringLiteral("compile: %1 %2").arg(python, bargs.join(QLatin1Char(' '))));
  const int brc = QProcess::execute(python, withPython(python, bargs));
  if (brc != 0) {
    // 编译失败不阻断安装：mod 保持原始布局，内容仍在。
    logLine(QStringLiteral("compile failed, rc=%1 (mod left in original layout)").arg(brc));
    qWarning() << "[NscInstaller] nsc_mod_build.py failed, rc =" << brc;
    return false;
  }
  logLine(QStringLiteral("compiled OK: ") + modDir);
  return true;
}

void NscInstaller::runCompileBeforeRun()
{
  if (!isSupportedGame()) {
    return;
  }

  const QString python = resolvePython(m_Organizer);
  const QString base = m_Organizer->basePath();
  const QString toolchain = resolveToolchainDir(m_Organizer, base);
  // 启动前：(1) 扫一遍还没编译的 NSC 包并就地编译成成品（安装器钩子没兜住的情况，
  // 以及手工拷进 mods/ 的包）；(2) 预建 USVFS 目标目录——USVFS 会静默丢弃目标目录
  // 不存在的映射，而 mod 有多种内容类别（ui/message/sound/skill/effect/rpg/ia/spcload/
  // Shaders），每类在 data_win32 下都有各自目录，缺一个该类映射就整体失效。
  const QString script = toolchain + QStringLiteral("/nsc_vfs_prepare.py");
  if (!QFileInfo(script).exists()) {
    qWarning() << "[NscInstaller] vfs prepare script not found:" << script;
    return;
  }

  QStringList args;
  args << script << QStringLiteral("--base") << base
       << QStringLiteral("--profile") << m_Organizer->profileName()
       << QStringLiteral("--game") << gameShort();

  qDebug() << "[NscInstaller] preparing USVFS target dirs (base =" << base
           << ", profile =" << m_Organizer->profileName() << ")";
  logLine(QStringLiteral("before-run: vfs prepare + sweep (profile=%1)")
              .arg(m_Organizer->profileName()));
  const int rc = QProcess::execute(python, withPython(python, args));
  logLine(QStringLiteral("before-run: finished, rc=%1").arg(rc));
  qDebug() << "[NscInstaller] vfs prepare finished, rc =" << rc;
}
