// Debug tool: checks if property schema registration works
// and if our COM object can be instantiated.

#include <Windows.h>
#include <propsys.h>
#include <propkey.h>
#include <shlobj.h>
#include <iostream>
#include <string>

#include "dirsize/guids.h"

#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "ole32.lib")

int wmain() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // 1. Check property schema registration
    std::wstring propdescPath = L"C:\\Users\\laptop\\Desktop\\dir-size-for-explorer\\build\\shell-ext\\Debug\\DirSizeTotalSize.propdesc";
    HRESULT hr = PSRegisterPropertySchema(propdescPath.c_str());
    std::wcout << L"PSRegisterPropertySchema: 0x" << std::hex << hr
               << (SUCCEEDED(hr) ? L" (OK)" : L" (FAILED)") << std::endl;

    // 2. Try to instantiate our property handler via COM
    IPropertyStore* pStore = nullptr;
    hr = CoCreateInstance(CLSID_DirSizePropertyHandler, nullptr,
                          CLSCTX_INPROC_SERVER, IID_IPropertyStore,
                          reinterpret_cast<void**>(&pStore));
    std::wcout << L"CoCreateInstance PropertyHandler: 0x" << std::hex << hr
               << (SUCCEEDED(hr) ? L" (OK)" : L" (FAILED)") << std::endl;

    if (pStore) {
        // 3. Try initializing it with a directory path
        IInitializeWithFile* pInit = nullptr;
        hr = pStore->QueryInterface(IID_IInitializeWithFile,
                                     reinterpret_cast<void**>(&pInit));
        std::wcout << L"QueryInterface IInitializeWithFile: 0x" << std::hex << hr
                   << (SUCCEEDED(hr) ? L" (OK)" : L" (FAILED)") << std::endl;

        if (pInit) {
            hr = pInit->Initialize(L"C:\\Users\\laptop\\Desktop", STGM_READ);
            std::wcout << L"Initialize with Desktop path: 0x" << std::hex << hr
                       << (SUCCEEDED(hr) ? L" (OK)" : L" (FAILED)") << std::endl;

            // 4. Try getting PKEY_Size
            PROPVARIANT pv;
            PropVariantInit(&pv);
            hr = pStore->GetValue(PKEY_Size, &pv);
            std::wcout << L"GetValue PKEY_Size: 0x" << std::hex << hr;
            if (SUCCEEDED(hr) && pv.vt == VT_UI8) {
                std::wcout << L" = " << std::dec << pv.uhVal.QuadPart << L" bytes" << std::endl;
            } else {
                std::wcout << L" vt=" << std::dec << pv.vt << L" (empty or failed)" << std::endl;
            }
            PropVariantClear(&pv);
            pInit->Release();
        }
        pStore->Release();
    }

    // 5. Check registry to see if our handler is registered
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, L"Directory\\ShellEx\\PropertyHandler",
                      0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t value[256];
        DWORD size = sizeof(value);
        if (RegQueryValueExW(hKey, nullptr, nullptr, nullptr,
                             reinterpret_cast<BYTE*>(value), &size) == ERROR_SUCCESS) {
            std::wcout << L"Directory PropertyHandler registered: " << value << std::endl;
        } else {
            std::wcout << L"Directory PropertyHandler key exists but no default value" << std::endl;
        }
        RegCloseKey(hKey);
    } else {
        std::wcout << L"Directory PropertyHandler key NOT FOUND" << std::endl;
    }

    CoUninitialize();
    return 0;
}
