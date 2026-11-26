#pragma once

#include <QString>
#include <QStringList>

namespace nsc {
namespace Merge {

/**
 * 全局表修复 —— 纯 C++，零外部工具链、零 Python、零游戏根写入。
 *
 * 根因：NSC 社区 mod 把「编译器工作区」原样打包。这些工作区里的
 *   Characters/<id>/data/... 与 Models/<id>/data/... 下是几百字节的**参数表碎片**
 *   （characterSelectParam.xfbin 只有 1~4 条新角色记录）。
 * MO2 的 [VFSContentLayout] layout_markers 含 "data"，于是任意目录下名为 data 的
 *   子目录都会被映射成游戏数据根 —— 这些碎片被铺到 data/ui/max/select/ 等，
 *   直接覆盖原版几十 KB 的整表，引擎按整表 memcpy 时目标越界 → 选人界面/进场闪退。
 *
 * 修复（三步，全部在 MO2 实例内部）：
 *   1. 扫描启用 mod，把工作区 data 目录整体遮蔽（改后缀 .nsc_shadowed），
 *      让 VFS 再也看不到碎片；资产目录 Resources/Files/data 不动。
 *   2. 用已验证的字节级 Roster 算法，把每个碎片里的新角色记录合并进一份
 *      完整基底表（345 条记录 / 121KB）。
 *   3. 合并后的整表写回本地化模组 <实例>/mods/忍者与舞台中文本地化/
 *      ui/max/select/characterSelectParam.xfbin —— 绝不写 overwrite（用户约束：
 *      overwrite 必须全空）。服装碎片（group 命中原版家族）以 page=-1 隐藏槽
 *      挂进原角色时装列表，不再是 p163+ 独立新页。
 *
 * 所有操作幂等：重复调用不会重复追加记录，也不会破坏已遮蔽的目录。
 */

struct Result {
  int shadowedDirs = 0;       ///< 本次新遮蔽的工作区 data 目录数
  int fragmentsMerged = 0;    ///< 合并进整表的碎片数
  int recordsAdded = 0;       ///< 新增的角色记录数
  int recordsTotal = 0;       ///< 合并后整表的总记录数
  int fragmentCandidates = 0; ///< 找到的碎片候选数
  int fragmentRejected = 0;   ///< 解析失败/被排除的碎片数
  int fragmentDuplicates = 0; ///< 角色码已在基底中而跳过的记录数
  int baseRecords = 0;        ///< 实际基底表记录数
  QString baseSource;         ///< 实际基底表来源路径
  QString finalTable;         ///< 写出的整表路径
  QStringList errors;         ///< 非致命错误（单个碎片解析失败等）
};

/**
 * 对当前 profile 里所有启用的 mod 执行修复。
 * basePath: MO2 实例根（含 mods/ profiles/ overwrite/）
 * profile:  当前 profile 名
 * backupBasePath: 备份根（放遮蔽目录，通常为 <实例>/mod-tools/nsc-mo2/backup）
 *              留空则遮蔽目录就地改后缀，不移动。
 */
Result fixEnabledMods(const QString& basePath, const QString& profile,
                      const QString& backupBasePath);

/**
 * 从启用 mod 中挑选一份最完整的 characterSelectParam.xfbin 作为基底。
 * 判据：记录数 >= 100（完整表 345 条，碎片只有 1~4 条）、通过 Roster 自检、
 * 且记录数最多。找不到返回空，由 fixEnabledMods 走 mod-tools 兜底基底。
 */
QString findBestBaseTable(const QString& basePath, const QString& profile);

}  // namespace Merge
}  // namespace nsc
