#pragma once

#include "iplugin.h"
#include "iplugininstaller.h"
#include "iplugininstallercustom.h"
#include "imoinfo.h"
#include "imodinterface.h"
#include "iplugingame.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <set>
#include <memory>

/**
 * NSC Installer & Compiler — 一个纯 C++ 的 MO2 插件（MO2 C++ SDK / uibase v2.5.2）。
 *
 * 职责（独立于 Python 插件框架）：
 *  1. 安装：通过 IPluginInstallerCustom 认领 .nsc/.ensc/.uns/.unse/.nus4，
 *     在 install() 里创建 MO2 mod，用原生 7z.dll 解包 + 布局归一化（零 Python）。
 *  2. 启动前准备：在 onAboutToRun（MO2 完全空闲）里做嵌套归档展开、布局归一化、
 *     VFS 目录预建，全部原生 C++；仅「合并全局表/合成 CPK」这步仍调 Python
 *     编排官方 .NET 编译器（依赖外部工具链）。
 *
 * 隔离：只在 NARUTO X BORUTO / Storm Connections / NSC 游戏实例认领与操作，
 * 不影响其他游戏（Skyrim / Fallout 等）的 MOD 管理。
 */
// 注意：只继承 IPluginInstallerCustom，不要再写 public QObject。因为
// IPluginInstallerCustom 自身已经 public 继承 QObject（见 uibase iplugininstallercustom.h），
// 重复写 QObject 会形成菱形继承，导致 moc 生成的 static_cast<QObject*> 二义（C2594）。
// Q_OBJECT 通过接口间接继承 QObject 仍可正常工作。
class NscInstaller : public MOBase::IPluginInstallerCustom
{
  Q_OBJECT
  Q_INTERFACES(MOBase::IPlugin MOBase::IPluginInstaller MOBase::IPluginInstallerCustom)
  Q_PLUGIN_METADATA(IID "com.tannin.ModOrganizer.Plugin/2.0" FILE "nsc_plugin.json")

public:
  explicit NscInstaller(QObject* parent = nullptr);

  // --- IPlugin ---
  bool init(MOBase::IOrganizer* organizer) override;
  QString name() const override;
  QString localizedName() const override { return name(); }
  QString author() const override;
  QString description() const override;
  MOBase::VersionInfo version() const override;
  QList<MOBase::PluginSetting> settings() const override;

  // --- IPluginInstaller ---
  unsigned int priority() const override;
  bool isManualInstaller() const override { return false; }
  bool isArchiveSupported(std::shared_ptr<const MOBase::IFileTree> tree) const override;
  bool isArchiveSupported(const QString& archiveName) const override;
  std::set<QString> supportedExtensions() const override;

  // --- IPluginInstallerCustom ---
  MOBase::IPluginInstaller::EInstallResult install(MOBase::GuessedValue<QString>& modName,
                                                   QString gameName,
                                                   const QString& archiveName,
                                                   const QString& version,
                                                   int nexusID) override;

  // 安装流程结束（无论哪个安装器干的活）——这是「装完即编译」的可靠挂点。
  void onInstallationEnd(MOBase::IPluginInstaller::EInstallResult result,
                         MOBase::IModInterface* newMod) override;

private:
  MOBase::IOrganizer* m_Organizer;
  // 认领 NSC（Storm Connections）与 NS4（Ultimate Ninja STORM 4）两类实例。
  bool isSupportedGame() const;
  // 返回档案短名 "NSC" / "NS4"（传给 nsc_to_datawin32.py --game）。
  QString gameShort() const;
  void runCompileBeforeRun();
  /// 游戏根路径（managedGame()->gameDirectory()），失败返回空串。
  QString gameRootPath() const;
  /// 备份根（布局归一化时冲突文件挪这里，不硬删）。
  QString layoutBackupRoot() const;
  /// 把单个 mod 目录就地解包+布局归一化（纯 C++，替代 nsc_unpack.py）。
  /// 返回 false 表示归档解不开。
  bool unpackMod(const QString& archiveName, const QString& modDir);
  // 把单个 mod 目录就地编译成成品（已编译过则直接返回 true）。
  // 这是唯一还需要 Python 的一步：调官方 .NET 编译器合并全局表。
  bool compileModInPlace(const QString& modDir);
  static void logLine(const QString& message);
};
