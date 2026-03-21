#pragma once

#include <Unknwn.h>
#include <ShlObj.h>
#include <shobjidl_core.h>

#include <string>

namespace dirsize {

// Shell context menu extension that adds "Recalculate Size" to the
// right-click menu for directories.
class DirSizeContextMenu :
    public IContextMenu,
    public IShellExtInit
{
public:
    DirSizeContextMenu();
    virtual ~DirSizeContextMenu();

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // IShellExtInit
    HRESULT STDMETHODCALLTYPE Initialize(PCIDLIST_ABSOLUTE pidlFolder,
                                         IDataObject* pdtobj,
                                         HKEY hkeyProgID) override;

    // IContextMenu
    HRESULT STDMETHODCALLTYPE QueryContextMenu(HMENU hmenu, UINT indexMenu,
                                                UINT idCmdFirst, UINT idCmdLast,
                                                UINT uFlags) override;
    HRESULT STDMETHODCALLTYPE InvokeCommand(CMINVOKECOMMANDINFO* pici) override;
    HRESULT STDMETHODCALLTYPE GetCommandString(UINT_PTR idCmd, UINT uType,
                                               UINT* pReserved, CHAR* pszName,
                                               UINT cchMax) override;

private:
    LONG m_refCount = 1;
    std::wstring m_selectedPath;
};

} // namespace dirsize
