#include "context_menu.h"
#include "property_handler.h"
#include "dirsize/ipc.h"

#include <shellapi.h>
#include <strsafe.h>

namespace dirsize {

DirSizeContextMenu::DirSizeContextMenu() {
    DllAddRef();
}

DirSizeContextMenu::~DirSizeContextMenu() {
    DllRelease();
}

HRESULT DirSizeContextMenu::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) return E_POINTER;

    if (riid == IID_IUnknown) {
        *ppvObject = static_cast<IContextMenu*>(this);
    } else if (riid == IID_IContextMenu) {
        *ppvObject = static_cast<IContextMenu*>(this);
    } else if (riid == IID_IShellExtInit) {
        *ppvObject = static_cast<IShellExtInit*>(this);
    } else {
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
}

ULONG DirSizeContextMenu::AddRef() {
    return InterlockedIncrement(&m_refCount);
}

ULONG DirSizeContextMenu::Release() {
    ULONG count = InterlockedDecrement(&m_refCount);
    if (count == 0) delete this;
    return count;
}

HRESULT DirSizeContextMenu::Initialize(PCIDLIST_ABSOLUTE /*pidlFolder*/,
                                        IDataObject* pdtobj,
                                        HKEY /*hkeyProgID*/) {
    if (!pdtobj) return E_INVALIDARG;

    // Extract the selected folder path from the data object
    FORMATETC fmt = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM stg = {};

    HRESULT hr = pdtobj->GetData(&fmt, &stg);
    if (FAILED(hr)) return hr;

    HDROP hDrop = static_cast<HDROP>(GlobalLock(stg.hGlobal));
    if (!hDrop) {
        ReleaseStgMedium(&stg);
        return E_FAIL;
    }

    // We only handle single selection for recalculate
    UINT fileCount = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
    if (fileCount >= 1) {
        wchar_t path[MAX_PATH * 2];
        if (DragQueryFileW(hDrop, 0, path, _countof(path)) > 0) {
            m_selectedPath = path;
        }
    }

    GlobalUnlock(stg.hGlobal);
    ReleaseStgMedium(&stg);

    return m_selectedPath.empty() ? E_FAIL : S_OK;
}

HRESULT DirSizeContextMenu::QueryContextMenu(HMENU hmenu, UINT indexMenu,
                                               UINT idCmdFirst, UINT /*idCmdLast*/,
                                               UINT uFlags) {
    // Don't add menu items if Explorer is showing the default verb only
    if (uFlags & CMF_DEFAULTONLY) return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 0);

    InsertMenuW(hmenu, indexMenu, MF_STRING | MF_BYPOSITION,
                idCmdFirst, L"Recalculate Size");

    // Return the number of menu items added (1)
    return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 1);
}

HRESULT DirSizeContextMenu::InvokeCommand(CMINVOKECOMMANDINFO* pici) {
    // Verify it's our command (index 0)
    if (HIWORD(pici->lpVerb) != 0) {
        return E_INVALIDARG; // String verb — not ours
    }
    if (LOWORD(pici->lpVerb) != 0) {
        return E_INVALIDARG; // Not our command index
    }

    if (m_selectedPath.empty()) return E_FAIL;

    // Send recalculate command to the service via named pipe
    IpcStatus status;
    bool sent = SendCommand(IpcCommand::Recalculate, m_selectedPath, status);

    if (sent && status == IpcStatus::Ok) {
        // Invalidate the cache so Explorer re-queries
        SizeCache::Instance().Invalidate(m_selectedPath);

        // Tell Explorer to refresh this folder's parent view
        SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW,
                       m_selectedPath.c_str(), nullptr);
    }

    return S_OK;
}

HRESULT DirSizeContextMenu::GetCommandString(UINT_PTR idCmd, UINT uType,
                                              UINT* /*pReserved*/, CHAR* pszName,
                                              UINT cchMax) {
    if (idCmd != 0) return E_INVALIDARG;

    if (uType == GCS_HELPTEXTW) {
        StringCchCopyW(reinterpret_cast<LPWSTR>(pszName), cchMax,
                       L"Recalculate the total size of this directory");
        return S_OK;
    }

    if (uType == GCS_HELPTEXTA) {
        StringCchCopyA(pszName, cchMax,
                       "Recalculate the total size of this directory");
        return S_OK;
    }

    if (uType == GCS_VERBW) {
        StringCchCopyW(reinterpret_cast<LPWSTR>(pszName), cchMax,
                       L"DirSizeRecalculate");
        return S_OK;
    }

    if (uType == GCS_VERBA) {
        StringCchCopyA(pszName, cchMax, "DirSizeRecalculate");
        return S_OK;
    }

    return E_INVALIDARG;
}

} // namespace dirsize
