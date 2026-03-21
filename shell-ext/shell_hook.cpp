#include "shell_hook.h"
#include "property_handler.h"
#include "dirsize/config.h"
#include "dirsize/guids.h"

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
        f = _wfopen(L"C:\\ProgramData\\DirSizeForExplorer\\hook_debug.log", L"w");
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

static Database& GetHookDb() {
    static Database db;
    static std::once_flag initFlag;
    std::call_once(initFlag, [] {
        db.Open(Database::GetDefaultPath(), true /* readOnly */);
    });
    return db;
}

static std::wstring NormalizePath(const wchar_t* path) {
    std::wstring result(path);
    std::replace(result.begin(), result.end(), L'/', L'\\');
    while (result.size() > 3 && result.back() == L'\\')
        result.pop_back();
    std::transform(result.begin(), result.end(), result.begin(), ::towlower);
    return result;
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

static uint64_t LookupDirSize(const std::wstring& fullPath) {
    std::wstring normalized = NormalizePath(fullPath.c_str());
    SizeMetric metric = GetCurrentSizeMetric();

    if (metric == SizeMetric::AllocationSize) {
        auto cached = SizeCache::Instance().GetAlloc(normalized);
        if (cached) return *cached;
    } else {
        auto cached = SizeCache::Instance().Get(normalized);
        if (cached) return *cached;
    }

    auto& db = GetHookDb();
    auto entry = db.GetEntry(normalized);
    if (entry) {
        SizeCache::Instance().Put(normalized, entry->totalSize, entry->allocSize);
        return (metric == SizeMetric::AllocationSize) ? entry->allocSize : entry->totalSize;
    }

    return 0;
}

// ---------------------------------------------------------------------------
// Handle → directory path resolution
// ---------------------------------------------------------------------------

static std::wstring GetDirForHandle(HANDLE h) {
    wchar_t buf[MAX_PATH + 4];
    DWORD len = GetFinalPathNameByHandleW(h, buf, _countof(buf),
                                           FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (len == 0 || len >= _countof(buf))
        return {};

    std::wstring result(buf);
    if (result.size() > 4 && result.substr(0, 4) == L"\\\\?\\")
        result = result.substr(4);

    return result;
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
    case kFileDirectoryInformation:       return 64;
    case kFileFullDirectoryInformation:   return 68;
    case kFileBothDirectoryInformation:   return 94;
    case kFileIdBothDirectoryInformation: return 104;
    case kFileIdFullDirectoryInformation: return 72;
    case kFileIdExtdDirectoryInformation: return 120;
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

                uint64_t size = LookupDirSize(fullPath);
                if (size > 0) {
                    hdr->EndOfFile.QuadPart = static_cast<LONGLONG>(size);
                    hdr->AllocationSize.QuadPart = static_cast<LONGLONG>(size);
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
static thread_local uint64_t t_lastInjectedDirSize = 0;

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
                uint64_t size = LookupDirSize(path);
                if (size > 0) {
                    PropVariantClear(ppropvar);
                    ppropvar->vt = VT_UI8;
                    ppropvar->uhVal.QuadPart = size;
                    t_lastInjectedDirSize = size;
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
            if (t_lastInjectedDirSize != 0 &&
                propvar.uhVal.QuadPart == t_lastInjectedDirSize) {
                t_lastInjectedDirSize = 0;
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

void InstallShellHook() {
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
            DebugLog("InstallShellHook: all hooks installed OK\n");
        } else {
            DebugLog("InstallShellHook: DetourTransactionCommit failed: %ld\n",
                     error);
        }
    });
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
