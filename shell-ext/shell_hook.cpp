#include "shell_hook.h"
#include "property_handler.h"
#include "dirsize/config.h"
#include "dirsize/guids.h"
#include "dirsize/path_utils.h"

#include <detours/detours.h>
#include <ShlObj.h>
#include <Shlwapi.h>
#include <winternl.h>
#include <propkey.h>
#include <propsys.h>

#include <mutex>
#include <atomic>
#include <algorithm>
#include <cstdio>

namespace dirsize {

// ---------------------------------------------------------------------------
// Debug logging (lightweight — only logs errors/init)
// ---------------------------------------------------------------------------

static FILE* OpenLog() {
    static FILE* f = nullptr;
    static std::once_flag logOnce;
    std::call_once(logOnce, [] {
        // Next to the per-user database (%LocalAppData%\DirSizeForExplorer)
        std::wstring path = Database::GetDefaultPath();
        size_t sep = path.find_last_of(L'\\');
        if (sep == std::wstring::npos) return;
        path = path.substr(0, sep + 1) + L"hook_debug.log";
        f = _wfopen(path.c_str(), L"w");
    });
    return f;
}

static void DebugLog(const char* fmt, ...) {
    FILE* f = OpenLog();
    if (!f) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fflush(f);
}

// ---------------------------------------------------------------------------
// Shared DB / cache access
// ---------------------------------------------------------------------------

// Shared with the property handler — retries the open if the service
// hasn't created the database yet (see property_handler.cpp).
static Database& GetHookDb() {
    return GetReadOnlyDb();
}

// ---------------------------------------------------------------------------
// Cached config reads (30s TTL to avoid registry hits on every call)
// ---------------------------------------------------------------------------

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

static DisplayFormat GetCurrentDisplayFormat() {
    static std::atomic<DisplayFormat> s_fmt{DisplayFormat::ExplorerDefault};
    static std::atomic<ULONGLONG> s_lastRead{0};

    ULONGLONG now = GetTickCount64();
    if (now - s_lastRead.load() > 30000) {
        s_fmt.store(static_cast<DisplayFormat>(ReadRegDword(L"DisplayFormat", 0)));
        s_lastRead.store(now);
    }
    return s_fmt.load();
}

static bool GetAutoScaleFoldersOnly() {
    static std::atomic<bool> s_val{true};
    static std::atomic<ULONGLONG> s_lastRead{0};

    ULONGLONG now = GetTickCount64();
    if (now - s_lastRead.load() > 30000) {
        s_val.store(ReadRegDword(L"AutoScaleFoldersOnly", 1) != 0);
        s_lastRead.store(now);
    }
    return s_val.load();
}

static std::optional<uint64_t> LookupDirSize(const std::wstring& fullPath) {
    std::wstring normalized = CanonicalizePath(fullPath);
    SizeMetric metric = GetCurrentSizeMetric();

    // In-memory cache first — positive AND negative hits. Negative caching
    // matters: without it, every Explorer refresh of an unscanned folder
    // re-queried SQLite once per directory entry.
    if (auto cached = SizeCache::Instance().Lookup(normalized)) {
        if (!cached->found) return std::nullopt;
        return (metric == SizeMetric::AllocationSize) ? cached->allocSize
                                                      : cached->size;
    }

    auto& db = GetHookDb();
    if (!db.IsOpen()) return std::nullopt;

    if (auto entry = db.GetEntry(normalized)) {
        SizeCache::Instance().Put(normalized, entry->totalSize, entry->allocSize);
        return (metric == SizeMetric::AllocationSize) ? entry->allocSize : entry->totalSize;
    }

    SizeCache::Instance().PutNegative(normalized);
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Handle → directory path resolution
// ---------------------------------------------------------------------------

static std::wstring GetDirForHandle(HANDLE h) {
    // Big enough for long paths; MAX_PATH silently broke deep trees.
    wchar_t buf[2048];
    DWORD len = GetFinalPathNameByHandleW(h, buf, _countof(buf),
                                           FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (len == 0 || len >= _countof(buf))
        return {};
    return std::wstring(buf, len);
}

// ---------------------------------------------------------------------------
// NtQueryDirectoryFile hook — inject sizes into NT directory listings
// ---------------------------------------------------------------------------

enum {
    kFileDirectoryInformation = 1,
    kFileFullDirectoryInformation = 2,
    kFileBothDirectoryInformation = 3,
    kFileIdBothDirectoryInformation = 37,
    kFileIdFullDirectoryInformation = 38,
    kFileIdExtdDirectoryInformation = 60,
};

struct DirEntryHeader {
    ULONG         NextEntryOffset;
    ULONG         FileIndex;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER EndOfFile;
    LARGE_INTEGER AllocationSize;
    ULONG         FileAttributes;
    ULONG         FileNameLength;
};

static ULONG GetFileNameOffset(ULONG infoClass) {
    switch (infoClass) {
    // Offsets are offsetof(FILE_xxx_INFORMATION, FileName):
    case kFileDirectoryInformation:       return 64;
    case kFileFullDirectoryInformation:   return 68;  // + EaSize
    case kFileBothDirectoryInformation:   return 94;  // + ShortName[12]
    case kFileIdBothDirectoryInformation: return 104; // + FileId (8, aligned)
    case kFileIdFullDirectoryInformation: return 80;  // EaSize + pad + FileId(8) — was wrongly 72
    case kFileIdExtdDirectoryInformation: return 88;  // EaSize + ReparseTag + FILE_ID_128 — was wrongly 120
    default: return 0;
    }
}

typedef NTSTATUS(NTAPI* NtQueryDirectoryFileFn)(
    HANDLE, HANDLE, PVOID, PVOID, PIO_STATUS_BLOCK,
    PVOID, ULONG, ULONG, BOOLEAN, PVOID, BOOLEAN);

static NtQueryDirectoryFileFn TrueNtQueryDirectoryFile = nullptr;

static NTSTATUS NTAPI HookedNtQueryDirectoryFile(
    HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine, PVOID ApcContext,
    PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation, ULONG Length,
    ULONG FileInformationClass, BOOLEAN ReturnSingleEntry,
    PVOID FileName, BOOLEAN RestartScan)
{
    NTSTATUS status = TrueNtQueryDirectoryFile(
        FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock,
        FileInformation, Length, FileInformationClass,
        ReturnSingleEntry, FileName, RestartScan);

    if (status != 0 || !FileInformation)
        return status;

    ULONG fnOffset = GetFileNameOffset(FileInformationClass);
    if (fnOffset == 0)
        return status;

    // Cheap early-out: if the size database was never readable there is
    // nothing to inject — don't pay for handle-path resolution and
    // per-entry canonicalization on every directory enumeration.
    if (!GetHookDb().IsOpen())
        return status;

    // Second early-out: only resolve the handle path if the listing
    // actually contains at least one real subdirectory.
    {
        bool hasDir = false;
        BYTE* scan = reinterpret_cast<BYTE*>(FileInformation);
        for (;;) {
            auto* hdr = reinterpret_cast<DirEntryHeader*>(scan);
            if ((hdr->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                !(hdr->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
                hdr->FileNameLength > 0) {
                const wchar_t* fn = reinterpret_cast<const wchar_t*>(scan + fnOffset);
                ULONG n = hdr->FileNameLength / sizeof(wchar_t);
                bool isDot = (n == 1 && fn[0] == L'.') ||
                             (n == 2 && fn[0] == L'.' && fn[1] == L'.');
                if (!isDot) { hasDir = true; break; }
            }
            if (hdr->NextEntryOffset == 0) break;
            scan += hdr->NextEntryOffset;
        }
        if (!hasDir)
            return status;
    }

    std::wstring dirPath = GetDirForHandle(FileHandle);
    if (dirPath.empty())
        return status;

    BYTE* ptr = reinterpret_cast<BYTE*>(FileInformation);
    for (;;) {
        auto* hdr = reinterpret_cast<DirEntryHeader*>(ptr);

        if ((hdr->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            hdr->FileNameLength > 0) {

            const wchar_t* fileName =
                reinterpret_cast<const wchar_t*>(ptr + fnOffset);
            ULONG nameChars = hdr->FileNameLength / sizeof(wchar_t);

            bool isDot = (nameChars == 1 && fileName[0] == L'.') ||
                         (nameChars == 2 && fileName[0] == L'.' && fileName[1] == L'.');

            if (!isDot) {
                std::wstring fullPath = dirPath + L"\\" +
                    std::wstring(fileName, nameChars);

                auto size = LookupDirSize(fullPath);
                if (size.has_value()) {
                    hdr->EndOfFile.QuadPart = static_cast<LONGLONG>(*size);
                    hdr->AllocationSize.QuadPart = static_cast<LONGLONG>(*size);
                }
            }
        }

        if (hdr->NextEntryOffset == 0) break;
        ptr += hdr->NextEntryOffset;
    }

    return status;
}

// ---------------------------------------------------------------------------
// IPropertyStore::GetValue hook — inject dir sizes at property level
// ---------------------------------------------------------------------------

typedef HRESULT(STDMETHODCALLTYPE* PropStoreGetValueFn)(
    void* pThis, REFPROPERTYKEY key, PROPVARIANT* ppropvar);
static PropStoreGetValueFn TruePropStoreGetValue = nullptr;

// Track which size values we injected for directories (per-thread).
// The format hook uses this to only auto-scale directory sizes, not files.
static thread_local std::optional<uint64_t> t_lastInjectedDirSize;

static HRESULT STDMETHODCALLTYPE HookedPropStoreGetValue(
    void* pThis, REFPROPERTYKEY key, PROPVARIANT* ppropvar)
{
    HRESULT hr = TruePropStoreGetValue(pThis, key, ppropvar);

    if (!IsEqualPropertyKey(key, PKEY_Size))
        return hr;

    // Intercept PKEY_Size returning VT_EMPTY (directories)
    if (SUCCEEDED(hr) && ppropvar &&
        (ppropvar->vt == VT_EMPTY || ppropvar->vt == VT_NULL)) {

        PROPVARIANT pathVar;
        PropVariantInit(&pathVar);
        HRESULT hrPath = TruePropStoreGetValue(pThis, PKEY_ParsingPath, &pathVar);
        if (FAILED(hrPath) || pathVar.vt != VT_LPWSTR) {
            PropVariantClear(&pathVar);
            PropVariantInit(&pathVar);
            hrPath = TruePropStoreGetValue(pThis, PKEY_ItemPathDisplay, &pathVar);
        }

        if (SUCCEEDED(hrPath) && pathVar.vt == VT_LPWSTR && pathVar.pwszVal) {
            std::wstring path(pathVar.pwszVal);

            DWORD attrs = GetFileAttributesW(path.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES &&
                (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                auto size = LookupDirSize(path);
                if (size.has_value()) {
                    PropVariantClear(ppropvar);
                    ppropvar->vt = VT_UI8;
                    ppropvar->uhVal.QuadPart = *size;
                    t_lastInjectedDirSize = *size;
                }
            }
        }
        PropVariantClear(&pathVar);
    }

    return hr;
}

// ---------------------------------------------------------------------------
// IPropertyDescription::FormatForDisplay hook — auto-scale size labels
// ---------------------------------------------------------------------------

// IPropertyDescription::FormatForDisplay is vtable[22]:
//   HRESULT FormatForDisplay(REFPROPVARIANT propvar,
//                            PROPDESC_FORMAT_FLAGS pdfFlags,
//                            LPWSTR* ppszDisplay);
typedef HRESULT(STDMETHODCALLTYPE* PropDescFormatForDisplayFn)(
    void* pThis, REFPROPVARIANT propvar, PROPDESC_FORMAT_FLAGS pdfFlags,
    LPWSTR* ppszDisplay);
static PropDescFormatForDisplayFn TruePropDescFormatForDisplay = nullptr;

static HRESULT STDMETHODCALLTYPE HookedPropDescFormatForDisplay(
    void* pThis, REFPROPVARIANT propvar, PROPDESC_FORMAT_FLAGS pdfFlags,
    LPWSTR* ppszDisplay)
{
    if (GetCurrentDisplayFormat() == DisplayFormat::AutoScale &&
        ppszDisplay && propvar.vt == VT_UI8) {

        bool shouldFormat = false;

        if (GetAutoScaleFoldersOnly()) {
            // Only format values we injected for directories
            if (t_lastInjectedDirSize.has_value() &&
                propvar.uhVal.QuadPart == *t_lastInjectedDirSize) {
                t_lastInjectedDirSize.reset();
                shouldFormat = true;
            }
        } else {
            // Format all PKEY_Size values (files + folders)
            IPropertyDescription* pDesc = reinterpret_cast<IPropertyDescription*>(pThis);
            PROPERTYKEY pk = {};
            if (SUCCEEDED(pDesc->GetPropertyKey(&pk)) &&
                IsEqualPropertyKey(pk, PKEY_Size)) {
                shouldFormat = true;
            }
        }

        if (shouldFormat) {
            LONGLONG bytes = static_cast<LONGLONG>(propvar.uhVal.QuadPart);
            wchar_t buf[64];
            if (StrFormatByteSizeW(bytes, buf, _countof(buf))) {
                size_t len = wcslen(buf) + 1;
                LPWSTR result = static_cast<LPWSTR>(
                    CoTaskMemAlloc(len * sizeof(wchar_t)));
                if (result) {
                    wcscpy_s(result, len, buf);
                    *ppszDisplay = result;
                    return S_OK;
                }
            }
        }
    }

    return TruePropDescFormatForDisplay(pThis, propvar, pdfFlags, ppszDisplay);
}

// ---------------------------------------------------------------------------
// Hook installation / removal
// ---------------------------------------------------------------------------

static std::once_flag g_hookOnce;
static std::atomic<bool> g_hookInstalled{false};

// The Detours hooks patch process-wide entry points (an ntdll syscall stub
// and shared COM vtables) and rewrite directory-entry sizes for EVERY
// caller in the process. That is intended solely for Explorer's folder
// views — but this DLL also gets loaded into any application that shows a
// common file dialog or activates our COM objects, and those applications
// (backup tools, sync clients, installers) must see real filesystem data.
static bool IsExplorerProcess() {
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, path, _countof(path));
    if (len == 0 || len >= _countof(path)) return false;
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    return _wcsicmp(name, L"explorer.exe") == 0;
}

void InstallShellHook() {
    // Never hook a foreign host process (see IsExplorerProcess). Checked
    // outside the once-flag so an early call from a non-Explorer process
    // doesn't permanently consume it.
    if (!IsExplorerProcess()) return;

    std::call_once(g_hookOnce, [] {
        DebugLog("InstallShellHook: starting\n");

        HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
        if (!hNtdll) return;

        TrueNtQueryDirectoryFile = reinterpret_cast<NtQueryDirectoryFileFn>(
            GetProcAddress(hNtdll, "NtQueryDirectoryFile"));
        if (!TrueNtQueryDirectoryFile) return;

        LONG error = DetourTransactionBegin();
        if (error != NO_ERROR) return;

        DetourUpdateThread(GetCurrentThread());

        // Hook 1: NtQueryDirectoryFile — inject sizes into raw NT listings
        DetourAttach(&(PVOID&)TrueNtQueryDirectoryFile,
                     HookedNtQueryDirectoryFile);

        // Hook 2: IPropertyDescription::FormatForDisplay — auto-scale formatting
        //   Get the IPropertyDescription for PKEY_Size, then hook vtable[22].
        {
            HRESULT hrCo2 = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            IPropertyDescription* pDesc = nullptr;
            HRESULT hrPD = PSGetPropertyDescription(
                PKEY_Size, IID_IPropertyDescription,
                reinterpret_cast<void**>(&pDesc));
            if (SUCCEEDED(hrPD) && pDesc) {
                void** vtable = *reinterpret_cast<void***>(pDesc);
                TruePropDescFormatForDisplay =
                    reinterpret_cast<PropDescFormatForDisplayFn>(vtable[22]);
                DetourAttach(&(PVOID&)TruePropDescFormatForDisplay,
                             HookedPropDescFormatForDisplay);
                DebugLog("Hooked IPropertyDescription::FormatForDisplay at %p\n",
                         TruePropDescFormatForDisplay);
                // NOTE: intentionally NOT releasing pDesc — we need it alive
                // for the identity check in the hook.
            }
            if (SUCCEEDED(hrCo2)) CoUninitialize();
        }

        // Hook 3: IPropertyStore::GetValue — inject sizes at property level
        {
            HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            IPropertyStore* pStore = nullptr;
            HRESULT hrPS = SHGetPropertyStoreFromParsingName(
                L"C:\\Windows", nullptr, GPS_DEFAULT,
                IID_IPropertyStore, reinterpret_cast<void**>(&pStore));
            if (SUCCEEDED(hrPS) && pStore) {
                void** vtable = *reinterpret_cast<void***>(pStore);
                TruePropStoreGetValue =
                    reinterpret_cast<PropStoreGetValueFn>(vtable[5]);
                DetourAttach(&(PVOID&)TruePropStoreGetValue,
                             HookedPropStoreGetValue);
                DebugLog("Hooked IPropertyStore::GetValue at %p\n",
                         TruePropStoreGetValue);
                pStore->Release();
            }
            if (SUCCEEDED(hrCo)) CoUninitialize();
        }

        error = DetourTransactionCommit();
        if (error == NO_ERROR) {
            g_hookInstalled.store(true);
            // Pin this DLL in the process. Once Detours has patched
            // ntdll/vtable entries, unloading the DLL (e.g. after
            // DllCanUnloadNow returned S_OK) would leave the patched
            // functions jumping into freed memory and crash Explorer.
            HMODULE self = nullptr;
            GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(&InstallShellHook), &self);
            DebugLog("InstallShellHook: all hooks installed OK\n");
        } else {
            DebugLog("InstallShellHook: DetourTransactionCommit failed: %ld\n",
                     error);
        }
    });
}

bool ShellHookActive() {
    return g_hookInstalled.load();
}

void RemoveShellHook() {
    if (!g_hookInstalled.load()) return;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if (TrueNtQueryDirectoryFile)
        DetourDetach(&(PVOID&)TrueNtQueryDirectoryFile,
                     HookedNtQueryDirectoryFile);
    if (TruePropDescFormatForDisplay)
        DetourDetach(&(PVOID&)TruePropDescFormatForDisplay,
                     HookedPropDescFormatForDisplay);
    if (TruePropStoreGetValue)
        DetourDetach(&(PVOID&)TruePropStoreGetValue,
                     HookedPropStoreGetValue);
    DetourTransactionCommit();

    g_hookInstalled.store(false);
}

// ---------------------------------------------------------------------------
// DirSizeOverlay — dummy icon overlay to force DLL load at Explorer startup
// ---------------------------------------------------------------------------

DirSizeOverlay::DirSizeOverlay() {
    DllAddRef();
    InstallShellHook();
}

DirSizeOverlay::~DirSizeOverlay() {
    DllRelease();
}

HRESULT DirSizeOverlay::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;

    if (riid == IID_IUnknown) {
        *ppv = static_cast<IShellIconOverlayIdentifier*>(this);
    } else if (riid == IID_IShellIconOverlayIdentifier) {
        *ppv = static_cast<IShellIconOverlayIdentifier*>(this);
    } else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
}

ULONG DirSizeOverlay::AddRef() {
    return InterlockedIncrement(&m_refCount);
}

ULONG DirSizeOverlay::Release() {
    ULONG count = InterlockedDecrement(&m_refCount);
    if (count == 0) delete this;
    return count;
}

HRESULT DirSizeOverlay::IsMemberOf(LPCWSTR, DWORD) {
    return S_FALSE;
}

HRESULT DirSizeOverlay::GetOverlayInfo(LPWSTR pwszIconFile, int cchMax,
                                        int* pIndex, DWORD* pdwFlags) {
    if (!pwszIconFile || !pIndex || !pdwFlags) return E_POINTER;
    GetModuleFileNameW(nullptr, pwszIconFile, cchMax);
    *pIndex = 0;
    *pdwFlags = ISIOI_ICONFILE | ISIOI_ICONINDEX;
    return S_OK;
}

HRESULT DirSizeOverlay::GetPriority(int* pPriority) {
    if (!pPriority) return E_POINTER;
    *pPriority = 100;
    return S_OK;
}

} // namespace dirsize
