#include "nsc_payload.h"

#include <QByteArray>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSet>
#include <QtGlobal>

namespace nsc {
namespace {

// EncryptCPK.dll!DecryptOrSkip 内建 4 组 32 位状态。状态顺序已按反汇编修正：
// 循环里先用 r9(arg4) 作当前态、r8(arg3) 作下一态，再轮换 arg5/arg6。
// 已对 Sage Kabuto 的全部 5 个 payload 做整文件逐字节比对，完全等同原 DLL。
struct CipherState {
  quint32 a, b, c, d;
};

const CipherState kStates[] = {
    {0x154f048dU, 0x02b3d1cdU, 0x43a3d2f9U, 0x617e9602U},
    {0x19308803U, 0x020efee5U, 0x140d7cc7U, 0xf1a568cfU},
    {0x5d744e4bU, 0x796c2041U, 0x386dacaeU, 0x54b0003fU},
    {0x57b86023U, 0x5c53b2c9U, 0x42eef6a6U, 0xa4e78a35U},
};

bool isEncryptedNucc(const QByteArray& in)
{
  static const QByteArray magics[] = {
      QByteArray::fromHex("ba4f0ae8"),
      QByteArray::fromHex("bc0a8973"),
      QByteArray::fromHex("26c30b2f")};
  for (const QByteArray& m : magics) {
    if (in.startsWith(m)) {
      return true;
    }
  }
  return false;
}

/// 返回解密后的内容；不是已知加密头或 4 组密钥都解不出时返回空。
QByteArray decryptPayload(const QByteArray& in)
{
  if (!isEncryptedNucc(in)) {
    return QByteArray();
  }

  for (const CipherState& s : kStates) {
    QByteArray out(in.size(), Qt::Uninitialized);
    quint32 a = s.a, b = s.b, c = s.c, d = s.d;
    for (int i = 0; i < in.size(); i += 4) {
      const quint32 x = (a << 11) ^ a;
      quint32 y = (d >> 11) ^ x;
      y = (y >> 8) ^ x;
      a = b;
      b = c;
      c = d;
      d = d ^ y;

      const quint32 key = d;
      const int n = qMin(4, in.size() - i);
      for (int j = 0; j < n; ++j) {
        out[i + j] = static_cast<char>(
            (static_cast<quint8>(in[i + j]) ^
             static_cast<quint8>(key >> (8 * j))));
      }
    }
    if (out.startsWith(QByteArrayLiteral("NUCC")) ||
        out.startsWith(QByteArrayLiteral("CRI "))) {
      return out;
    }
  }
  return QByteArray();
}

const QSet<QString>& reservedNames()
{
  static const QSet<QString> names{QStringLiteral(".workbuddy"),
                                   QStringLiteral("__pycache__"),
                                   QStringLiteral("backup"),
                                   QStringLiteral("_trash")};
  return names;
}

bool isReserved(const QString& name)
{
  return reservedNames().contains(name.toLower());
}

QString childDir(const QString& parent, const QString& wanted)
{
  const QFileInfoList subs = QDir(parent).entryInfoList(
      QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
  for (const QFileInfo& e : subs) {
    if (e.fileName().compare(wanted, Qt::CaseInsensitive) == 0) {
      return e.absoluteFilePath();
    }
  }
  return QString();
}

bool dirIsEmpty(const QString& dir)
{
  return QDir(dir)
      .entryList(QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)
      .isEmpty();
}

void pruneEmpty(QString dir, const QString& stop)
{
  while (!dir.isEmpty() && dir != stop && QFileInfo(dir).absolutePath() != dir) {
    if (!dirIsEmpty(dir) || !QDir().rmdir(dir)) {
      break;
    }
    dir = QFileInfo(dir).absolutePath();
  }
}

QString uniqueBackupPath(const QString& dir, const QString& fileName)
{
  QDir().mkpath(dir);
  const QString first = dir + QLatin1Char('/') + fileName;
  if (!QFileInfo::exists(first)) {
    return first;
  }
  const QFileInfo info(fileName);
  for (int i = 1; i < 1000; ++i) {
    const QString candidate = dir + QLatin1Char('/') +
        QStringLiteral("%1__%2%3")
            .arg(info.completeBaseName())
            .arg(i)
            .arg(info.suffix().isEmpty() ? QString() : QLatin1Char('.') + info.suffix());
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

void stash(const QString& path, const QString& backupRoot, QStringList& actions)
{
  const QString target = uniqueBackupPath(
      backupRoot, QFileInfo(path).fileName());
  if (moveFile(path, target)) {
    actions.append(QStringLiteral("backup ") + QDir::fromNativeSeparators(target));
  }
}

/// 把 src 的内容递归并入 dst；同名文件保留体积大的，另一方进备份区。
void mergeDir(const QString& src, const QString& dst, const QString& backupRoot,
              QStringList& actions)
{
  QDir().mkpath(dst);
  const QFileInfoList entries = QDir(src).entryInfoList(
      QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
      QDir::Name);
  for (const QFileInfo& e : entries) {
    if (isReserved(e.fileName())) {
      continue;
    }
    const QString target = dst + QLatin1Char('/') + e.fileName();
    if (e.isDir()) {
      mergeDir(e.absoluteFilePath(), target,
               backupRoot + QLatin1Char('/') + e.fileName(), actions);
      pruneEmpty(e.absoluteFilePath(), src);
      continue;
    }

    if (!QFileInfo::exists(target)) {
      if (moveFile(e.absoluteFilePath(), target)) {
        actions.append(QStringLiteral("lift ") + e.fileName());
      }
      continue;
    }

    const bool keepNew = e.size() > QFileInfo(target).size();
    const QString loser = keepNew ? target : e.absoluteFilePath();
    stash(loser, backupRoot, actions);
    if (keepNew) {
      moveFile(e.absoluteFilePath(), target);
    }
    actions.append(QStringLiteral("conflict ") + e.fileName() +
                   (keepNew ? QStringLiteral(": keep new")
                            : QStringLiteral(": keep existing")));
  }
}

struct AssetChain {
  QString data;
  QString targetParent;
};

bool findAssetChain(const QString& modDir, AssetChain& out)
{
  const QFileInfoList subs = QDir(modDir).entryInfoList(
      QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
  for (const QFileInfo& res : subs) {
    if (isReserved(res.fileName())) {
      continue;
    }
    const QString files = childDir(res.absoluteFilePath(), QStringLiteral("Files"));
    if (files.isEmpty()) {
      continue;
    }
    const QString data = childDir(files, QStringLiteral("data"));
    if (data.isEmpty()) {
      continue;
    }
    out.data = data;
    out.targetParent = res.absolutePath();
    return true;
  }
  return false;
}

}  // namespace

QStringList Payload::normalizeAssets(const QString& modDir, const QString& backupRoot)
{
  QStringList actions;
  if (!QFileInfo(modDir).isDir()) {
    return actions;
  }

  for (int round = 0; round < 8; ++round) {
    AssetChain chain;
    if (!findAssetChain(modDir, chain)) {
      break;
    }
    actions.append(QStringLiteral("flatten ") +
                   QDir(modDir).relativeFilePath(chain.data) +
                   QStringLiteral("/ -> mod root"));
    mergeDir(chain.data, chain.targetParent,
             backupRoot + QLatin1Char('/') +
                 QDir(modDir).relativeFilePath(chain.data),
             actions);

    // 清掉被抽空的 Resources/Files/data 链条；非空目录（CPKs/Shaders）保留。
    pruneEmpty(chain.data, modDir);
    const QString files = QFileInfo(chain.data).absolutePath();
    pruneEmpty(files, modDir);
    const QString resources = QFileInfo(files).absolutePath();
    pruneEmpty(resources, modDir);
  }
  return actions;
}

int Payload::decryptEncrypted(const QString& modDir, const QString& backupRoot,
                              QStringList& actions)
{
  int decrypted = 0;
  QDirIterator it(modDir,
                  QStringList{QStringLiteral("*.xfbin"), QStringLiteral("*.bin")},
                  QDir::Files | QDir::Hidden | QDir::System,
                  QDirIterator::Subdirectories);
  while (it.hasNext()) {
    const QString path = it.next();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
      continue;
    }
    const QByteArray in = f.readAll();
    f.close();

    const QByteArray out = decryptPayload(in);
    if (out.isEmpty()) {
      if (isEncryptedNucc(in)) {
        stash(path, backupRoot + QLatin1Char('/') +
                        QDir(modDir).relativeFilePath(QFileInfo(path).absolutePath()),
              actions);
        actions.append(QStringLiteral("decrypt FAIL ") +
                       QDir(modDir).relativeFilePath(path));
      }
      continue;
    }

    QSaveFile sf(path);
    if (!sf.open(QIODevice::WriteOnly) || sf.write(out) != out.size() ||
        !sf.commit()) {
      actions.append(QStringLiteral("decrypt WRITE FAIL ") +
                     QDir(modDir).relativeFilePath(path));
      continue;
    }
    ++decrypted;
    actions.append(QStringLiteral("decrypt ") +
                   QDir(modDir).relativeFilePath(path) + QStringLiteral(" -> NUCC"));
  }
  return decrypted;
}

}  // namespace nsc
