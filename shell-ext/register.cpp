#include "dirsize/guids.h"
#include "property_handler.h"

#include <Windows.h>
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

} // namespace

// Called by regsvr32 or the installer to register the shell extensions.
STDAPI DllRegisterServer() {
    std::wstring dllPath = GetModulePath();
    std::wstring propHandlerClsid = GuidToString(CLSID_DirSizePropertyHandler);
    std::wstring contextMenuClsid = GuidToString(CLSID_DirSizeContextMenu);
    std::wstring overlayClsid = GuidToString(CLSID_DirSizeOverlay);

    // --- Property Handler CLSID ---
    std::wstring keyPath = L"CLSID\\" + propHandlerClsid;
    CreateRegKeyWithDefault(HKEY_CLASSES_ROOT, keyPath, L"DirSize Property Handler");
    CreateRegKeyWithDefault(HKEY_CLASSES_ROOT, keyPath + L"\\InprocServer32", dllPath);
    SetRegValue(HKEY_CLASSES_ROOT, keyPath + L"\\InprocServer32",
                L"ThreadingModel", L"Both");

    // --- Context Menu CLSID ---
    keyPath = L"CLSID\\" + contextMenuClsid;
    CreateRegKeyWithDefault(HKEY_CLASSES_ROOT, keyPath, L"DirSize Context Menu");
    CreateRegKeyWithDefault(HKEY_CLASSES_ROOT, keyPath + L"\\InprocServer32", dllPath);
    SetRegValue(HKEY_CLASSES_ROOT, keyPath + L"\\InprocServer32",
                L"ThreadingModel", L"Apartment");

    // --- Register property handler for Directory ---
    CreateRegKeyWithDefault(HKEY_CLASSES_ROOT,
                            L"Directory\\ShellEx\\PropertyHandler",
                            propHandlerClsid);

    // --- Register context menu handler for Directory ---
    CreateRegKeyWithDefault(HKEY_CLASSES_ROOT,
                            L"Directory\\ShellEx\\ContextMenuHandlers\\DirSize",
                            contextMenuClsid);

    // --- Also register for Folder (covers virtual folders) ---
    CreateRegKeyWithDefault(HKEY_CLASSES_ROOT,
                            L"Folder\\ShellEx\\ContextMenuHandlers\\DirSize",
                            contextMenuClsid);

    // --- Icon Overlay CLSID (ensures DLL loads into Explorer at startup) ---
    keyPath = L"CLSID\\" + overlayClsid;
    CreateRegKeyWithDefault(HKEY_CLASSES_ROOT, keyPath, L"DirSize Overlay");
    CreateRegKeyWithDefault(HKEY_CLASSES_ROOT, keyPath + L"\\InprocServer32", dllPath);
    SetRegValue(HKEY_CLASSES_ROOT, keyPath + L"\\InprocServer32",
                L"ThreadingModel", L"Apartment");

    // --- Register icon overlay handler (loaded at Explorer startup) ---
    // Space prefix ensures we sort near the top (Explorer only loads ~15 overlays)
    CreateRegKeyWithDefault(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\ShellIconOverlayIdentifiers\\   DirSize",
        overlayClsid);

    // --- Approve the extensions ---
    std::wstring approvedKey =
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved";
    HKEY hApproved;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, approvedKey.c_str(), 0, KEY_WRITE,
                      &hApproved) == ERROR_SUCCESS) {
        auto setApproval = [&](const std::wstring& clsid, const wchar_t* name) {
            RegSetValueExW(hApproved, clsid.c_str(), 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(name),
                           static_cast<DWORD>((wcslen(name) + 1) * sizeof(wchar_t)));
        };
        setApproval(propHandlerClsid, L"DirSize Property Handler");
        setApproval(contextMenuClsid, L"DirSize Context Menu");
        setApproval(overlayClsid, L"DirSize Overlay");
        RegCloseKey(hApproved);
    }

    // --- Register the property schema ---
    std::wstring propdescPath = GetModuleDir() + L"\\DirSizeTotalSize.propdesc";
    HRESULT hr = PSRegisterPropertySchema(propdescPath.c_str());
    if (FAILED(hr)) {
        // Not fatal — the column just won't appear in the chooser
    }

    // Notify Explorer of the association change
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);

    return S_OK;
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

    // Remove property handler default value (our CLSID)
    HKEY hPropHandler;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, L"Directory\\ShellEx\\PropertyHandler",
                      0, KEY_WRITE, &hPropHandler) == ERROR_SUCCESS) {
        RegDeleteValueW(hPropHandler, nullptr); // Delete the (Default) value
        RegCloseKey(hPropHandler);
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
