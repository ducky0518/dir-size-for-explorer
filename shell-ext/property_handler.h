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

// In-process cache entry for directory sizes.
// Avoids hitting SQLite on every Explorer GetValue call.
// found == false records a "known miss" (path not in the database),
// which is just as important to cache: without it every Explorer
// refresh of an unscanned folder re-queries SQLite once per entry.
struct CacheEntry {
    uint64_t size = 0;
    uint64_t allocSize = 0;
    bool found = false;
    uint32_t generation = 0; // Size-data generation when cached; entries
                             // from an older generation are stale (the
                             // engine bumped it after writing new sizes)
    std::chrono::steady_clock::time_point timestamp;
};

// Shared cache accessed by all property handler instances in the explorer.exe process.
class SizeCache {
public:
    static SizeCache& Instance();

    // Returns the cached entry (positive or negative) if still fresh,
    // std::nullopt if the path isn't cached / entry expired.
    std::optional<CacheEntry> Lookup(const std::wstring& path);

    void Put(const std::wstring& path, uint64_t size, uint64_t allocSize);
    void PutNegative(const std::wstring& path);
    void Invalidate(const std::wstring& path);

private:
    SizeCache() = default;

    static constexpr auto kTtl = std::chrono::seconds(60);
    static constexpr auto kNegativeTtl = std::chrono::seconds(15);
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
    std::optional<uint64_t> m_cachedLogicalSize;
    std::optional<uint64_t> m_cachedAllocSize;
};

// Global reference count for the DLL (used by DllCanUnloadNow)
extern LONG g_dllRefCount;
void DllAddRef();
void DllRelease();

// Process-wide read-only database connection shared by the property
// handler and the hooks. Retries the open periodically if it failed
// (e.g. Explorer started before the service created the database).
Database& GetReadOnlyDb();

} // namespace dirsize
