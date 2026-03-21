#pragma once

#include <Windows.h>
#include <Unknwn.h>
#include <shlobj.h>

namespace dirsize {

// Install the IShellFolder2::GetDetailsEx hook.
// Must be called after COM is initialized (not from DllMain).
// Thread-safe: only installs once.
void InstallShellHook();

// Remove the hook (called during DLL unload).
void RemoveShellHook();

// Minimal icon overlay handler that exists solely to get our DLL
// loaded into Explorer's process at startup. IsMemberOf always
// returns S_FALSE so no overlay icons are ever shown.
class DirSizeOverlay : public IShellIconOverlayIdentifier {
public:
    DirSizeOverlay();
    ~DirSizeOverlay();

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // IShellIconOverlayIdentifier
    HRESULT STDMETHODCALLTYPE IsMemberOf(LPCWSTR pwszPath, DWORD dwAttrib) override;
    HRESULT STDMETHODCALLTYPE GetOverlayInfo(LPWSTR pwszIconFile, int cchMax,
                                              int* pIndex, DWORD* pdwFlags) override;
    HRESULT STDMETHODCALLTYPE GetPriority(int* pPriority) override;

private:
    LONG m_refCount = 1;
};

} // namespace dirsize
