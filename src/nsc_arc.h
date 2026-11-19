#pragma once

#include <QString>
#include <QStringList>

namespace nsc {

/**
 * 归档解包 —— 直接调用 MO2 自带的 dlls/7z.dll（7-Zip 官方 COM 接口）。
 *
 * 为什么不用 Python：解包是安装/启动路径上最常跑的活，QProcess 起解释器要
 * 200~400 ms，而且一旦用户机器上没有 python 就整条链断掉。7z.dll 是 MO2 的
 * 必备文件（它自己解压 mod 就用这个），所以插件可以零额外依赖地做同样的事。
 *
 * 覆盖格式：zip / 7z / rar —— .nsc/.ensc/.uns/.unse/.nus4 本身就是 zip 容器。
 */
class Archive final
{
public:
  /// 扩展名是否属于可解包的容器。
  static bool isArchive(const QString& fileName);

  /// 解包 archivePath 到 outDir（保留内部相对路径，自动建父目录）。
  /// entries 非空时写入解出的条目（目录以 '/' 结尾）。失败返回 false 并填 error。
  static bool extract(const QString& archivePath, const QString& outDir,
                      QStringList* entries, QString* error);

  /// 解析 7z.dll 位置（找不到返回空串）。
  static QString sevenZipPath();
};

}  // namespace nsc
