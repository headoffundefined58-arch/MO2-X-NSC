#pragma once

#include <QString>
#include <QStringList>

namespace nsc {

/**
 * 社区模组的最后一道归一化 —— 构建在 Layout / Prepare 之上，专门处理实测中
 * 让 Sage Kabuto、九尾之力等工作失败的两件事：
 *
 *   1) Resources/Files/data/<内容> 是 NSC ModManager 的工作区路径。即使 mod
 *      同时带 Characters/Models（旧逻辑会把它当“编译器包”原样保留），MO2 分支
 *      也只认 mod 根 == 游戏 data，所以这一层必须无条件展平。
 *   2) 部分 .xfbin/.bin 是加密 NUCC（ba4f0ae8 / bc0a8973 / 26c30b2f）。
 *      已逆向出 EncryptCPK.dll 的 4 组 32 位状态 stream cipher，插件原生解密，
 *      不依赖任何外部 DLL/运行时。
 *
 * 全部操作幂等：已经展平的目录不会再动，明文 payload 不会被改写。
 * 冲突和解密失败的文件移到 backupRoot，绝不硬删。
 */
class Payload final
{
public:
  /// 无条件把任意深度的 Resources/Files/data 内容并入其父目录（通常是 mod 根）。
  /// 返回人类可读的动作清单。
  static QStringList normalizeAssets(const QString& modDir, const QString& backupRoot);

  /// 用内建的 4 组密钥流解密 mod 内所有加密 NUCC payload。
  /// 返回成功解密的文件数；动作写入 actions。
  static int decryptEncrypted(const QString& modDir, const QString& backupRoot,
                              QStringList& actions);
};

}  // namespace nsc
