#pragma once

#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>

namespace nsc {

/**
 * 启动前的 VFS 准备工作 —— 全部纯文件操作，替代 nsc_vfs_prepare.py 里的
 * 「嵌套归档展开 + mod 布局归一化 + data_win32 目标目录预建」三段。
 *
 * 为什么不走 Python：这三段在**每次启动游戏前**都要跑，QProcess 起解释器
 * 200~400ms 且要求用户机器有 Python。现在由插件直接复用 7z.dll（Archive）和
 * 布局规则（Layout），零外部依赖、零子进程。
 *
 * 仍然保留 Python 的只有「调官方 .NET 编译器合并全局表」那一步（nsc_tables.py），
 * 它依赖外部工具链 NscCompileHost.exe / FastCpk，不属于纯文件操作。
 */
class Prepare final
{
public:
  /// 读 modlist.txt，返回启用(+)的 mod 名，保持顺序。
  static QStringList enabledMods(const QString& basePath, const QString& profile);

  /// moddingapi 宿主 mod 的前缀（这些走 Root/ 映射，不落 data_win32）。
  /// game 为 "NSC"/"NS4"。
  static QString moddingApiPrefix(const QString& game);

  /// 就地展开 mod 根目录里未解开的归档（.nsc/.ensc/.zip/...），
  /// 最多 3 轮，展开后删壳体。返回被展开的归档文件名。
  /// 复用 nsc::Archive::extract；每轮展开后跑一次布局归一化。
  static QStringList expandNested(const QString& modDir,
                                  const QString& backupRoot);

  /// 计算 data_win32 下必须存在的相对目录集合。
  /// routing: nxb_win32_routing.json 读出的 {文件名: 目标相对目录}。
  static QSet<QString> requiredDirs(const QString& basePath, const QString& profile,
                                    const QString& game,
                                    const QHash<QString, QString>& routing);

  /// 在 gameRoot/data_win32 下建好 needed 里的每个目录。
  /// 返回实际新建的个数。
  static int createDirs(const QString& gameRoot, const QSet<QString>& needed);

  /// 该 mod 是否「自带成品 CPK」（moddingapi/mods/**/*.cpk）——CPKLoader 直接挂，
  /// 无需编译。
  static bool hasOwnCpk(const QString& modDir);

  /// 该 mod 是否是「需要编译的 NSC 包」（有 mod_config.ini / Characters /
  /// Resources/Files/data 之一）。
  static bool isPackage(const QString& modDir);
};

}  // namespace nsc
