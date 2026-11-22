// nsc_plugin.cpp — NSC Installer & Compiler (pure C++ MO2 plugin, uibase v2.5.2 SDK)
//
// 设计要点：
//   * 解包 / 布局归一化 / 嵌套展开 / VFS 目录预建全部由原生 C++ 模块完成
//     （nsc_arc + nsc_layout + nsc_prepare），复用 MO2 自带的 7z.dll，零 Python、
//     零子进程。这些是安装/启动路径上每次都跑的活。
//   * 唯一还需要 Python 的是「调官方 .NET 编译器合并全局表」——它依赖外部工具链
//     NscCompileHost.exe / FastCpk，不属于纯文件操作，保留 nsc_tables.py。
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

#include "nsc_arc.h"
#include "nsc_layout.h"
#include "nsc_prepare.h"
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

  // 原生解包（7z.dll）+ 布局归一化，不再依赖 Python 解释器。
  qDebug() << "[NscInstaller] unpacking" << archiveName << "->" << modDir;
  logLine(QStringLiteral("install: %1 -> %2").arg(archiveName, modDir));
  if (!unpackMod(archiveName, modDir)) {
    logLine(QStringLiteral("unpack failed: ") + archiveName);
    qWarning() << "[NscInstaller] unpack failed:" << archiveName;
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

  // 【2026-11-19 修复】MO2 的 Simple installer 只解最外层 zip；包内若还套着
  // .nsc/.ensc/.zip 容器（如「Naruto - Kurama Chakra Mode」里就是一个 33MB 的
  // .nsc），mod 根会只剩这个容器文件。此前这里直接调 compileModInPlace，
  // dirLooksLikeNscPackage 认不出 → 打印 "skip (not an NSC package)" 就走了，
  // 容器永远不被展开。现按 install() 的路径补齐：先循环展开嵌套归档 + 布局
  // 归一化，再判断是否编译。before-run 里也有同样的 sweep 作兜底，但那要等
  // 下次启动；安装当场展开才能真正做到「装完即用」。
  const QString backup = layoutBackupRoot();
  const QStringList nested = nsc::Prepare::expandNested(modDir, backup);
  for (const QString& n : nested) {
    logLine(QStringLiteral("  installation: nested ") + n);
  }
  // 【2026-11-22 修复】无条件做布局归一化。
  // 之前只在 nested 非空时剥壳 —— 但「头文件夹已解包、mod 根只剩一个
  // <头文件夹>/」的情况没有归档可展开，nested 为空，剥壳被跳过，
  // dirLooksLikeNscPackage 认不出 → 既不剥壳也不编译（日志显示
  // "installation: nested ..." 后直接 "skip (not an NSC package)"，无 layout 动作）。
  // 现在无论有没有 nested 都跑一次 normalizeOne：它会剥掉单一头文件夹、
  // 折叠重复目录、归位散落 xfbin，是幂等的。
  const QStringList acts = nsc::Layout::normalizeOne(modDir, backup);
  for (const QString& a : acts.mid(0, 12)) {
    logLine(QStringLiteral("  installation: layout ") + a);
  }
  if (!acts.isEmpty() || !nested.isEmpty()) {
    // 让新展开出来的文件立刻在 MO2 里可见（vfs/冲突视图依赖这层刷新）。
    if (m_Organizer) {
      m_Organizer->refresh(false);
    }
  }

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
  // 安装期成品化：识别是否需要编译，需要则当场编译，把资产 CPK 放进该 mod
  // 自己的 moddingapi/mods/<mod名>/；全局名册表留给启动期共享编译。
  // 旧 nsc_mod_build.py 已废弃（main 是 no-op），改用 nsc_install_build.py。
  const QString buildScript = toolchain + QStringLiteral("/nsc_install_build.py");
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
    qWarning() << "[NscInstaller] nsc_install_build.py failed, rc =" << brc;
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

  const QString base = m_Organizer->basePath();
  const QString profile = m_Organizer->profileName();
  const QString game = gameShort();
  const QString backup = layoutBackupRoot();
  logLine(QStringLiteral("before-run: start (profile=%1, game=%2)").arg(profile, game));

  // ---- 原生步骤 1：展开嵌套归档（zip 里套 .nsc）——MO2 只解最外层。----
  const QStringList enabled = nsc::Prepare::enabledMods(base, profile);
  int expanded = 0;
  for (const QString& name : enabled) {
    const QString modDir = base + QStringLiteral("/mods/") + name;
    if (!QFileInfo(modDir).isDir()) {
      continue;
    }
    const QStringList done = nsc::Prepare::expandNested(modDir, backup);
    if (!done.isEmpty()) {
      ++expanded;
      logLine(QStringLiteral("  expanded %1: %2").arg(name, done.join(QLatin1Char(','))));
    }
  }

  // ---- 原生步骤 2：布局归一化（重名目录折叠 / 包壳上移 / 散文件归位）。----
  const QStringList layoutActions = nsc::Layout::normalizeProfile(base, profile, backup);
  if (!layoutActions.isEmpty()) {
    logLine(QStringLiteral("  layout: %1 action(s)").arg(layoutActions.count()));
    for (const QString& a : layoutActions.mid(0, 12)) {
      logLine(QStringLiteral("    ") + a);
    }
  }

  // ---- 原生步骤 3：预建 USVFS 目标目录。----
  // USVFS 会静默丢弃「目标目录不存在」的映射，而 mod 内容有多个类别（ui/message/
  // sound/skill/effect/rpg/spcload…），缺一个该类映射就整体失效。
  const QHash<QString, QString> routing = nsc::Layout::readRouting(base);
  const QSet<QString> needed = nsc::Prepare::requiredDirs(base, profile, game, routing);
  const QString root = gameRootPath();
  if (root.isEmpty()) {
    qWarning() << "[NscInstaller] game root not found, skip VFS dir prep";
    logLine(QStringLiteral("  game root not found - VFS dir prep skipped"));
  } else {
    const int created = nsc::Prepare::createDirs(root, needed);
    logLine(QStringLiteral("  vfs dirs: %1 required, %2 created (root=%3)")
                .arg(needed.count()).arg(created).arg(root));
  }
  logLine(QStringLiteral("before-run: native prep done (expanded=%1 mods)")
              .arg(expanded));

  // ---- 仍需 Python 的步骤：调官方 .NET 编译器合并全局表并合成 CPK。----
  // sweep_pending 会编译还没成品的 NSC 包；compose_runtime_tables 按当前启用组合
  // 重算总表。二者依赖外部工具链 NscCompileHost.exe / FastCpk，非纯文件操作。
  const QString python = resolvePython(m_Organizer);
  const QString toolchain = resolveToolchainDir(m_Organizer, base);
  const QString script = toolchain + QStringLiteral("/nsc_vfs_prepare.py");
  if (!QFileInfo(script).exists()) {
    qWarning() << "[NscInstaller] compile script not found:" << script;
    return;
  }
  // 脚本里的展开/布局因原生步骤 1&2 已经跑过而变成空操作（幂等），这里真正要它做的是
  // sweep_pending（编译新包）+ compose_runtime_tables（重算总表）。
  QStringList args;
  args << script << QStringLiteral("--base") << base
       << QStringLiteral("--profile") << profile
       << QStringLiteral("--game") << game;
  if (!root.isEmpty()) {
    args << QStringLiteral("--game-root") << root;
  }
  logLine(QStringLiteral("before-run: compile+compose via toolchain"));
  const int rc = QProcess::execute(python, withPython(python, args));
  logLine(QStringLiteral("before-run: finished, rc=%1").arg(rc));
}

QString NscInstaller::gameRootPath() const
{
  if (!m_Organizer || !m_Organizer->managedGame()) {
    return QString();
  }
  const QDir dir = m_Organizer->managedGame()->gameDirectory();
  return dir.path().isEmpty() ? QString() : dir.absolutePath();
}

QString NscInstaller::layoutBackupRoot() const
{
  // 布局归一化时冲突文件挪这里（不硬删）；放 mod-tools 下，不污染 mod 目录。
  return QCoreApplication::applicationDirPath() +
         QStringLiteral("/mod-tools/nsc-mo2/backup/mod_layout");
}

bool NscInstaller::unpackMod(const QString& archiveName, const QString& modDir)
{
  // 1) 解最外层归档到 mod 根（.nsc/.ensc/.zip/... 本身就是容器）。
  QString err;
  if (!nsc::Archive::extract(archiveName, modDir, nullptr, &err)) {
    logLine(QStringLiteral("unpack: extract failed: %1").arg(err));
    return false;
  }

  // 2) 循环展开嵌套归档（zip 里再套 .nsc）。
  const QString backup = layoutBackupRoot();
  const QStringList nested = nsc::Prepare::expandNested(modDir, backup);
  for (const QString& n : nested) {
    logLine(QStringLiteral("  unpack: nested ") + n);
  }

  // 3) 布局归一化（重名目录折叠 / 包壳上移 / 散文件归位）。
  const QStringList acts = nsc::Layout::normalizeOne(modDir, backup);
  for (const QString& a : acts.mid(0, 12)) {
    logLine(QStringLiteral("  layout: ") + a);
  }

  // 4) 写元数据 nsc_mod.json（replace 类型，与原 Python 版一致）。
  const QString metaPath = modDir + QStringLiteral("/nsc_mod.json");
  if (!QFileInfo::exists(metaPath)) {
    QFile f(metaPath);
    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
      const QByteArray json =
          "{\n  \"kind\": \"replace\",\n  \"name\": \"" +
          QFileInfo(modDir).fileName().toUtf8() +
          "\",\n  \"version\": \"\",\n  \"globalOverrides\": false\n}\n";
      f.write(json);
      f.close();
    }
  }
  logLine(QStringLiteral("unpack OK: ") + archiveName);
  return true;
}
