#include "nsc_layout.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

namespace nsc {
namespace {

const QStringList kReserved = {QStringLiteral(".workbuddy"),
                               QStringLiteral("__pycache__"),
                               QStringLiteral("backup"),
                               QStringLiteral("_trash")};

const QStringList kMetaFiles = {QStringLiteral("nsc_mod.json"),
                                QStringLiteral("mod_config.ini"),
                                QStringLiteral("mod_icon.png"),
                                QStringLiteral(".nsc_compiled"),
                                QStringLiteral("meta.ini"),
                                QStringLiteral("readme.txt")};

// 带这些目录说明是「给编译器用的 NSC 包工作区」，布局必须原样保留。
const QStringList kPackageMarkers = {QStringLiteral("Models"),
                                     QStringLiteral("Characters"),
                                     QStringLiteral("moddingapi")};

// 只有一个 data_win32 包壳时才整体上移；data/ 与 root/ 是 layout marker
// （映射到游戏根），拆了反而让整片文件读不到。
const QStringList kWrappers = {QStringLiteral("data_win32")};

// NSC_ModManager 的资源壳：Resources/Files/data/<内容目录>
const QStringList kAssetChain = {QStringLiteral("Resources"), QStringLiteral("Files"),
                                 QStringLiteral("data")};

bool isReserved(const QString& name)
{
  return kReserved.contains(name, Qt::CaseInsensitive);
}

QString joinPath(const QString& left, const QString& right)
{
  return left + QLatin1Char('/') + right;
}

QString uniqueTarget(const QString& dir, const QString& fileName)
{
  const QString first = joinPath(dir, fileName);
  if (!QFileInfo::exists(first)) {
    return first;
  }
  const QFileInfo info(fileName);
  const QString base = info.completeBaseName();
  const QString suffix =
      info.suffix().isEmpty() ? QString() : QLatin1Char('.') + info.suffix();
  for (int i = 1; i < 1000; ++i) {
    const QString candidate = joinPath(
        dir, QStringLiteral("%1__%2%3").arg(base).arg(i).arg(suffix));
    if (!QFileInfo::exists(candidate)) {
      return candidate;
    }
  }
  return first;
}

bool moveFile(const QString& from, const QString& to)
{
  if (QFile::rename(from, to)) {
    return true;
  }
  if (!QFile::copy(from, to)) {
    return false;
  }
  return QFile::remove(from);
}

/// 把冲突的那一份挪进备份区（绝不硬删）。返回是否成功。
bool stashPath(const QString& path, const QString& stashDir)
{
  if (!QFileInfo::exists(path)) {
    return false;
  }
  if (!QDir().mkpath(stashDir)) {
    return false;
  }
  return moveFile(path, uniqueTarget(stashDir, QFileInfo(path).fileName()));
}

bool hasPackageMarker(const QString& dir)
{
  for (const QString& marker : kPackageMarkers) {
    if (QFileInfo(joinPath(dir, marker)).isDir()) {
      return true;
    }
  }
  return false;
}

/// 把 srcDir 的内容并入 dstDir（同名子目录递归合并），返回搬运的文件数。
int mergeUp(const QString& srcDir, const QString& dstDir, const QString& stashDir,
            QStringList& actions)
{
  int moved = 0;
  const QFileInfoList entries =
      QDir(srcDir).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                                 QDir::Name);
  for (const QFileInfo& entry : entries) {
    if (isReserved(entry.fileName())) {
      continue;
    }
    const QString target = joinPath(dstDir, entry.fileName());

    if (entry.isDir()) {
      if (QFileInfo(target).isDir()) {
        moved += mergeUp(entry.absoluteFilePath(), target,
                         joinPath(stashDir, entry.fileName()), actions);
        QDir().rmdir(entry.absoluteFilePath());
        continue;
      }
      if (QFileInfo::exists(target)) {
        actions.append(QStringLiteral("conflict ") + entry.fileName() +
                       QStringLiteral(": dir vs file, keep file"));
        stashPath(entry.absoluteFilePath(), stashDir);
        continue;
      }
      actions.append(QStringLiteral("lift ") + entry.fileName() + QLatin1Char('/'));
      if (QDir().rename(entry.absoluteFilePath(), target)) {
        ++moved;
      }
      continue;
    }

    ++moved;
    if (!QFileInfo::exists(target)) {
      actions.append(QStringLiteral("lift ") + entry.fileName());
      QDir().mkpath(dstDir);
      moveFile(entry.absoluteFilePath(), target);
      continue;
    }

    const qint64 newSize = entry.size();
    const qint64 oldSize = QFileInfo(target).size();
    const bool keepNew = newSize > oldSize;
    actions.append(QStringLiteral("conflict ") + entry.fileName() +
                   (keepNew ? QStringLiteral(": keep new") : QStringLiteral(": keep existing")));
    const QString loser = keepNew ? target : entry.absoluteFilePath();
    if (stashPath(loser, stashDir) && keepNew) {
      moveFile(entry.absoluteFilePath(), target);
    }
  }
  return moved;
}

void collectDirs(const QString& root, QStringList& out)
{
  const QFileInfoList subs =
      QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
  for (const QFileInfo& sub : subs) {
    if (isReserved(sub.fileName())) {
      continue;
    }
    out.append(sub.absoluteFilePath());
    collectDirs(sub.absoluteFilePath(), out);
  }
}

/// 递归查找任意深度的 data_win32 包壳（用户明确要求：压缩包里
/// naruto/data_win32/spc，安装后只要 spc）。返回相对 mod 根最浅的那个。
QString findDataWin32Wrapper(const QString& modDir)
{
  QStringList dirs;
  collectDirs(modDir, dirs);
  std::sort(dirs.begin(), dirs.end(), [](const QString& a, const QString& b) {
    return a.count(QLatin1Char('/')) < b.count(QLatin1Char('/'));
  });
  for (const QString& dir : dirs) {
    if (QFileInfo(dir).fileName().compare(QStringLiteral("data_win32"),
                                          Qt::CaseInsensitive) == 0) {
      return dir;
    }
  }
  return QString();
}

void removeEmptyTree(const QString& path)
{
  const QFileInfoList subs =
      QDir(path).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
  for (const QFileInfo& sub : subs) {
    removeEmptyTree(sub.absoluteFilePath());
  }
  const QStringList files = QDir(path).entryList(
      QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
  if (files.isEmpty()) {
    QDir().rmdir(path);
  }
}

/// 规则 1：X/X 折叠成一层。
int collapseDuplicate(const QString& modDir, const QString& stashDir,
                      QStringList& actions)
{
  int moved = 0;
  for (int round = 0; round < 8; ++round) {
    QStringList dirs;
    collectDirs(modDir, dirs);
    std::sort(dirs.begin(), dirs.end(), [](const QString& a, const QString& b) {
      return a.count(QLatin1Char('/')) > b.count(QLatin1Char('/'));
    });

    QString hit;
    for (const QString& dir : dirs) {
      const QString parent = QFileInfo(dir).absolutePath();
      if (parent == modDir) {
        continue;  // mod 根这一层由规则 2 负责
      }
      if (QFileInfo(dir).fileName().compare(QFileInfo(parent).fileName(),
                                            Qt::CaseInsensitive) == 0) {
        hit = dir;
        break;
      }
    }
    if (hit.isEmpty()) {
      break;
    }

    const QString rel = QDir(modDir).relativeFilePath(hit);
    actions.append(QStringLiteral("collapse ") + rel + QStringLiteral("/"));
    const QString parent = QFileInfo(hit).absolutePath();
    moved += mergeUp(hit, parent, joinPath(stashDir, rel), actions);
    removeEmptyTree(hit);
  }
  return moved;
}

/// 规则 2：包壳整体上移。
int stripWrapper(const QString& modDir, const QString& stashDir,
                 QStringList& actions)
{
  int moved = 0;
  for (int round = 0; round < 6; ++round) {
    QStringList tops;
    for (const QFileInfo& sub :
         QDir(modDir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
      if (!isReserved(sub.fileName())) {
        tops.append(sub.absoluteFilePath());
      }
    }
    if (tops.isEmpty()) {
      break;
    }

    // 每轮重算：剥掉外层之后结构可能刚变成「NSC 包」，那时 Resources 不能动。
    const bool isPackage = Layout::looksLikeNscPackage(modDir);
    QString candidate;

    if (tops.size() == 1) {
      const QFileInfo only(tops.first());
      if (kWrappers.contains(only.fileName(), Qt::CaseInsensitive)) {
        candidate = tops.first();
      } else if (hasPackageMarker(tops.first())) {
        // zip 里再套一层同名目录：X/X/{Characters,Resources}
        candidate = tops.first();
      }
    }
    if (candidate.isEmpty()) {
      for (const QString& top : tops) {
        if (QFileInfo(top).fileName().compare(QStringLiteral("data_win32"),
                                              Qt::CaseInsensitive) == 0) {
          candidate = top;  // 多个顶层里混着 data_win32：只上移它
          break;
        }
      }
    }
    if (candidate.isEmpty() && !isPackage) {
      // 纯资源包：Resources/Files/data/<内容> 整体提到 mod 根
      QString chain = modDir;
      bool complete = true;
      for (const QString& segment : kAssetChain) {
        QString next;
        for (const QFileInfo& sub : QDir(chain).entryInfoList(
                 QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
          if (sub.fileName().compare(segment, Qt::CaseInsensitive) == 0) {
            next = sub.absoluteFilePath();
            break;
          }
        }
        if (next.isEmpty()) {
          complete = false;
          break;
        }
        chain = next;
      }
      if (complete &&
          !QDir(chain).entryList(QDir::NoDotAndDotDot | QDir::Hidden).isEmpty()) {
        candidate = chain;
      }
    }
    if (candidate.isEmpty() && !isPackage) {
      // 用户明确要求：头文件夹嵌套的 data_win32 也要剥。
      // 例如压缩包是 naruto/data_win32/spc，安装后只留 spc（data_win32 里的内容
      // 直接提到 mod 根）。上面的 kAssetChain 只认 Resources/Files/data，覆盖
      // 不到「xxx/data_win32/」这种包壳，这里递归兜底。
      const QString nested = findDataWin32Wrapper(modDir);
      if (!nested.isEmpty() &&
          !QDir(nested).entryList(QDir::NoDotAndDotDot | QDir::Hidden).isEmpty()) {
        candidate = nested;
      }
    }
    if (candidate.isEmpty()) {
      break;
    }
    if (QDir(candidate).entryList(QDir::NoDotAndDotDot | QDir::Hidden).isEmpty()) {
      break;  // 空壳不动
    }

    const QString rel = QDir(modDir).relativeFilePath(candidate);
    actions.append(QStringLiteral("strip wrapper ") + rel + QStringLiteral("/"));
    moved += mergeUp(candidate, modDir, joinPath(stashDir, rel), actions);

    // 清掉被抽空的壳目录链（Resources/Files/data 整条）
    QString dir = candidate;
    while (!dir.isEmpty() && dir != modDir) {
      removeEmptyTree(dir);
      const QString parent = QFileInfo(dir).absolutePath();
      if (parent == dir) {
        break;
      }
      dir = parent;
    }
  }
  return moved;
}

/// 规则 3：根下散落的 *.xfbin 按路由表归位。
int routeStray(const QString& modDir, const QHash<QString, QString>& routing,
               const QString& stashDir, QStringList& actions)
{
  int n = 0;
  const QFileInfoList files =
      QDir(modDir).entryInfoList(QStringList{QStringLiteral("*.xfbin")},
                                 QDir::Files, QDir::Name);
  for (const QFileInfo& file : files) {
    if (kMetaFiles.contains(file.fileName(), Qt::CaseInsensitive)) {
      continue;
    }
    const QString sub = routing.value(file.fileName(), QStringLiteral("spc"));
    const QString targetDir = joinPath(modDir, sub);
    const QString target = joinPath(targetDir, file.fileName());
    actions.append(QStringLiteral("route ") + file.fileName() + QStringLiteral(" -> ") +
                   sub + QLatin1Char('/'));
    ++n;
    QDir().mkpath(targetDir);
    if (QFileInfo::exists(target)) {
      if (file.size() <= QFileInfo(target).size()) {
        stashPath(file.absoluteFilePath(), joinPath(stashDir, sub));
        continue;
      }
      stashPath(target, joinPath(stashDir, sub));
    }
    moveFile(file.absoluteFilePath(), target);
  }
  return n;
}

}  // namespace

bool Layout::looksLikeNscPackage(const QString& modDir)
{
  return hasPackageMarker(modDir);
}

QStringList Layout::normalizeOne(const QString& modDir, const QString& backupRoot)
{
  QStringList actions;
  const QFileInfo info(modDir);
  if (!info.isDir()) {
    return actions;
  }
  const QString stashDir = joinPath(backupRoot, info.fileName());
  stripWrapper(modDir, stashDir, actions);
  collapseDuplicate(modDir, stashDir, actions);
  routeStray(modDir, {}, stashDir, actions);
  return actions;
}

QStringList Layout::normalizeProfile(const QString& basePath, const QString& profile,
                                     const QString& backupRoot)
{
  QStringList all;
  QFile file(basePath + QStringLiteral("/profiles/") + profile +
             QStringLiteral("/modlist.txt"));
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return all;
  }
  const QString text = QString::fromUtf8(file.readAll());
  file.close();

  for (const QString& raw : text.split(QLatin1Char('\n'))) {
    const QString line = raw.trimmed();
    if (!line.startsWith(QLatin1Char('+'))) {
      continue;
    }
    const QString name = line.mid(1).trimmed();
    if (name.isEmpty()) {
      continue;
    }
    const QString modDir = basePath + QStringLiteral("/mods/") + name;
    if (!QFileInfo(modDir).isDir()) {
      continue;
    }
    all.append(normalizeOne(modDir, backupRoot));
  }
  return all;
}

QHash<QString, QString> Layout::readRouting(const QString& basePath)
{
  QHash<QString, QString> routing;
  QFile file(basePath + QStringLiteral("/nxb_win32_routing.json"));
  if (!file.open(QIODevice::ReadOnly)) {
    return routing;
  }
  const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
  file.close();
  if (!doc.isObject()) {
    return routing;
  }
  const QJsonObject obj = doc.object();
  for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
    if (it.value().isString()) {
      routing.insert(it.key(), it.value().toString());
    }
  }
  return routing;
}

}  // namespace nsc
