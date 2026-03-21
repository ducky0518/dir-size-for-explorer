// Diagnostic: check DB contents and test if the vtable hook is working.

#include "dirsize/db.h"
#include <Windows.h>
#include <ShlObj.h>
#include <Shlwapi.h>
#include <propkey.h>
#include <OleAuto.h>
#include <iostream>
#include <string>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "oleaut32.lib")

int wmain() {
    // --- Part 1: Check database contents ---
    std::wcout << L"=== Database Check ===" << std::endl;
    dirsize::Database db;
    auto dbPath = dirsize::Database::GetDefaultPath();
    std::wcout << L"DB path: " << dbPath << std::endl;

    if (!db.Open(dbPath, true /* readOnly */)) {
        std::wcerr << L"Failed to open database read-only!" << std::endl;
    } else {
        // Check a few known paths
        const wchar_t* testPaths[] = {
            L"c:\\users\\laptop\\desktop",
            L"c:\\users\\laptop\\desktop\\7zchd",
            L"c:\\users\\laptop\\desktop\\dir-size-for-explorer",
            L"c:\\users\\laptop\\desktop\\games1",
        };

        for (auto path : testPaths) {
            auto size = db.GetSize(path);
            if (size) {
                std::wcout << L"  " << path << L" => " << *size << L" bytes" << std::endl;
            } else {
                std::wcout << L"  " << path << L" => NOT FOUND" << std::endl;
            }
        }
        db.Close();
    }

    // --- Part 2: Test IShellFolder2::GetDetailsEx hook ---
    std::wcout << L"\n=== Hook Test ===" << std::endl;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    IShellFolder* pDesktop = nullptr;
    if (FAILED(SHGetDesktopFolder(&pDesktop))) {
        std::wcerr << L"Failed to get desktop folder" << std::endl;
        CoUninitialize();
        return 1;
    }

    // Bind to C:\Users\laptop to get its IShellFolder2
    PIDLIST_ABSOLUTE pidlParent = nullptr;
    HRESULT hr = SHParseDisplayName(L"C:\\Users\\laptop", nullptr, &pidlParent, 0, nullptr);
    if (FAILED(hr)) {
        std::wcerr << L"Failed to parse C:\\Users\\laptop" << std::endl;
        pDesktop->Release();
        CoUninitialize();
        return 1;
    }

    IShellFolder2* pFolder2 = nullptr;
    hr = pDesktop->BindToObject(pidlParent, nullptr, IID_IShellFolder2,
                                 reinterpret_cast<void**>(&pFolder2));
    CoTaskMemFree(pidlParent);
    pDesktop->Release();

    if (FAILED(hr) || !pFolder2) {
        std::wcerr << L"Failed to bind to C:\\Users\\laptop: 0x" << std::hex << hr << std::endl;
        CoUninitialize();
        return 1;
    }

    std::wcout << L"Got IShellFolder2 for C:\\Users\\laptop" << std::endl;

    // Now enumerate children and try GetDetailsEx for PKEY_Size on "Desktop"
    PIDLIST_RELATIVE pidlChild = nullptr;
    hr = SHParseDisplayName(L"C:\\Users\\laptop\\Desktop", nullptr,
                             reinterpret_cast<PIDLIST_ABSOLUTE*>(&pidlChild), 0, nullptr);

    // We need the child-relative PIDL. Let's enumerate instead.
    IEnumIDList* pEnum = nullptr;
    hr = pFolder2->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &pEnum);
    if (SUCCEEDED(hr) && pEnum) {
        PITEMID_CHILD pidlItem = nullptr;
        ULONG fetched = 0;
        int tested = 0;

        while (pEnum->Next(1, &pidlItem, &fetched) == S_OK && fetched == 1) {
            // Get display name
            STRRET strret = {};
            hr = pFolder2->GetDisplayNameOf(pidlItem, SHGDN_FORPARSING, &strret);
            if (SUCCEEDED(hr)) {
                wchar_t szName[MAX_PATH];
                StrRetToBufW(&strret, pidlItem, szName, MAX_PATH);

                // Test Desktop and Documents
                std::wstring name(szName);
                bool isInteresting = (name.find(L"Desktop") != std::wstring::npos ||
                                      name.find(L"Documents") != std::wstring::npos ||
                                      name.find(L"Downloads") != std::wstring::npos);

                if (isInteresting) {
                    // Check if it's a directory
                    DWORD attrs = GetFileAttributesW(szName);
                    bool isDir = (attrs != INVALID_FILE_ATTRIBUTES &&
                                  (attrs & FILE_ATTRIBUTE_DIRECTORY));

                    // Call GetDetailsEx for PKEY_Size
                    VARIANT var;
                    VariantInit(&var);
                    SHCOLUMNID scid;
                    scid.fmtid = PKEY_Size.fmtid;
                    scid.pid = PKEY_Size.pid;

                    hr = pFolder2->GetDetailsEx(pidlItem, &scid, &var);
                    std::wcout << L"  " << szName
                               << L" [" << (isDir ? L"DIR" : L"FILE") << L"]"
                               << L" GetDetailsEx=0x" << std::hex << hr;

                    if (SUCCEEDED(hr) && var.vt == VT_UI8) {
                        std::wcout << L" SIZE=" << std::dec << var.ullVal << L" bytes";
                    } else if (SUCCEEDED(hr) && var.vt == VT_EMPTY) {
                        std::wcout << L" EMPTY (no size)";
                    } else {
                        std::wcout << L" vt=" << std::dec << var.vt;
                    }
                    std::wcout << std::endl;

                    VariantClear(&var);
                    tested++;
                }
            }

            CoTaskMemFree(pidlItem);
        }
        pEnum->Release();

        if (tested == 0) {
            std::wcout << L"  (no matching items found in enumeration)" << std::endl;
        }
    } else {
        std::wcerr << L"Failed to enumerate: 0x" << std::hex << hr << std::endl;
    }

    pFolder2->Release();
    CoUninitialize();

    std::wcout << L"\nDone." << std::endl;
    return 0;
}
