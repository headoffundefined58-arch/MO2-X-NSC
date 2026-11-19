#include "nsc_arc.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <oleauto.h>
#include <propidl.h>

// 7-Zip SDK 的定宽类型（windows.h 不提供这些名字）。虚表签名必须与之逐字一致。
using Int32 = int;
using UInt32 = unsigned int;
using Int64 = long long;
using UInt64 = unsigned long long;

namespace nsc {
namespace {

// ---------------------------------------------------------------------------
// 7-Zip COM 接口的最小声明
//
// 只声明用得到的部分。方法顺序必须与 7-Zip 的头文件**完全一致**（前三个永远是
// IUnknown），虚表错一位就是崩溃，不能凭感觉加减方法。
// ---------------------------------------------------------------------------
#define NSC_COM_BASE                                                     \
  virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) = 0; \
  virtual ULONG STDMETHODCALLTYPE AddRef() = 0;                          \
  virtual ULONG STDMETHODCALLTYPE Release() = 0;

struct ISeqInStreamX {
  NSC_COM_BASE
  virtual HRESULT STDMETHODCALLTYPE Read(void* data, UInt32 size,
                                         UInt32* processed) = 0;
};

struct IInStreamX : public ISeqInStreamX {
  virtual HRESULT STDMETHODCALLTYPE Seek(Int64 offset, UInt32 origin,
                                         UInt64* newPos) = 0;
};

struct ISeqOutStreamX {
  NSC_COM_BASE
  virtual HRESULT STDMETHODCALLTYPE Write(const void* data, UInt32 size,
                                          UInt32* processed) = 0;
};

struct IProgressX {
  NSC_COM_BASE
  virtual HRESULT STDMETHODCALLTYPE SetTotal(UInt64 total) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetCompleted(const UInt64* completed) = 0;
};

struct IArchiveExtractCallbackX : public IProgressX {
  virtual HRESULT STDMETHODCALLTYPE GetStream(UInt32 index,
                                              ISeqOutStreamX** outStream,
                                              Int32 askExtractMode) = 0;
  virtual HRESULT STDMETHODCALLTYPE PrepareOperation(Int32 askExtractMode) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetOperationResult(Int32 opRes) = 0;
};

struct IInArchiveX {
  NSC_COM_BASE
  virtual HRESULT STDMETHODCALLTYPE Open(IInStreamX* stream,
                                         const UInt64* maxCheckStartPosition,
                                         void* openCallback) = 0;
  virtual HRESULT STDMETHODCALLTYPE Close() = 0;
  virtual HRESULT STDMETHODCALLTYPE GetNumberOfItems(UInt32* numItems) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetProperty(UInt32 index, UInt32 propID,
                                                PROPVARIANT* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE Extract(const UInt32* indices,
                                            UInt32 numItems, Int32 testMode,
                                            IArchiveExtractCallbackX* callback) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetArchiveProperty(UInt32 propID,
                                                       PROPVARIANT* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetNumberOfProperties(UInt32* numProps) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetPropertyInfo(UInt32 index, BSTR* name,
                                                    UInt32* propID,
                                                    VARTYPE* varType) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetNumberOfArchiveProperties(
      UInt32* numProps) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetArchivePropertyInfo(UInt32 index,
                                                           BSTR* name,
                                                           UInt32* propID,
                                                           VARTYPE* varType) = 0;
};

// 7-Zip 的属性 id（只用两个）。
constexpr UInt32 kpidPath = 3;
constexpr UInt32 kpidIsDir = 6;

const GUID kIIDInArchive = {0x23170F69,
                            0x40C1,
                            0x278A,
                            {0x00, 0x00, 0x00, 0x06, 0x00, 0x60, 0x00, 0x00}};

// 格式 handler 的 CLSID。规则：23170F69-40C1-278A-1000-0001 10 <fmt> 0000
const GUID kClsidZip = {0x23170F69,
                        0x40C1,
                        0x278A,
                        {0x10, 0x00, 0x00, 0x01, 0x10, 0x01, 0x00, 0x00}};
const GUID kClsid7z = {0x23170F69,
                       0x40C1,
                       0x278A,
                       {0x10, 0x00, 0x00, 0x01, 0x10, 0x07, 0x00, 0x00}};
const GUID kClsidRar = {0x23170F69,
                        0x40C1,
                        0x278A,
                        {0x10, 0x00, 0x00, 0x01, 0x10, 0x03, 0x00, 0x00}};

// ---------------------------------------------------------------------------
// 输入流：把 QFile 适配成 IInStream
// ---------------------------------------------------------------------------
class FileInStream final : public IInStreamX
{
public:
  explicit FileInStream(const QString& path) : m_file(path)
  {
    m_open = m_file.open(QIODevice::ReadOnly);
  }

  bool isOpen() const { return m_open; }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** ppv) override
  {
    if (ppv) {
      *ppv = nullptr;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }

  HRESULT STDMETHODCALLTYPE Read(void* data, UInt32 size,
                                 UInt32* processed) override
  {
    if (processed) {
      *processed = 0;
    }
    if (!m_open) {
      return S_FALSE;
    }
    const qint64 n = m_file.read(static_cast<char*>(data), static_cast<qint64>(size));
    if (n < 0) {
      return E_FAIL;
    }
    if (processed) {
      *processed = static_cast<UInt32>(n);
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Seek(Int64 offset, UInt32 origin,
                                 UInt64* newPos) override
  {
    if (!m_open) {
      return S_FALSE;
    }
    qint64 base = 0;
    if (origin == 1) {
      base = m_file.pos();
    } else if (origin == 2) {
      base = m_file.size();
    }
    if (!m_file.seek(base + static_cast<qint64>(offset))) {
      return E_FAIL;
    }
    if (newPos) {
      *newPos = static_cast<UInt64>(m_file.pos());
    }
    return S_OK;
  }

private:
  QFile m_file;
  bool m_open = false;
};

// ---------------------------------------------------------------------------
// 输出流：把 QFile 适配成 ISeqOutStream
// ---------------------------------------------------------------------------
class FileOutStream final : public ISeqOutStreamX
{
public:
  explicit FileOutStream(QFile* file) : m_file(file) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** ppv) override
  {
    if (ppv) {
      *ppv = nullptr;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }

  HRESULT STDMETHODCALLTYPE Write(const void* data, UInt32 size,
                                  UInt32* processed) override
  {
    if (processed) {
      *processed = 0;
    }
    if (!m_file) {
      return S_FALSE;
    }
    const qint64 n =
        m_file->write(static_cast<const char*>(data), static_cast<qint64>(size));
    if (n < 0) {
      return E_FAIL;
    }
    if (processed) {
      *processed = static_cast<UInt32>(n);
    }
    return S_OK;
  }

private:
  QFile* m_file = nullptr;
};

// ---------------------------------------------------------------------------
// 解包回调：按条目路径在 outDir 下落盘
// ---------------------------------------------------------------------------
class ExtractSink final : public IArchiveExtractCallbackX
{
public:
  ExtractSink(IInArchiveX* archive, const QString& outRoot, QStringList* entries)
      : m_archive(archive), m_root(outRoot), m_entries(entries)
  {
  }

  // COM 接口基类没有虚析构函数，不能加 override；由 Release() 管理生命周期。
  ~ExtractSink() { closeCurrent(); }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** ppv) override
  {
    if (ppv) {
      *ppv = nullptr;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }

  HRESULT STDMETHODCALLTYPE SetTotal(UInt64) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE SetCompleted(const UInt64*) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE PrepareOperation(Int32) override { return S_OK; }

  HRESULT STDMETHODCALLTYPE SetOperationResult(Int32) override
  {
    closeCurrent();
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetStream(UInt32 index, ISeqOutStreamX** outStream,
                                      Int32) override
  {
    if (outStream) {
      *outStream = nullptr;
    }
    closeCurrent();

    const QString rel = relativePath(index);
    if (rel.isEmpty()) {
      return S_OK;
    }
    const QString abs = m_root + QLatin1Char('/') + rel;

    if (isDirectory(index)) {
      QDir().mkpath(abs);
      if (m_entries) {
        m_entries->append(rel + QLatin1Char('/'));
      }
      return S_OK;
    }

    QDir().mkpath(QFileInfo(abs).absolutePath());
    m_file = new QFile(abs);
    if (!m_file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
      delete m_file;
      m_file = nullptr;
      return E_FAIL;
    }
    m_writer = new FileOutStream(m_file);
    if (outStream) {
      *outStream = m_writer;
    }
    if (m_entries) {
      m_entries->append(rel);
    }
    return S_OK;
  }

private:
  void closeCurrent()
  {
    if (m_file) {
      m_file->close();
      delete m_file;
      m_file = nullptr;
    }
    delete m_writer;
    m_writer = nullptr;
  }

  QString relativePath(UInt32 index) const
  {
    PROPVARIANT prop;
    PropVariantInit(&prop);
    QString out;
    if (m_archive->GetProperty(index, kpidPath, &prop) == S_OK && prop.vt == VT_BSTR &&
        prop.bstrVal) {
      out = QString::fromWCharArray(prop.bstrVal);
    }
    if (prop.vt == VT_BSTR && prop.bstrVal) {
      SysFreeString(prop.bstrVal);
    }
    out.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (out.startsWith(QLatin1Char('/'))) {
      out.remove(0, 1);
    }
    // 拒绝绝对路径与上跳，防止归档写出 mod 目录之外。
    if (out.isEmpty() || out.contains(QLatin1String("..")) ||
        out.contains(QLatin1Char(':'))) {
      return QString();
    }
    return out;
  }

  bool isDirectory(UInt32 index) const
  {
    PROPVARIANT prop;
    PropVariantInit(&prop);
    bool dir = false;
    if (m_archive->GetProperty(index, kpidIsDir, &prop) == S_OK &&
        prop.vt == VT_BOOL) {
      dir = (prop.boolVal != VARIANT_FALSE);
    }
    return dir;
  }

  IInArchiveX* m_archive = nullptr;
  QString m_root;
  QStringList* m_entries = nullptr;
  QFile* m_file = nullptr;
  FileOutStream* m_writer = nullptr;
};

}  // namespace

bool Archive::isArchive(const QString& fileName)
{
  static const QStringList kSuffixes = {
      QStringLiteral("zip"),  QStringLiteral("nsc"),  QStringLiteral("ensc"),
      QStringLiteral("uns"),  QStringLiteral("unse"), QStringLiteral("nus4"),
      QStringLiteral("7z"),   QStringLiteral("rar")};
  return kSuffixes.contains(QFileInfo(fileName).suffix().toLower());
}

QString Archive::sevenZipPath()
{
  const QString app = QCoreApplication::applicationDirPath();
  const QStringList candidates = {
      app + QStringLiteral("/dlls/7z.dll"),
      app + QStringLiteral("/7z.dll"),
      app + QStringLiteral("/../dlls/7z.dll"),
      app + QStringLiteral("/plugins/nsc_tools/7z.dll"),
  };
  for (const QString& candidate : candidates) {
    const QFileInfo info(candidate);
    if (!info.isFile()) {
      continue;
    }
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? info.absoluteFilePath() : canonical;
  }
  return QString();
}

bool Archive::extract(const QString& archivePath, const QString& outDir,
                      QStringList* entries, QString* error)
{
  const auto fail = [error](const QString& message) {
    if (error) {
      *error = message;
    }
    return false;
  };

  if (!QFileInfo(archivePath).isFile()) {
    return fail(QStringLiteral("archive not found: ") + archivePath);
  }

  const QString dll = sevenZipPath();
  if (dll.isEmpty()) {
    return fail(QStringLiteral("7z.dll not found next to MO2"));
  }

  HMODULE module = ::LoadLibraryW(reinterpret_cast<LPCWSTR>(dll.utf16()));
  if (!module) {
    return fail(QStringLiteral("LoadLibrary failed: ") + dll);
  }

  using CreateObjectFn = HRESULT(STDMETHODCALLTYPE*)(const GUID*, const GUID*, void**);
  auto createObject = reinterpret_cast<CreateObjectFn>(
      reinterpret_cast<void*>(::GetProcAddress(module, "CreateObject")));
  if (!createObject) {
    ::FreeLibrary(module);
    return fail(QStringLiteral("CreateObject missing in 7z.dll"));
  }

  QDir().mkpath(outDir);

  auto* stream = new FileInStream(archivePath);
  if (!stream->isOpen()) {
    delete stream;
    ::FreeLibrary(module);
    return fail(QStringLiteral("cannot open archive: ") + archivePath);
  }

  const GUID* handlers[] = {&kClsidZip, &kClsid7z, &kClsidRar};
  IInArchiveX* archive = nullptr;
  for (const GUID* clsid : handlers) {
    IInArchiveX* candidate = nullptr;
    if (createObject(clsid, &kIIDInArchive, reinterpret_cast<void**>(&candidate)) !=
            S_OK ||
        !candidate) {
      continue;
    }
    stream->Seek(0, 0, nullptr);
    if (candidate->Open(stream, nullptr, nullptr) == S_OK) {
      archive = candidate;
      break;
    }
    candidate->Close();
    candidate->Release();
  }

  if (!archive) {
    delete stream;
    ::FreeLibrary(module);
    return fail(QStringLiteral("unsupported archive format: ") + archivePath);
  }

  UInt32 count = 0;
  if (archive->GetNumberOfItems(&count) != S_OK) {
    count = 0;
  }

  ExtractSink sink(archive, outDir, entries);
  const HRESULT hr = archive->Extract(nullptr, static_cast<UInt32>(-1), 0, &sink);

  archive->Close();
  archive->Release();
  delete stream;
  ::FreeLibrary(module);

  if (hr != S_OK && hr != S_FALSE) {
    return fail(QStringLiteral("extract failed (hr=0x%1)").arg(static_cast<uint>(hr), 8, 16));
  }
  return true;
}

}  // namespace nsc
