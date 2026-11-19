#include "nsc_prepare.h"

#include "nsc_arc.h"
#include "nsc_layout.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>

namespace nsc {
namespace {

const QStringList kSkipDirs = {QStringLiteral("Screenshots"),
                               QStringLiteral(".workbuddy"),
                               QStringLiteral(".git"),
                               QStringLiteral("__pycache__")};

// 映射到游戏根的顶层目录，不当作 data_win32 内容。
const QStringList kRootMapped = {QStringLiteral("moddingapi"),
                                 QStringLiteral("root"),
                                 QStringLiteral("data"),
                                 QStringLiteral("engine")};

bool skipDir(const QString& name)
{
  return kSkipDirs.contains(name, Qt::CaseInsensitive);
}

bool rootMapped(const QString& name)
{
  return kRootMapped.contains(name, Qt::CaseInsensitive);
}

/// 该目录是否是 data_win32 布局标记（mod 用整树映射）。
bool isDataWin32Layout(const QString& modDir)
{
  return QFileInfo(modDir + QStringLiteral("/data_win32")).isDir();
}

/// 列出 mod 里真正属于 data_win32 内容的顶级子目录（相对 mod 根的绝对路径）。
QStringList contentDirs(const QString& modDir)
{
  const bool hasMarker = isDataWin32Layout(modDir);
  const QString root = hasMarker ? modDir + QStringLiteral("/data_win32") : modDir;
  QStringList out;
  const QFileInfoList entries = QDir(root).entryInfoList(
      QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
  for (const QFileInfo& e : entries) {
    if (skipDir(e.fileName())) {
      continue;
    }
    if (!hasMarker && rootMapped(e.fileName())) {
      continue;
    }
    out.append(e.absoluteFilePath());
  }
  return out;
}

/// 把某个相对目录及其所有父目录加进 needed。
void addWithParents(const QString& rel, QSet<QString>& needed)
{
  if (rel.isEmpty() || rel == QStringLiteral(".")) {
    return;
  }
  needed.insert(rel);
  QDir d(rel);
  while (d.cdUp()) {
    const QString p = d.path();
    if (p.isEmpty() || p == QStringLiteral(".") || p == QStringLiteral("..")) {
      break;
    }
    needed.insert(p);
  }
}

}  // namespace

QStringList Prepare::enabledMods(const QString& basePath, const QString& profile)
{
  QStringList out;
  QFile f(basePath + QStringLiteral("/profiles/") + profile +
          QStringLiteral("/modlist.txt"));
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return out;
  }
  const QString text = QString::fromUtf8(f.readAll());
  f.close();
  for (const QString& raw : text.split(QLatin1Char('\n'))) {
    const QString line = raw.trimmed();
    if (line.startsWith(QLatin1Char('+'))) {
      const QString name = line.mid(1).trimmed();
      if (!name.isEmpty()) {
        out.append(name);
      }
    }
  }
  return out;
}

QString Prepare::moddingApiPrefix(const QString& game)
{
  return game.compare(QStringLiteral("NS4"), Qt::CaseInsensitive) == 0
             ? QStringLiteral("_NS4")
             : QStringLiteral("_NSC");
}

bool Prepare::hasOwnCpk(const QString& modDir)
{
  const QString api = modDir + QStringLiteral("/moddingapi/mods");
  if (!QFileInfo(api).isDir()) {
    return false;
  }
  QDirIterator it(api, QStringList{QStringLiteral("*.cpk")},
                  QDir::Files, QDirIterator::Subdirectories);
  return it.hasNext();
}

bool Prepare::isPackage(const QString& modDir)
{
  for (const QString& marker :
       {QStringLiteral("mod_config.ini"), QStringLiteral("Characters"),
        QStringLiteral("Resources"), QStringLiteral("Models")}) {
    const QString p = modDir + QLatin1Char('/') + marker;
    if (QFileInfo::exists(p)) {
      return true;
    }
  }
  // Resources/Files/data 这种三级链在 mod 根只露出 Resources。
  return false;
}

QStringList Prepare::expandNested(const QString& modDir,
                                  const QString& backupRoot)
{
  QStringList done;
  for (int round = 0; round < 3; ++round) {
    QStringList arcs;
    const QFileInfoList entries = QDir(modDir).entryInfoList(
        QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo& e : entries) {
      if (Archive::isArchive(e.fileName())) {
        arcs.append(e.absoluteFilePath());
      }
    }
    if (arcs.isEmpty()) {
      break;
    }

    bool anyOk = false;
    for (const QString& arc : arcs) {
      // 展开到一个临时子目录，避免归档内的相对路径直接砸在 mod 根上产生冲突。
      const QString tmp = modDir + QStringLiteral("/__nsc_expand_") +
                          QFileInfo(arc).completeBaseName();
      QString err;
      if (!Archive::extract(arc, tmp, nullptr, &err)) {
        // 解不开就留着壳体，交给后续流程/日志，不阻塞。
        QDir(tmp).removeRecursively();
        continue;
      }
      // 把展开结果并入 mod 根（nsc::Layout 的合并逻辑），然后删壳体与临时目录。
      Layout::normalizeOne(tmp, backupRoot);
      // 临时目录内容上移一层到 modDir。
      for (const QFileInfo& e : QDir(tmp).entryInfoList(
               QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        const QString target = modDir + QLatin1Char('/') + e.fileName();
        if (e.isDir()) {
          if (QFileInfo(target).isDir()) {
            // 目录合并：逐个文件搬，同名留大的。
            QDirIterator di(e.absoluteFilePath(), QDir::Files,
                            QDirIterator::Subdirectories);
            while (di.hasNext()) {
              const QString src = di.next();
              const QString rel = QDir(e.absoluteFilePath()).relativeFilePath(src);
              const QString dst = target + QLatin1Char('/') + rel;
              QDir().mkpath(QFileInfo(dst).absolutePath());
              if (!QFileInfo::exists(dst) ||
                  QFileInfo(src).size() > QFileInfo(dst).size()) {
                if (QFileInfo::exists(dst)) {
                  QFile::remove(dst);
                }
                QFile::copy(src, dst);
              }
            }
            QDir(e.absoluteFilePath()).removeRecursively();
          } else {
            QDir().rename(e.absoluteFilePath(), target);
          }
        } else {
          if (QFileInfo::exists(target)) {
            QFile::remove(target);
          }
          QFile::rename(e.absoluteFilePath(), target);
        }
      }
      QDir(tmp).removeRecursively();
      QFile::remove(arc);
      done.append(QFileInfo(arc).fileName());
      anyOk = true;
    }
    if (!anyOk) {
      break;
    }
  }
  return done;
}

QSet<QString> Prepare::requiredDirs(const QString& basePath, const QString& profile,
                                   const QString& game,
                                   const QHash<QString, QString>& routing)
{
  QSet<QString> needed;
  needed.insert(QStringLiteral("spc"));  // bare_destination 兜底

  const QString prefix = moddingApiPrefix(game);
  for (const QString& name : enabledMods(basePath, profile)) {
    if (name.startsWith(prefix)) {
      continue;
    }
    const QString modDir = basePath + QStringLiteral("/mods/") + name;
    if (!QFileInfo(modDir).isDir()) {
      continue;
    }
    for (const QString& dir : contentDirs(modDir)) {
      // 内容根本身相对 data_win32 的路径。
      const QString rel = isDataWin32Layout(modDir)
                              ? QDir(modDir + QStringLiteral("/data_win32"))
                                    .relativeFilePath(dir)
                              : QDir(modDir).relativeFilePath(dir);
      addWithParents(rel, needed);

      // 该目录下所有文件的祖先目录 + 路由目标。
      QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
      while (it.hasNext()) {
        const QString file = it.next();
        const QString relParent = isDataWin32Layout(modDir)
                                      ? QDir(modDir + QStringLiteral("/data_win32"))
                                            .relativeFilePath(QFileInfo(file).absolutePath())
                                      : QDir(modDir).relativeFilePath(
                                            QFileInfo(file).absolutePath());
        addWithParents(relParent, needed);
        const QString routed = routing.value(QFileInfo(file).fileName());
        if (!routed.isEmpty()) {
          addWithParents(routed, needed);
        }
      }
    }
  }

  // overwrite 是 VFS 最高优先级虚拟 mod，里面文件已是 data_win32 相对布局。
  const QString ow = basePath + QStringLiteral("/overwrite");
  if (QFileInfo(ow).isDir()) {
    QDirIterator it(ow, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
      const QString file = it.next();
      const QString rel = QDir(ow).relativeFilePath(QFileInfo(file).absolutePath());
      addWithParents(rel, needed);
      const QString routed = routing.value(QFileInfo(file).fileName());
      if (!routed.isEmpty()) {
        addWithParents(routed, needed);
      }
    }
  }

  needed.remove(QString());
  needed.remove(QStringLiteral("."));
  return needed;
}

int Prepare::createDirs(const QString& gameRoot, const QSet<QString>& needed)
{
  const QString layer = gameRoot + QStringLiteral("/data_win32");
  int created = 0;
  for (const QString& rel : needed) {
    const QString d = layer + QLatin1Char('/') + rel;
    if (QFileInfo(d).isDir()) {
      continue;
    }
    if (QDir().mkpath(d)) {
      ++created;
    }
  }
  return created;
}

}  // namespace nsc
