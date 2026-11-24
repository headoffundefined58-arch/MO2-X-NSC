// nsc_plugin.cpp — MO2 主导的 NSC 安装器（纯 C++，零 Python，零物理写入）
//
// MO2 是唯一管理器。插件只做安装期的事：
//   * 认领 .nsc/.ensc/.uns/.unse/.nus4，原生 7z 解包 + 布局归一化 + 嵌套展开。
//   * 全部内容经 MO2 的 VFSContentLayout 在进程内虚拟成 data_win32，
//     游戏物理根永不写入任何目录/文件（data / data_dlc 之外一律不创建）。
//   * 不编译 CPK、不拉 Python 子进程、不写 moddingapi、不写 overwrite。
//     本游戏的 mod 是 Resources/Files/data/... 散文件，靠 VFS 路由表直接可读。
//
// 不链接整套 uibase：versioninfo.cpp 编成极小静态库，运行时复用 MO2 自带 uibase.dll。
// 隔离：只在 NARUTO X BORUTO / Storm Connections / NSC 实例认领与操作。

#include "nsc_plugin.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QTextStream>
#include <QDebug>

#include "nsc_arc.h"
#include "nsc_layout.h"
#include "nsc_prepare.h"
#include "nsc_merge.h"
#include "versioninfo.h"

namespace {

// 支持的安装器扩展名（自定义 NSC 容器）。
const QStringList kNscExtensions = {
    QStringLiteral("nsc"),  QStringLiteral("ensc"), QStringLiteral("uns"),
    QStringLiteral("unse"), QStringLiteral("nus4")};

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
  // VFS 直读散文件，不再编译 CPK / 不再拉 Python 子进程。
  Q_UNUSED(modDir);
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

  // ---- 原生步骤 3：修复全局参数表 ----
  // NSC 社区 mod 把编译器工作区原样打包，Characters/<id>/data 与 Models/<id>/data
  // 下是几百字节的参数表碎片。MO2 的 layout_markers 含 "data"，VFS 会把这些碎片
  // 铺成游戏的 data/ui/max/select/characterSelectParam.xfbin 等整表，覆盖原版
  // 几十 KB 的完整表 → 引擎 memcpy 越界 → 选人界面/进场闪退。
  // 修复：遮蔽工作区 data 目录 + 把碎片合并进完整基底表写到 overwrite。
  // 全程只在 MO2 实例内部读写，游戏物理根零写入。
  const nsc::Merge::Result merge = nsc::Merge::fixEnabledMods(
      base, profile, layoutBackupRoot());
  logLine(QStringLiteral("  tables: shadowed=%1 dirs, merged=%2 fragments, "
                         "records %3 -> %4")
              .arg(merge.shadowedDirs)
              .arg(merge.fragmentsMerged)
              .arg(merge.recordsTotal - merge.recordsAdded)
              .arg(merge.recordsTotal));
  logLine(QStringLiteral("  tables: base=%1 (%2 records), candidates=%3, "
                         "rejected=%4, duplicates=%5")
              .arg(merge.baseSource)
              .arg(merge.baseRecords)
              .arg(merge.fragmentCandidates)
              .arg(merge.fragmentRejected)
              .arg(merge.fragmentDuplicates));
  if (!merge.finalTable.isEmpty()) {
    logLine(QStringLiteral("  tables: wrote ") + merge.finalTable);
  }
  for (const QString& e : merge.errors.mid(0, 8)) {
    logLine(QStringLiteral("  tables: WARN ") + e);
  }

  // 注意：绝不在游戏物理根建任何目录。MO2 的 VFS 在进程内虚拟出 data_win32，
  // 游戏根保持干净（data / data_dlc 之外的目录一律不创建）。
  logLine(QStringLiteral("before-run: native prep done (expanded=%1 mods)")
              .arg(expanded));

  logLine(QStringLiteral("before-run: finished (VFS + in-DLL table merge, "
                         "no physical dirs)"));
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
