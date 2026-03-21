#include "class_factory.h"
#include "property_handler.h"
#include "context_menu.h"
#include "shell_hook.h"
#include "dirsize/guids.h"

#include <new>

HMODULE g_hModule = nullptr;

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*reserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
    }
    if (reason == DLL_PROCESS_DETACH) {
        dirsize::RemoveShellHook();
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    // Install the vtable hook on any COM creation (idempotent via call_once)
    dirsize::InstallShellHook();

    if (rclsid == CLSID_DirSizePropertyHandler) {
        auto* factory = new (std::nothrow)
            dirsize::ClassFactory<dirsize::DirSizePropertyHandler>();
        if (!factory) return E_OUTOFMEMORY;
        HRESULT hr = factory->QueryInterface(riid, ppv);
        factory->Release();
        return hr;
    }

    if (rclsid == CLSID_DirSizeContextMenu) {
        auto* factory = new (std::nothrow)
            dirsize::ClassFactory<dirsize::DirSizeContextMenu>();
        if (!factory) return E_OUTOFMEMORY;
        HRESULT hr = factory->QueryInterface(riid, ppv);
        factory->Release();
        return hr;
    }

    if (rclsid == CLSID_DirSizeOverlay) {
        auto* factory = new (std::nothrow)
            dirsize::ClassFactory<dirsize::DirSizeOverlay>();
        if (!factory) return E_OUTOFMEMORY;
        HRESULT hr = factory->QueryInterface(riid, ppv);
        factory->Release();
        return hr;
    }

    return CLASS_E_CLASSNOTAVAILABLE;
}

STDAPI DllCanUnloadNow() {
    return (dirsize::g_dllRefCount == 0) ? S_OK : S_FALSE;
}
