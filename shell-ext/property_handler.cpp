#include "property_handler.h"
#include "dirsize/config.h"
#include "dirsize/generation.h"
#include "dirsize/guids.h"
#include "dirsize/path_utils.h"

#include <algorithm>
#include <atomic>
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

std::optional<CacheEntry> SizeCache::Lookup(const std::wstring& path) {
    std::shared_lock lock(m_mutex);
    auto it = m_cache.find(path);
    if (it == m_cache.end()) return std::nullopt;

    // Stale if the engine wrote fresh size data since this was cached —
    // this is what lets Explorer show new sizes immediately after a
    // rescan instead of serving up-to-60s-old values.
    if (it->second.generation != GetSizeDataGeneration()) return std::nullopt;

    auto age = std::chrono::steady_clock::now() - it->second.timestamp;
    auto ttl = it->second.found ? kTtl : kNegativeTtl;
    if (age > ttl) return std::nullopt; // Stale

    return it->second;
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
    entry.found = true;
    entry.generation = GetSizeDataGeneration();
    entry.timestamp = std::chrono::steady_clock::now();
    m_cache[path] = entry;
}

void SizeCache::PutNegative(const std::wstring& path) {
    std::unique_lock lock(m_mutex);

    if (m_cache.size() >= kMaxEntries) {
        auto it = m_cache.begin();
        size_t toRemove = kMaxEntries / 2;
        while (it != m_cache.end() && toRemove > 0) {
            it = m_cache.erase(it);
            toRemove--;
        }
    }

    CacheEntry entry;
    entry.found = false;
    entry.generation = GetSizeDataGeneration();
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

Database& GetReadOnlyDb() {
    static Database db;
    static std::mutex openMutex;
    static ULONGLONG lastAttempt = 0;
    constexpr ULONGLONG kRetryIntervalMs = 30000;

    if (!db.IsOpen()) {
        std::lock_guard lock(openMutex);
        ULONGLONG now = GetTickCount64();
        // Retry periodically: Explorer may start before the service has
        // created the database; a single failed call_once left the shell
        // extension permanently blind until the next Explorer restart.
        if (!db.IsOpen() &&
            (lastAttempt == 0 || now - lastAttempt >= kRetryIntervalMs)) {
            lastAttempt = now;
            db.Open(Database::GetDefaultPath(), true /* readOnly */);
        }
    }
    return db;
}

static Database& GetSharedDb() {
    return GetReadOnlyDb();
}

static SizeMetric GetCurrentSizeMetric() {
    static std::atomic<SizeMetric> s_metric{SizeMetric::LogicalSize};
    static std::atomic<ULONGLONG> s_lastRead{0};

    ULONGLONG now = GetTickCount64();
    if (now - s_lastRead.load() > 30000) {
        s_metric.store(static_cast<SizeMetric>(ReadRegDword(L"SizeMetric", 0)));
        s_lastRead.store(now);
    }
    return s_metric.load();
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

    m_path = CanonicalizePath(pszFilePath);
    m_cachedLogicalSize.reset();
    m_cachedAllocSize.reset();

    // Try in-memory cache first (positive OR negative hit)
    if (auto cached = SizeCache::Instance().Lookup(m_path)) {
        if (cached->found) {
            m_cachedLogicalSize = cached->size;
            m_cachedAllocSize = cached->allocSize;
        }
        return S_OK;
    }

    // Fall back to SQLite
    auto& db = GetSharedDb();
    if (!db.IsOpen()) return S_OK;

    if (auto entry = db.GetEntry(m_path)) {
        m_cachedLogicalSize = entry->totalSize;
        m_cachedAllocSize = entry->allocSize;
        SizeCache::Instance().Put(m_path, entry->totalSize, entry->allocSize);
    } else {
        SizeCache::Instance().PutNegative(m_path);
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

    // Respond to System.Size queries with current metric selection.
    if (IsEqualPropertyKey(key, PKEY_Size)) {
        std::optional<uint64_t> value;
        if (GetCurrentSizeMetric() == SizeMetric::AllocationSize) {
            value = m_cachedAllocSize ? m_cachedAllocSize : m_cachedLogicalSize;
        } else {
            value = m_cachedLogicalSize ? m_cachedLogicalSize : m_cachedAllocSize;
        }

        if (!value.has_value()) {
            return S_OK;
        }

        pv->vt = VT_UI8;
        pv->uhVal.QuadPart = *value;
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
