#include "property_handler.h"
#include "dirsize/guids.h"

#include <algorithm>
#include <mutex>
#include <propkey.h>

namespace dirsize {

LONG g_dllRefCount = 0;

void DllAddRef() { InterlockedIncrement(&g_dllRefCount); }
void DllRelease() { InterlockedDecrement(&g_dllRefCount); }

// ---------------------------------------------------------------------------
// SizeCache — process-wide LRU cache
// ---------------------------------------------------------------------------

SizeCache& SizeCache::Instance() {
    static SizeCache instance;
    return instance;
}

std::optional<uint64_t> SizeCache::Get(const std::wstring& path) {
    std::shared_lock lock(m_mutex);
    auto it = m_cache.find(path);
    if (it == m_cache.end() || !it->second.valid) return std::nullopt;

    auto age = std::chrono::steady_clock::now() - it->second.timestamp;
    if (age > kTtl) return std::nullopt; // Stale

    return it->second.size;
}

std::optional<uint64_t> SizeCache::GetAlloc(const std::wstring& path) {
    std::shared_lock lock(m_mutex);
    auto it = m_cache.find(path);
    if (it == m_cache.end() || !it->second.valid) return std::nullopt;

    auto age = std::chrono::steady_clock::now() - it->second.timestamp;
    if (age > kTtl) return std::nullopt;

    return it->second.allocSize;
}

void SizeCache::Put(const std::wstring& path, uint64_t size, uint64_t allocSize) {
    std::unique_lock lock(m_mutex);

    // Evict oldest entries if cache is full
    if (m_cache.size() >= kMaxEntries) {
        // Simple eviction: clear half the cache
        auto it = m_cache.begin();
        size_t toRemove = kMaxEntries / 2;
        while (it != m_cache.end() && toRemove > 0) {
            it = m_cache.erase(it);
            toRemove--;
        }
    }

    CacheEntry entry;
    entry.size = size;
    entry.allocSize = allocSize;
    entry.valid = true;
    entry.timestamp = std::chrono::steady_clock::now();
    m_cache[path] = entry;
}

void SizeCache::Invalidate(const std::wstring& path) {
    std::unique_lock lock(m_mutex);
    m_cache.erase(path);
}

// ---------------------------------------------------------------------------
// Shared SQLite connection for the shell extension (read-only)
// ---------------------------------------------------------------------------

static Database& GetSharedDb() {
    static Database db;
    static std::once_flag initFlag;
    std::call_once(initFlag, [] {
        db.Open(Database::GetDefaultPath(), true /* readOnly */);
    });
    return db;
}

// ---------------------------------------------------------------------------
// DirSizePropertyHandler
// ---------------------------------------------------------------------------

DirSizePropertyHandler::DirSizePropertyHandler() {
    DllAddRef();
}

DirSizePropertyHandler::~DirSizePropertyHandler() {
    DllRelease();
}

HRESULT DirSizePropertyHandler::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) return E_POINTER;

    if (riid == IID_IUnknown) {
        *ppvObject = static_cast<IPropertyStore*>(this);
    } else if (riid == IID_IPropertyStore) {
        *ppvObject = static_cast<IPropertyStore*>(this);
    } else if (riid == IID_IInitializeWithFile) {
        *ppvObject = static_cast<IInitializeWithFile*>(this);
    } else if (riid == IID_IPropertyStoreCapabilities) {
        *ppvObject = static_cast<IPropertyStoreCapabilities*>(this);
    } else {
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
}

ULONG DirSizePropertyHandler::AddRef() {
    return InterlockedIncrement(&m_refCount);
}

ULONG DirSizePropertyHandler::Release() {
    ULONG count = InterlockedDecrement(&m_refCount);
    if (count == 0) delete this;
    return count;
}

HRESULT DirSizePropertyHandler::Initialize(LPCWSTR pszFilePath, DWORD /*grfMode*/) {
    if (!pszFilePath) return E_INVALIDARG;

    m_path = pszFilePath;
    // Normalize for cache/DB lookup
    std::replace(m_path.begin(), m_path.end(), L'/', L'\\');
    while (m_path.size() > 3 && m_path.back() == L'\\') {
        m_path.pop_back();
    }
    std::transform(m_path.begin(), m_path.end(), m_path.begin(), ::towlower);

    // Try in-memory cache first
    m_cachedSize = SizeCache::Instance().Get(m_path);

    if (!m_cachedSize) {
        // Fall back to SQLite
        auto& db = GetSharedDb();
        auto entry = db.GetEntry(m_path);
        if (entry) {
            m_cachedSize = entry->totalSize;
            SizeCache::Instance().Put(m_path, entry->totalSize, entry->allocSize);
        }
    }

    return S_OK;
}

HRESULT DirSizePropertyHandler::GetCount(DWORD* cProps) {
    if (!cProps) return E_POINTER;
    *cProps = 1;
    return S_OK;
}

HRESULT DirSizePropertyHandler::GetAt(DWORD iProp, PROPERTYKEY* pkey) {
    if (!pkey) return E_POINTER;
    if (iProp == 0) {
        *pkey = PKEY_Size;  // System.Size — the built-in "Size" column
        return S_OK;
    }
    return E_INVALIDARG;
}

HRESULT DirSizePropertyHandler::GetValue(REFPROPERTYKEY key, PROPVARIANT* pv) {
    if (!pv) return E_POINTER;
    PropVariantInit(pv);

    // Respond to System.Size queries with our cached directory size
    if (IsEqualPropertyKey(key, PKEY_Size) && m_cachedSize) {
        pv->vt = VT_UI8;
        pv->uhVal.QuadPart = *m_cachedSize;
        return S_OK;
    }

    return S_OK; // Return empty for unknown properties
}

HRESULT DirSizePropertyHandler::SetValue(REFPROPERTYKEY /*key*/, REFPROPVARIANT /*propvar*/) {
    return STG_E_ACCESSDENIED; // Read-only
}

HRESULT DirSizePropertyHandler::Commit() {
    return S_OK; // Nothing to commit
}

HRESULT DirSizePropertyHandler::IsPropertyWritable(REFPROPERTYKEY /*key*/) {
    return S_FALSE; // Not writable
}

} // namespace dirsize
