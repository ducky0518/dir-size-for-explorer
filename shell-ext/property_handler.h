#pragma once

#include <Unknwn.h>
#include <propsys.h>
#include <propvarutil.h>
#include <propkey.h>

#include "dirsize/db.h"

#include <string>
#include <shared_mutex>
#include <unordered_map>
#include <chrono>

namespace dirsize {

// In-process LRU cache entry for directory sizes.
// Avoids hitting SQLite on every Explorer GetValue call.
struct CacheEntry {
    uint64_t size = 0;
    uint64_t allocSize = 0;
    bool valid = false;
    std::chrono::steady_clock::time_point timestamp;
};

// Shared cache accessed by all property handler instances in the explorer.exe process.
class SizeCache {
public:
    static SizeCache& Instance();

    std::optional<uint64_t> Get(const std::wstring& path);
    std::optional<uint64_t> GetAlloc(const std::wstring& path);
    void Put(const std::wstring& path, uint64_t size, uint64_t allocSize);
    void Invalidate(const std::wstring& path);

private:
    SizeCache() = default;

    static constexpr auto kTtl = std::chrono::seconds(60);
    static constexpr size_t kMaxEntries = 10000;

    std::shared_mutex m_mutex;
    std::unordered_map<std::wstring, CacheEntry> m_cache;
};

// COM property handler for directories.
// Implements IPropertyStore (for GetValue), IInitializeWithFile (Explorer passes the path),
// and IPropertyStoreCapabilities (marks our property as read-only).
class DirSizePropertyHandler :
    public IPropertyStore,
    public IInitializeWithFile,
    public IPropertyStoreCapabilities
{
public:
    DirSizePropertyHandler();
    virtual ~DirSizePropertyHandler();

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // IInitializeWithFile
    HRESULT STDMETHODCALLTYPE Initialize(LPCWSTR pszFilePath, DWORD grfMode) override;

    // IPropertyStore
    HRESULT STDMETHODCALLTYPE GetCount(DWORD* cProps) override;
    HRESULT STDMETHODCALLTYPE GetAt(DWORD iProp, PROPERTYKEY* pkey) override;
    HRESULT STDMETHODCALLTYPE GetValue(REFPROPERTYKEY key, PROPVARIANT* pv) override;
    HRESULT STDMETHODCALLTYPE SetValue(REFPROPERTYKEY key, REFPROPVARIANT propvar) override;
    HRESULT STDMETHODCALLTYPE Commit() override;

    // IPropertyStoreCapabilities
    HRESULT STDMETHODCALLTYPE IsPropertyWritable(REFPROPERTYKEY key) override;

private:
    LONG m_refCount = 1;
    std::wstring m_path;
    std::optional<uint64_t> m_cachedSize;
};

// Global reference count for the DLL (used by DllCanUnloadNow)
extern LONG g_dllRefCount;
void DllAddRef();
void DllRelease();

} // namespace dirsize
