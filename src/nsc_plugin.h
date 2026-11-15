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
 *     在 install() 里创建 MO2 mod 并调用独立的 nsc_unpack.py 完成归档解包。
 *  2. 编译合并：在 onAboutToRun（启动游戏前，MO2 完全空闲）钩子里调用已验证的
 *     nsc_to_datawin32.py 合并带碎片全局参数表的 mod，并原子改写 modlist。
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

private:
  MOBase::IOrganizer* m_Organizer;
  // 认领 NSC（Storm Connections）与 NS4（Ultimate Ninja STORM 4）两类实例。
  bool isSupportedGame() const;
  // 返回档案短名 "NSC" / "NS4"（传给 nsc_to_datawin32.py --game）。
  QString gameShort() const;
  void runCompileBeforeRun();
};
