#include "nsc_merge.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPair>
#include <QStringList>
#include <QTextStream>
#include <QResource>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <map>
#include <set>
#include <vector>

namespace nsc {
namespace Merge {

// ---------------------------------------------------------------------------
// NUCC 参数表字节布局（Naruto Storm Connections 1.71，已在 Python 原型上验证）：
//   file header            0x000 .. 0x120
//   records                0x120 .. 0x120 + N*320        N at 0x134 (LE)
//   string header          32 bytes       (length at +0x10)
//   string groups          8-byte aligned slots, one per record, in order
//   trailer                20 bytes
// 每条记录的 +32 是它的组指针 f32 = data_off - 320*(r+1) + rel[r]。
// 追加记录会平移 data_off，所有旧记录的 +16/+32/+56/+72 必须同步平移，
// 否则引擎读记录数据时越界 —— 这正是碎片直接铺进 VFS 时的闪退机理。
// ---------------------------------------------------------------------------
namespace {

constexpr int REC_START = 0x120;
constexpr int REC_SIZE = 320;
constexpr int COUNT_OFF = 0x134;
constexpr int REGION_OFF = 0x140;
constexpr int STR_HDR = 0x20;
constexpr int TRAILER = 20;
constexpr int ALIGN = 8;

// 完整 NUCC 选人表有几百条记录；编译器工作区碎片只有 1~4 条。
// 没有这个门槛时，碎片会被误选成基底，再把 4 条记录写进 overwrite，
// 游戏选人界面按整表解析就越界闪退。
constexpr int MIN_BASE_RECORDS = 100;

constexpr int F_ACTIVE = 16;
constexpr int F_PAGE = 24;
constexpr int F_DATA = 32;
constexpr int F_GROUP = 44;
constexpr int F_SLOT = 48;
constexpr int F_56 = 56;
constexpr int F_72 = 72;

quint32 u32(const QByteArray& b, int off)
{
  if (off < 0 || off + 4 > b.size()) {
    return 0;
  }
  quint32 v = 0;
  std::memcpy(&v, b.constData() + off, 4);
  return v;
}

qint32 i32(const QByteArray& b, int off)
{
  return static_cast<qint32>(u32(b, off));
}

void put32(QByteArray& b, int off, quint32 v)
{
  if (off < 0 || off + 4 > b.size()) {
    return;
  }
  std::memcpy(b.data() + off, &v, 4);
}

void putBE32(QByteArray& b, int off, quint32 v)
{
  if (off < 0 || off + 4 > b.size()) {
    return;
  }
  const uchar p[4] = {static_cast<uchar>(v >> 24), static_cast<uchar>(v >> 16),
                      static_cast<uchar>(v >> 8), static_cast<uchar>(v)};
  std::memcpy(b.data() + off, p, 4);
}

/// 8 字节对齐的字符串组：第 0 个槽固定 8 字节，其余按字符串长度对齐到 8。
QByteArray buildGroup(const std::vector<QByteArray>& strings)
{
  QByteArray out;
  for (size_t i = 0; i < strings.size(); ++i) {
    const QByteArray& raw = strings[i];
    int width;
    if (i == 0) {
      width = 8;
    } else {
      width = std::max(16, static_cast<int>((raw.size() + 1 + ALIGN - 1) / ALIGN) * ALIGN);
    }
    if (raw.size() + 1 > width) {
      return QByteArray();  // 放不下：调用方跳过这个碎片
    }
    out.append(QByteArray(width, '\0'));
    std::memcpy(out.data() + out.size() - width, raw.constData(), raw.size());
  }
  return out;
}

struct FragRecord {
  QByteArray record;              // 320 字节
  std::vector<QByteArray> strings; // 该记录的 NUCC 字符串组
  // 碎片作者已经排好的选人表位置。以前全部被覆盖成 page=0/slot=0，
  // 多个新角色叠在同一格，游戏里表现为"不生效"。必须保留并整体重映射。
  qint32 page = -1;
  quint32 slot = 0;
  quint32 group = 0;
  QString source;                 // 来自哪个碎片；跨模组 page/group 必须分开映射
};

struct ParsedFrag {
  std::vector<FragRecord> records;
};

/// 解析一个碎片文件：返回全部记录 + 各自的字符串组。
/// 组边界用记录自己的 +32 指针推导，不靠字符串形状猜测（已在 89/386 组样本上验证）。
bool parseFragment(const QString& path, ParsedFrag& out, QString* err)
{
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) {
    if (err) *err = QStringLiteral("cannot open fragment");
    return false;
  }
  const QByteArray b = f.readAll();
  f.close();

  if (b.size() < REC_START + STR_HDR) {
    if (err) *err = QStringLiteral("fragment too small");
    return false;
  }
  const quint32 n = u32(b, COUNT_OFF);
  const quint32 region = u32(b, REGION_OFF);
  if (n == 0 || n > 4096 || region != n * REC_SIZE) {
    if (err) *err = QStringLiteral("fragment region mismatch");
    return false;
  }
  const int strHdr = REC_START + static_cast<int>(region);
  if (strHdr + STR_HDR > b.size()) {
    if (err) *err = QStringLiteral("fragment string header out of range");
    return false;
  }
  const quint32 strLen = u32(b, strHdr + 0x10);
  const int dataOff = strHdr + STR_HDR;
  if (dataOff + static_cast<int>(strLen) > b.size()) {
    if (err) *err = QStringLiteral("fragment string data out of range");
    return false;
  }
  const QByteArray data = b.mid(dataOff, static_cast<int>(strLen));

  // 每条记录的组相对偏移
  std::vector<int> rels;
  rels.reserve(n);
  for (quint32 r = 0; r < n; ++r) {
    const quint32 f32 = u32(b, REC_START + static_cast<int>(r) * REC_SIZE + F_DATA);
    const qint64 rel = static_cast<qint64>(f32) +
                       static_cast<qint64>(REC_SIZE) * (r + 1) - dataOff;
    if (rel < 0 || rel > static_cast<qint64>(data.size())) {
      if (err) *err = QStringLiteral("fragment group offset out of range");
      return false;
    }
    rels.push_back(static_cast<int>(rel));
  }
  // 排序后的下一个组起点 = 本组结束
  std::set<int> order(rels.begin(), rels.end());
  std::map<int, int> nextOf;
  for (auto it = order.begin(); it != order.end(); ++it) {
    auto nx = std::next(it);
    nextOf[*it] = (nx == order.end()) ? data.size() : *nx;
  }

  for (quint32 r = 0; r < n; ++r) {
    FragRecord fr;
    const int recOff = REC_START + static_cast<int>(r) * REC_SIZE;
    fr.record = b.mid(recOff, REC_SIZE);
    fr.page = i32(b, recOff + F_PAGE);
    fr.slot = u32(b, recOff + F_SLOT);
    fr.group = u32(b, recOff + F_GROUP);
    fr.source = path;
    const int start = rels[r];
    const int end = nextOf[start];
    const QByteArray group = data.mid(start, end - start);
    for (const QByteArray& tok : group.split('\0')) {
      if (!tok.isEmpty()) {
        fr.strings.push_back(tok);
      }
    }
    if (fr.strings.empty()) {
      if (err) *err = QStringLiteral("fragment has empty group");
      return false;
    }
    out.records.push_back(std::move(fr));
  }
  return true;
}

struct BaseTable {
  QByteArray raw;
  quint32 count = 0;
  int strHdr = 0;
  int dataOff = 0;
  QByteArray data;
  QByteArray trailer;
  std::vector<int> rels;  // 每条记录的组相对偏移
};

/// 解析并自检一份完整基底表。
bool parseBase(const QByteArray& raw, BaseTable& out, QString* err)
{
  if (raw.size() < REC_START + STR_HDR + TRAILER) {
    if (err) *err = QStringLiteral("base too small");
    return false;
  }
  const quint32 n = u32(raw, COUNT_OFF);
  const quint32 region = u32(raw, REGION_OFF);
  if (n == 0 || n > 4096 || region != n * REC_SIZE) {
    if (err) *err = QStringLiteral("base region mismatch");
    return false;
  }
  const int strHdr = REC_START + static_cast<int>(region);
  if (strHdr + STR_HDR > raw.size()) {
    if (err) *err = QStringLiteral("base string header out of range");
    return false;
  }
  const quint32 strLen = u32(raw, strHdr + 0x10);
  const int dataOff = strHdr + STR_HDR;
  if (strLen == 0 || dataOff + static_cast<int>(strLen) > raw.size()) {
    if (err) *err = QStringLiteral("base string data out of range");
    return false;
  }
  out.raw = raw;
  out.count = n;
  out.strHdr = strHdr;
  out.dataOff = dataOff;
  out.data = raw.mid(dataOff, static_cast<int>(strLen));
  out.trailer = raw.mid(dataOff + static_cast<int>(strLen));

  out.rels.reserve(n);
  for (quint32 r = 0; r < n; ++r) {
    const quint32 f32 = u32(raw, REC_START + static_cast<int>(r) * REC_SIZE + F_DATA);
    const qint64 rel = static_cast<qint64>(f32) +
                       static_cast<qint64>(REC_SIZE) * (r + 1) - dataOff;
    if (rel < 0 || rel >= static_cast<qint64>(out.data.size())) {
      if (err) *err = QStringLiteral("base f32 self-check failed");
      return false;
    }
    out.rels.push_back(static_cast<int>(rel));
  }
  return true;
}

/// 角色码家族键：前 4 字符（5obt_01 -> "5obt"，5obt00 -> "5obt"）。
QByteArray familyKey(const QByteArray& code)
{
  return code.left(4).toLower();
}

/// 每个原版 group id 下的角色码集合，用于判断碎片是否真属于那个家族。
std::map<quint32, std::set<QByteArray>> baseGroupCodes(const BaseTable& base)
{
  std::map<quint32, std::set<QByteArray>> out;
  std::set<int> order(base.rels.begin(), base.rels.end());
  std::map<int, int> nextOf;
  for (auto it = order.begin(); it != order.end(); ++it) {
    auto nx = std::next(it);
    nextOf[*it] = (nx == order.end()) ? base.data.size() : *nx;
  }
  for (quint32 r = 0; r < base.count; ++r) {
    const int start = base.rels[r];
    const QByteArray group = base.data.mid(start, nextOf[start] - start);
    const QList<QByteArray> toks = group.split('\0');
    if (!toks.isEmpty() && !toks[0].isEmpty()) {
      out[u32(base.raw, REC_START + static_cast<int>(r) * REC_SIZE + F_GROUP)]
          .insert(familyKey(toks[0]));
    }
  }
  return out;
}

bool sharesFamily(const std::set<QByteArray>& existing,
                  const std::vector<QByteArray>& additions)
{
  for (const QByteArray& code : additions) {
    if (existing.count(familyKey(code))) {
      return true;
    }
  }
  return false;
}

/// 核心合并：把 additions 的记录追加到基底，重建偏移/尺寸字段。
QByteArray mergeRecords(const BaseTable& base,
                        const std::vector<FragRecord>& additions)
{
  std::vector<QByteArray> records;
  records.reserve(base.count + additions.size());
  for (quint32 r = 0; r < base.count; ++r) {
    records.push_back(base.raw.mid(REC_START + static_cast<int>(r) * REC_SIZE, REC_SIZE));
  }

  // 碎片作者已经排好 page/slot/group，不能一律覆盖成 0/0：
  //   9nrq00/hnrq00/cnrq00  = 同一家族的 3 个形态，slot 0/1/2
  //   kush00..03            = 同一家族的 4 个形态，slot 0..3
  //   5obt_01/02/03         = 原版 Obito(group 7) 的三个服装，slot 1/2/3
  // 全改成 page=0/slot=0 会让它们叠在同一格，选人界面看不到。
  int maxPage = -1;
  quint32 maxGroupId = 0;
  for (quint32 r = 0; r < base.count; ++r) {
    const int off = REC_START + static_cast<int>(r) * REC_SIZE;
    maxPage = std::max(maxPage, i32(base.raw, off + F_PAGE));
    maxGroupId = std::max(maxGroupId, u32(base.raw, off + F_GROUP));
  }
  int nextPage = maxPage + 1;
  // 键必须带碎片路径：多个 mod 的作者都把新角色放在 page 8，
  // 只按原 page 映射会让所有 mod 的新角色再次叠在同一页。
  using GroupKey = QPair<QString, quint32>;
  using PageKey = QPair<QString, qint32>;
  std::map<PageKey, int> pageMap;

  const std::map<quint32, std::set<QByteArray>> baseGroups = baseGroupCodes(base);
  std::map<GroupKey, std::vector<QByteArray>> addCodesByGroup;
  for (const FragRecord& add : additions) {
    if (!add.strings.empty()) {
      addCodesByGroup[GroupKey(add.source, add.group)].push_back(add.strings[0]);
    }
  }
  std::map<GroupKey, quint32> groupMap;
  for (const auto& kv : addCodesByGroup) {
    auto it = baseGroups.find(kv.first.second);
    if (it != baseGroups.end() && sharesFamily(it->second, kv.second)) {
      groupMap[kv.first] = kv.first.second;  // 例：5obt_01/02/03 归入原版 Obito 家族
    } else {
      groupMap[kv.first] = ++maxGroupId;
    }
  }

  std::vector<QByteArray> groups;  // 只放新追加的组
  for (const FragRecord& add : additions) {
    QByteArray rec = add.record;
    if (rec.size() < REC_SIZE) {
      rec.append(QByteArray(REC_SIZE - rec.size(), '\0'));
    }

    int page = add.page;
    if (page >= 0) {
      const PageKey key(add.source, add.page);
      auto pit = pageMap.find(key);
      if (pit == pageMap.end()) {
        pit = pageMap.emplace(key, nextPage++).first;
      }
      page = pit->second;
    }
    quint32 group = maxGroupId;
    const GroupKey gkey(add.source, add.group);
    auto git = groupMap.find(gkey);
    if (git != groupMap.end()) {
      group = git->second;
    } else {
      group = ++maxGroupId;
    }

    put32(rec, F_PAGE, static_cast<quint32>(page));
    put32(rec, F_SLOT, add.slot);
    put32(rec, F_GROUP, group);
    records.push_back(rec);
    QByteArray g = buildGroup(add.strings);
    if (g.isEmpty()) {
      g = QByteArray(8, '\0');  // 占位，保证 groups 与 additions 一一对应
    }
    groups.push_back(std::move(g));
  }

  const int newN = static_cast<int>(records.size());
  const int oldDataOff = base.dataOff;
  const int newDataOff = REC_START + newN * REC_SIZE + STR_HDR;
  const int delta = newDataOff - oldDataOff;

  QByteArray data = base.data;
  std::vector<int> rels = base.rels;  // 复制现有相对偏移
  for (const QByteArray& g : groups) {
    if (data.size() % ALIGN) {
      data.append(QByteArray(ALIGN - data.size() % ALIGN, '\0'));
    }
    rels.push_back(data.size());
    data.append(g);
  }
  if (data.size() % ALIGN) {
    data.append(QByteArray(ALIGN - data.size() % ALIGN, '\0'));
  }

  QByteArray out = base.raw.left(REC_START);
  for (const QByteArray& r : records) {
    out.append(r);
  }

  // 重写每条记录的偏移字段
  for (int r = 0; r < newN; ++r) {
    const int off = REC_START + r * REC_SIZE;
    const quint32 f32 = static_cast<quint32>(static_cast<qint64>(newDataOff) +
                                             rels[r] -
                                             static_cast<qint64>(REC_SIZE) * (r + 1));
    if (r < static_cast<int>(base.count)) {
      const quint32 f16 = u32(out, off + F_ACTIVE);
      const quint32 f56 = u32(out, off + F_56);
      const quint32 f72 = u32(out, off + F_72);
      if (f16) put32(out, off + F_ACTIVE, f16 + delta);
      if (f56) put32(out, off + F_56, f56 + delta);
      if (f72) put32(out, off + F_72, f72 + delta);
    } else {
      put32(out, off + F_ACTIVE, f32);
      put32(out, off + F_56, f32 - 16);
      put32(out, off + F_72, f32 - 16);
    }
    put32(out, off + F_DATA, f32);
  }

  QByteArray strHdr = base.raw.mid(base.strHdr, STR_HDR);
  put32(strHdr, 0x10, static_cast<quint32>(data.size()));
  out.append(strHdr);
  out.append(data);
  out.append(base.trailer.left(TRAILER));

  put32(out, COUNT_OFF, static_cast<quint32>(newN));
  put32(out, REGION_OFF, static_cast<quint32>(newN * REC_SIZE));
  putBE32(out, 0x120, static_cast<quint32>(out.size() - REC_SIZE));
  putBE32(out, 0x12C, static_cast<quint32>(out.size() - REC_SIZE - 4));
  return out;
}

/// 统计一份表里「已占用的角色码」——用于去重（同一角色只合并一次）。
std::set<QByteArray> occupiedCodes(const BaseTable& base)
{
  std::set<QByteArray> codes;
  std::set<int> order(base.rels.begin(), base.rels.end());
  std::map<int, int> nextOf;
  for (auto it = order.begin(); it != order.end(); ++it) {
    auto nx = std::next(it);
    nextOf[*it] = (nx == order.end()) ? base.data.size() : *nx;
  }
  for (quint32 r = 0; r < base.count; ++r) {
    const int start = base.rels[r];
    const QByteArray group = base.data.mid(start, nextOf[start] - start);
    const QList<QByteArray> toks = group.split('\0');
    if (!toks.isEmpty() && !toks[0].isEmpty()) {
      codes.insert(toks[0]);
    }
  }
  return codes;
}

/// data 目录是否为「编译器工作区」（其父目录名是 Characters/Models 下的角色 id）。
bool isWorkspaceDataDir(const QString& dirName, const QString& parentName)
{
  Q_UNUSED(dirName);
  // 只认 Characters/<id>/data 与 Models/<id>/data 这种两层结构里的 data。
  // parentName 是 <id>，调用方保证父的父是 Characters 或 Models。
  return parentName != QLatin1String("Files");  // Resources/Files/data 排除
}

/// 递归收集所有待遮蔽的编译器工作区 data 目录（Characters/*/data, Models/*/data）。
QStringList collectWorkspaceDataDirs(const QString& modDir)
{
  QStringList out;
  const QStringList roots = {QStringLiteral("Characters"), QStringLiteral("Models")};
  for (const QString& root : roots) {
    const QString rootPath = modDir + QLatin1Char('/') + root;
    QDir rd(rootPath);
    if (!rd.exists()) {
      continue;
    }
    for (const QFileInfo& idDir :
         rd.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
      const QString dataPath = idDir.absoluteFilePath() + QStringLiteral("/data");
      if (QFileInfo(dataPath).isDir()) {
        out.append(QDir::fromNativeSeparators(dataPath));
      }
    }
  }
  return out;
}

/// 递归 Find 某文件名（大小写不敏感），返回所有路径。
QStringList findFilesRecursive(const QString& dir, const QString& fileName)
{
  QStringList out;
  QDir d(dir);
  if (!d.exists()) {
    return out;
  }
  const QFileInfoList entries =
      d.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
  for (const QFileInfo& e : entries) {
    if (e.isDir()) {
      out.append(findFilesRecursive(e.absoluteFilePath(), fileName));
    } else if (e.fileName().compare(fileName, Qt::CaseInsensitive) == 0) {
      out.append(QDir::fromNativeSeparators(e.absoluteFilePath()));
    }
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
QString findBestBaseTable(const QString& basePath, const QString& profile)
{
  // 注意：只接受完整表。碎片（1~4 条记录）绝不能当基底，
  // 否则会把碎片自己的记录当作“已占用”而全部去重掉，写出残缺选人表。

  const QString modlist = basePath + QStringLiteral("/profiles/") + profile +
                          QStringLiteral("/modlist.txt");
  QFile f(modlist);
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return QString();
  }
  const QString text = QString::fromUtf8(f.readAll());
  f.close();

  const QString target = QStringLiteral("characterSelectParam.xfbin");
  QString best;
  quint32 bestCount = 0;
  for (const QString& raw : text.split(QLatin1Char('\n'))) {
    const QString line = raw.trimmed();
    if (!line.startsWith(QLatin1Char('+'))) {
      continue;
    }
    const QString modDir = basePath + QStringLiteral("/mods/") + line.mid(1).trimmed();
    for (const QString& path : findFilesRecursive(modDir, target)) {
      if (path.contains(QStringLiteral(".nsc_shadowed"))) {
        continue;  // 工作区碎片不是基底候选
      }
      QFile tf(path);
      if (!tf.open(QIODevice::ReadOnly)) {
        continue;
      }
      const QByteArray b = tf.readAll();
      tf.close();
      BaseTable bt;
      QString err;
      if (parseBase(b, bt, &err) &&
          static_cast<int>(bt.count) >= MIN_BASE_RECORDS &&
          bt.count > bestCount) {
        bestCount = bt.count;
        best = path;
      }
    }
  }
  return best;
}

Result fixEnabledMods(const QString& basePath, const QString& profile,
                      const QString& backupBasePath)
{
  Result res;
  const QString modlist = basePath + QStringLiteral("/profiles/") + profile +
                          QStringLiteral("/modlist.txt");
  QFile mf(modlist);
  if (!mf.open(QIODevice::ReadOnly | QIODevice::Text)) {
    res.errors.append(QStringLiteral("cannot read modlist: ") + modlist);
    return res;
  }
  const QString text = QString::fromUtf8(mf.readAll());
  mf.close();

  QStringList enabled;
  for (const QString& raw : text.split(QLatin1Char('\n'))) {
    const QString line = raw.trimmed();
    if (line.startsWith(QLatin1Char('+'))) {
      const QString name = line.mid(1).trimmed();
      if (!name.isEmpty()) {
        enabled.append(name);
      }
    }
  }

  // ---- 步骤 1：遮蔽编译器工作区的 data 目录 ----
  for (const QString& name : enabled) {
    const QString modDir = basePath + QStringLiteral("/mods/") + name;
    for (const QString& dataDir : collectWorkspaceDataDirs(modDir)) {
      const QString shadowed = dataDir + QStringLiteral(".nsc_shadowed");
      if (QFileInfo(shadowed).isDir()) {
        continue;  // 已遮蔽，幂等
      }
      if (QDir().rename(dataDir, shadowed)) {
        ++res.shadowedDirs;
      } else {
        res.errors.append(QStringLiteral("cannot shadow ") + dataDir);
      }
    }
  }

  // 如果 override 表已由官方 NSC 编译器 ViewModel 生成（外部合并器写入标记），
  // 本 DLL 不再二次合并覆盖 —— 只保留上面的遮蔽步骤即可。
  // 需要重新用内置合并器时，删除该标记文件。
  const QString officialMark =
      basePath + QStringLiteral("/overwrite/.nsc_official_tables");
  if (QFileInfo::exists(officialMark)) {
    return res;
  }

  // ---- 步骤 2：收集碎片（从已遮蔽或未遮蔽的 data 目录都读） ----
  const QString target = QStringLiteral("characterSelectParam.xfbin");
  std::vector<QString> fragPaths;
  for (const QString& name : enabled) {
    const QString modDir = basePath + QStringLiteral("/mods/") + name;
    for (const QString& path : findFilesRecursive(modDir, target)) {
      // 只收碎片（< 10KB），跳过完整表
      QFileInfo fi(path);
      if (fi.size() > 0 && fi.size() < 10240) {
        fragPaths.push_back(path);
        ++res.fragmentCandidates;
      }
    }
  }

  // ---- 步骤 3：选基底并合并 ----
  QString baseTablePath = findBestBaseTable(basePath, profile);
  QByteArray baseRaw;
  if (!baseTablePath.isEmpty()) {
    QFile bf(baseTablePath);
    if (bf.open(QIODevice::ReadOnly)) {
      baseRaw = bf.readAll();
      bf.close();
    }
  }
  if (baseRaw.isEmpty()) {
    // 兜底：随插件附带的 pristine 基底（部署在 mod-tools 下，不碰游戏根）。
    const QString fallbacks[] = {
        QCoreApplication::applicationDirPath() +
            QStringLiteral("/mod-tools/nsc-mo2/base/characterSelectParam.vanilla.xfbin"),
        backupBasePath + QStringLiteral("/base/characterSelectParam.vanilla.xfbin")};
    for (const QString& fb : fallbacks) {
      QFile bf(fb);
      if (bf.open(QIODevice::ReadOnly)) {
        const QByteArray b = bf.readAll();
        bf.close();
        BaseTable bt;
        QString e;
        if (!b.isEmpty() && parseBase(b, bt, &e)) {
          baseRaw = b;
          baseTablePath = fb;  // 记录真实基底来源，便于日志核对
          break;
        }
      }
    }
  }
  if (baseRaw.isEmpty()) {
    res.errors.append(QStringLiteral("no full base table found (mod / mod-tools fallback)"));
    return res;
  }
  BaseTable base;
  QString baseErr;
  if (!parseBase(baseRaw, base, &baseErr)) {
    res.errors.append(QStringLiteral("base table invalid: ") + baseErr);
    return res;
  }
  res.baseRecords = static_cast<int>(base.count);
  res.baseSource = baseTablePath.isEmpty() ? QStringLiteral("unknown")
                                           : baseTablePath;

  std::set<QByteArray> codes = occupiedCodes(base);
  std::vector<FragRecord> additions;
  const QString canonicalBase = QDir::fromNativeSeparators(baseTablePath);
  for (const QString& fp : fragPaths) {
    // 基底本身绝不能再当作待合并碎片，否则它的记录会全部被去重掉。
    if (!canonicalBase.isEmpty() &&
        QDir::fromNativeSeparators(fp) == canonicalBase) {
      ++res.fragmentRejected;
      continue;
    }
    ParsedFrag parsed;
    QString err;
    if (!parseFragment(fp, parsed, &err)) {
      ++res.fragmentRejected;
      res.errors.append(QFileInfo(fp).fileName() + QStringLiteral(": ") + err);
      continue;
    }
    for (FragRecord& fr : parsed.records) {
      // 用第一串（角色码）去重
      if (!fr.strings.empty() && codes.count(fr.strings[0])) {
        ++res.fragmentDuplicates;
        continue;
      }
      if (!fr.strings.empty()) {
        codes.insert(fr.strings[0]);
      }
      additions.push_back(std::move(fr));
      ++res.fragmentsMerged;
    }
  }

  const QByteArray merged = mergeRecords(base, additions);
  res.recordsAdded = static_cast<int>(additions.size());
  res.recordsTotal = static_cast<int>(u32(merged, COUNT_OFF));

  // ---- 步骤 4：写到 overwrite（VFS 最高优先级） ----
  const QString outPath = basePath + QStringLiteral("/overwrite/ui/max/select/") +
                          target;
  QDir().mkpath(QFileInfo(outPath).absolutePath());
  QFile out(outPath);
  if (!out.open(QIODevice::WriteOnly)) {
    res.errors.append(QStringLiteral("cannot write ") + outPath);
    return res;
  }
  out.write(merged);
  out.close();
  res.finalTable = outPath;
  return res;
}

}  // namespace Merge
}  // namespace nsc
