// Try different registration approaches to find one Explorer actually uses.
// Run as admin.

#include <Windows.h>
#include <propsys.h>
#include <propkey.h>
#include <shlobj.h>
#include <iostream>
#include <string>

#include "dirsize/guids.h"

#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

static std::wstring GuidToString(REFGUID guid) {
    wchar_t buf[64];
    StringFromGUID2(guid, buf, _countof(buf));
    return buf;
}

static void SetRegKey(HKEY root, const std::wstring& path, const std::wstring& value) {
    HKEY hKey;
    if (RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_WRITE,
                        nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, nullptr, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(value.c_str()),
                       static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(hKey);
        std::wcout << L"  Set: " << path << L" = " << value << std::endl;
    } else {
        std::wcout << L"  FAILED: " << path << std::endl;
    }
}

int wmain() {
    std::wstring clsid = GuidToString(CLSID_DirSizePropertyHandler);

    std::wcout << L"Trying additional registration paths for: " << clsid << std::endl;

    // Approach 1: Register under PropertySystem PropertyHandlers for Directory
    std::wcout << L"\n1. PropertySystem PropertyHandlers\\.directory" << std::endl;
    SetRegKey(HKEY_LOCAL_MACHINE,
              L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\PropertySystem\\PropertyHandlers\\.directory",
              clsid);

    // Approach 2: Register under Folder (not just Directory)
    std::wcout << L"\n2. Folder\\ShellEx\\PropertyHandler" << std::endl;
    SetRegKey(HKEY_CLASSES_ROOT, L"Folder\\ShellEx\\PropertyHandler", clsid);

    // Approach 3: Register under AllFilesystemObjects
    std::wcout << L"\n3. AllFilesystemObjects\\ShellEx\\PropertyHandler" << std::endl;
    SetRegKey(HKEY_CLASSES_ROOT, L"AllFilesystemObjects\\ShellEx\\PropertyHandler", clsid);

    // Approach 4: Register under CLSID of Directory
    // The shell object type for directories is {FE1290F0-CFBD-11CF-A330-00AA00C16E65}
    std::wcout << L"\n4. CLSID\\{FE1290F0-CFBD-11CF-A330-00AA00C16E65}\\ShellEx\\PropertyHandler" << std::endl;
    SetRegKey(HKEY_CLASSES_ROOT,
              L"CLSID\\{FE1290F0-CFBD-11CF-A330-00AA00C16E65}\\ShellEx\\PropertyHandler",
              clsid);

    // Notify Explorer
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);

    std::wcout << L"\nDone. Restart Explorer and check if Size column shows for folders." << std::endl;
    std::wcout << L"If none work, we'll need the IShellFolder2 hook approach." << std::endl;

    return 0;
}
