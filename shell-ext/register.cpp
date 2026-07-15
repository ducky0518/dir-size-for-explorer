#include "dirsize/guids.h"
#include "property_handler.h"

#include <Windows.h>
#include <olectl.h> // SELFREG_E_CLASS
#include <propsys.h>
#include <strsafe.h>
#include <shlobj.h>

#include <string>

extern HMODULE g_hModule;

namespace {

// Convert a GUID to its string representation {xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}
std::wstring GuidToString(REFGUID guid) {
    wchar_t buf[64];
    StringFromGUID2(guid, buf, _countof(buf));
    return buf;
}

// Get the full path to this DLL
std::wstring GetModulePath() {
    wchar_t path[MAX_PATH * 2];
    GetModuleFileNameW(g_hModule, path, _countof(path));
    return path;
}

// Get the directory containing this DLL
std::wstring GetModuleDir() {
    std::wstring path = GetModulePath();
    auto pos = path.find_last_of(L'\\');
    if (pos != std::wstring::npos) {
        return path.substr(0, pos);
    }
    return path;
}

// Helper to create a registry key and set its default value
LSTATUS CreateRegKeyWithDefault(HKEY hRoot, const std::wstring& subKey,
                                const std::wstring& value) {
    HKEY hKey;
    LSTATUS status = RegCreateKeyExW(hRoot, subKey.c_str(), 0, nullptr,
                                     REG_OPTION_NON_VOLATILE, KEY_WRITE,
                                     nullptr, &hKey, nullptr);
    if (status != ERROR_SUCCESS) return status;

    if (!value.empty()) {
        status = RegSetValueExW(hKey, nullptr, 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(value.c_str()),
                                static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    }

    RegCloseKey(hKey);
    return status;
}

// Helper to set a named string value on an existing key
LSTATUS SetRegValue(HKEY hRoot, const std::wstring& subKey,
                    const std::wstring& valueName, const std::wstring& value) {
    HKEY hKey;
    LSTATUS status = RegOpenKeyExW(hRoot, subKey.c_str(), 0, KEY_WRITE, &hKey);
    if (status != ERROR_SUCCESS) return status;

    status = RegSetValueExW(hKey, valueName.c_str(), 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(value.c_str()),
                            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(hKey);
    return status;
}

// Recursively delete a registry key tree
LSTATUS DeleteRegKeyTree(HKEY hRoot, const std::wstring& subKey) {
    return RegDeleteTreeW(hRoot, subKey.c_str());
}

// Where a displaced third-party Directory property handler is remembered
// between DllRegisterServer and DllUnregisterServer.
constexpr wchar_t kOwnKey[] = L"SOFTWARE\\DirSizeForExplorer";
constexpr wchar_t kSavedHandlerValue[] = L"PreviousDirectoryPropertyHandler";
constexpr wchar_t kDirPropertyHandlerKey[] =
    L"Directory\\ShellEx\\PropertyHandler";

// Read the current Directory property-handler CLSID (empty if none).
std::wstring ReadCurrentDirPropertyHandler() {
    wchar_t buf[64] = {};
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CLASSES_ROOT, kDirPropertyHandlerKey, nullptr,
                     RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) {
        return {};
    }
    return buf;
}

} // namespace

// Called by regsvr32 or the installer to register the shell extensions.
// Failures are REPORTED (SELFREG_E_CLASS → nonzero regsvr32 exit code) so
// the installer can fail and roll back instead of reporting a successful
// install whose Explorer integration silently doesn't work.
STDAPI DllRegisterServer() {
    std::wstring dllPath = GetModulePath();
    std::wstring propHandlerClsid = GuidToString(CLSID_DirSizePropertyHandler);
    std::wstring contextMenuClsid = GuidToString(CLSID_DirSizeContextMenu);
    std::wstring overlayClsid = GuidToString(CLSID_DirSizeOverlay);

    bool ok = true;
    auto check = [&ok](LSTATUS status) { if (status != ERROR_SUCCESS) ok = false; };

    // --- Property Handler CLSID ---
    std::wstring keyPath = L"CLSID\\" + propHandlerClsid;
    check(CreateRegKeyWithDefault(HKEY_CLASSES_ROOT, keyPath, L"DirSize Property Handler"));
    check(CreateRegKeyWithDefault(HKEY_CLASSES_ROOT, keyPath + L"\\InprocServer32", dllPath));
    check(SetRegValue(HKEY_CLASSES_ROOT, keyPath + L"\\InprocServer32",
                      L"ThreadingModel", L"Both"));

    // --- Context Menu CLSID ---
    keyPath = L"CLSID\\" + contextMenuClsid;
    check(CreateRegKeyWithDefault(HKEY_CLASSES_ROOT, keyPath, L"DirSize Context Menu"));
    check(CreateRegKeyWithDefault(HKEY_CLASSES_ROOT, keyPath + L"\\InprocServer32", dllPath));
    check(SetRegValue(HKEY_CLASSES_ROOT, keyPath + L"\\InprocServer32",
                      L"ThreadingModel", L"Apartment"));

    // --- Register property handler for Directory ---
    // This default value is a machine-wide singleton another product may
    // already own. Remember whatever we displace so DllUnregisterServer
    // can put it back instead of leaving the slot empty (or worse,
    // deleting the other product's live registration). A repair reruns
    // this with OUR CLSID already in place — the saved value is kept.
    {
        std::wstring existing = ReadCurrentDirPropertyHandler();
        if (!existing.empty() &&
            _wcsicmp(existing.c_str(), propHandlerClsid.c_str()) != 0) {
            HKEY hOwn = nullptr;
            if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kOwnKey, 0, nullptr,
                                REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr,
                                &hOwn, nullptr) == ERROR_SUCCESS) {
                RegSetValueExW(hOwn, kSavedHandlerValue, 0, REG_SZ,
                               reinterpret_cast<const BYTE*>(existing.c_str()),
                               static_cast<DWORD>((existing.size() + 1) * sizeof(wchar_t)));
                RegCloseKey(hOwn);
            }
        }
    }
    check(CreateRegKeyWithDefault(HKEY_CLASSES_ROOT,
                                  kDirPropertyHandlerKey,
                                  propHandlerClsid));

    // --- Register context menu handler for Directory ---
    check(CreateRegKeyWithDefault(HKEY_CLASSES_ROOT,
                                  L"Directory\\ShellEx\\ContextMenuHandlers\\DirSize",
                                  contextMenuClsid));

    // --- Also register for Folder (covers virtual folders) ---
    check(CreateRegKeyWithDefault(HKEY_CLASSES_ROOT,
                                  L"Folder\\ShellEx\\ContextMenuHandlers\\DirSize",
                                  contextMenuClsid));

    // --- Icon Overlay CLSID (ensures DLL loads into Explorer at startup) ---
    keyPath = L"CLSID\\" + overlayClsid;
    check(CreateRegKeyWithDefault(HKEY_CLASSES_ROOT, keyPath, L"DirSize Overlay"));
    check(CreateRegKeyWithDefault(HKEY_CLASSES_ROOT, keyPath + L"\\InprocServer32", dllPath));
    check(SetRegValue(HKEY_CLASSES_ROOT, keyPath + L"\\InprocServer32",
                      L"ThreadingModel", L"Apartment"));

    // --- Register icon overlay handler (loaded at Explorer startup) ---
    // Space prefix ensures we sort near the top (Explorer only loads ~15 overlays)
    check(CreateRegKeyWithDefault(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\ShellIconOverlayIdentifiers\\   DirSize",
        overlayClsid));

    // --- Approve the extensions ---
    std::wstring approvedKey =
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved";
    HKEY hApproved;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, approvedKey.c_str(), 0, KEY_WRITE,
                      &hApproved) == ERROR_SUCCESS) {
        auto setApproval = [&](const std::wstring& clsid, const wchar_t* name) {
            check(RegSetValueExW(hApproved, clsid.c_str(), 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(name),
                           static_cast<DWORD>((wcslen(name) + 1) * sizeof(wchar_t))));
        };
        setApproval(propHandlerClsid, L"DirSize Property Handler");
        setApproval(contextMenuClsid, L"DirSize Context Menu");
        setApproval(overlayClsid, L"DirSize Overlay");
        RegCloseKey(hApproved);
    } else {
        ok = false;
    }

    // --- Register the property schema ---
    // This registration is what makes the Size column render our values —
    // it silently failing for two releases is why registration errors are
    // no longer swallowed anywhere in this function.
    std::wstring propdescPath = GetModuleDir() + L"\\DirSizeTotalSize.propdesc";
    HRESULT hrSchema = PSRegisterPropertySchema(propdescPath.c_str());
    if (FAILED(hrSchema)) {
        ok = false;
    }

    // Notify Explorer of the association change
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);

    return ok ? S_OK : SELFREG_E_CLASS;
}

// Called to unregister the shell extensions.
STDAPI DllUnregisterServer() {
    std::wstring propHandlerClsid = GuidToString(CLSID_DirSizePropertyHandler);
    std::wstring contextMenuClsid = GuidToString(CLSID_DirSizeContextMenu);
    std::wstring overlayClsid = GuidToString(CLSID_DirSizeOverlay);

    // Unregister property schema
    std::wstring propdescPath = GetModuleDir() + L"\\DirSizeTotalSize.propdesc";
    PSUnregisterPropertySchema(propdescPath.c_str());

    // Remove registry entries
    DeleteRegKeyTree(HKEY_CLASSES_ROOT, L"CLSID\\" + propHandlerClsid);
    DeleteRegKeyTree(HKEY_CLASSES_ROOT, L"CLSID\\" + contextMenuClsid);
    DeleteRegKeyTree(HKEY_CLASSES_ROOT, L"CLSID\\" + overlayClsid);

    // Remove icon overlay registration
    DeleteRegKeyTree(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\ShellIconOverlayIdentifiers\\   DirSize");

    // Directory property handler: only touch the value if it still points
    // at US (another product may have taken the slot since install — that
    // registration must be left alone). If we displaced a handler at
    // install time, put it back; otherwise clear the value.
    {
        std::wstring current = ReadCurrentDirPropertyHandler();
        if (!current.empty() &&
            _wcsicmp(current.c_str(), propHandlerClsid.c_str()) == 0) {
            wchar_t saved[64] = {};
            DWORD savedSize = sizeof(saved);
            bool haveSaved = RegGetValueW(
                HKEY_LOCAL_MACHINE, kOwnKey, kSavedHandlerValue,
                RRF_RT_REG_SZ, nullptr, saved, &savedSize) == ERROR_SUCCESS &&
                saved[0] != L'\0';

            HKEY hPropHandler;
            if (RegOpenKeyExW(HKEY_CLASSES_ROOT, kDirPropertyHandlerKey,
                              0, KEY_WRITE, &hPropHandler) == ERROR_SUCCESS) {
                if (haveSaved) {
                    RegSetValueExW(hPropHandler, nullptr, 0, REG_SZ,
                                   reinterpret_cast<const BYTE*>(saved),
                                   static_cast<DWORD>((wcslen(saved) + 1) * sizeof(wchar_t)));
                } else {
                    RegDeleteValueW(hPropHandler, nullptr);
                }
                RegCloseKey(hPropHandler);
            }
        }
        // Drop the saved-handler bookkeeping either way.
        RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, kOwnKey, kSavedHandlerValue);
    }

    // Remove context menu registrations
    DeleteRegKeyTree(HKEY_CLASSES_ROOT,
                     L"Directory\\ShellEx\\ContextMenuHandlers\\DirSize");
    DeleteRegKeyTree(HKEY_CLASSES_ROOT,
                     L"Folder\\ShellEx\\ContextMenuHandlers\\DirSize");

    // Remove approvals
    std::wstring approvedKey =
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved";
    HKEY hApproved;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, approvedKey.c_str(), 0, KEY_WRITE,
                      &hApproved) == ERROR_SUCCESS) {
        RegDeleteValueW(hApproved, propHandlerClsid.c_str());
        RegDeleteValueW(hApproved, contextMenuClsid.c_str());
        RegDeleteValueW(hApproved, overlayClsid.c_str());
        RegCloseKey(hApproved);
    }

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);

    return S_OK;
}
