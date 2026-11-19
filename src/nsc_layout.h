#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

namespace nsc {

/**
 * 目录布局归一化 —— 让 mod 里的每个文件在 VFS 里只有一条可达路径。
 *
 * 本实例 MO2 的游戏插件声明 GameDataPath = "data_win32"，也就是虚拟 Data 根是
 * <game>/data_win32，mod 根下的目录会直接映射进去。所以：
 *
 *   mods/<m>/spc/x.xfbin              -> data_win32/spc/x.xfbin        OK
 *   mods/<m>/data_win32/spc/x.xfbin   -> data_win32/data_win32/spc/x   错（双层）
 *   mods/<m>/Resources/Files/data/... -> data_win32/Resources/...      错（三层包壳）
 *
 * 后两种（NSC 包 / 官方 zip 的原始形状）会让模型贴图整片失效，而 USVFS 只在日志里
 * 写一行 "doesn't exist"，界面上毫无提示 —— 这就是「槽位在、建模没了」的真因。
 *
 * 三条规则，全部幂等：
 *   1. 同名目录折叠：X/X 拆成一层（data_win32/data_win32/spc -> data_win32/spc）
 *   2. 包壳上移：根下唯一包壳（data_win32/、Resources/Files/data/、zip 里套的同名
 *      目录）整体提到 mod 根
 *   3. 散装 xfbin 归位：躺在根的 *.xfbin 按路由表放进子目录（默认 spc）
 *
 * 冲突一律「保留体积大的」，另一份移到 backupRoot 下，不硬删。
 */
class Layout final
{
public:
  /// 是否是 NSC 包（Models/ Characters/ moddingapi/ 任一存在）——包结构要原样留给编译器。
  static bool looksLikeNscPackage(const QString& modDir);

  /// 就地归一化单个 mod 目录，返回人类可读的动作清单。
  static QStringList normalizeOne(const QString& modDir, const QString& backupRoot);

  /// 对 profile 里所有启用的 mod 各做一次，返回所有动作。
  static QStringList normalizeProfile(const QString& basePath, const QString& profile,
                                      const QString& backupRoot);

  /// xfbin 路由表（来自 nxb_win32_routing.json，缺失时为空）。
  static QHash<QString, QString> readRouting(const QString& basePath);
};

}  // namespace nsc
