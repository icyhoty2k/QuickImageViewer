#include "../DropTarget.h"
#include "Platform/FileHandler.h"
#include <shlobj.h>

HRESULT __stdcall DropTarget::QueryInterface(REFIID iid, void** ppvObject) {
    if (iid == IID_IDropTarget || iid == IID_IUnknown) {
        *ppvObject = this;
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}
ULONG __stdcall DropTarget::AddRef() { return InterlockedIncrement(&m_refCount); }
ULONG __stdcall DropTarget::Release() {
    ULONG count = InterlockedDecrement(&m_refCount);
    if (count == 0) delete this;
    return count;
}

HRESULT __stdcall DropTarget::DragEnter(IDataObject*, DWORD, POINTL, DWORD* pdwEffect) {
    *pdwEffect = DROPEFFECT_COPY;
    return S_OK;
}
HRESULT __stdcall DropTarget::DragOver(DWORD, POINTL, DWORD* pdwEffect) {
    *pdwEffect = DROPEFFECT_COPY;
    return S_OK;
}
HRESULT __stdcall DropTarget::DragLeave() { return S_OK; }

HRESULT __stdcall DropTarget::Drop(IDataObject* pDataObj, DWORD, POINTL, DWORD* pdwEffect) {
    FORMATETC fmte = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM stgm;
    if (SUCCEEDED(pDataObj->GetData(&fmte, &stgm))) {
        HDROP hDrop = (HDROP)stgm.hGlobal;
        wchar_t szFile[MAX_PATH];
        DragQueryFileW(hDrop, 0, szFile, MAX_PATH);
        
        // Pass the dropped file to your existing image loader
        OpenSpecificImage(m_hWnd, szFile);
        
        DragFinish(hDrop);
        ReleaseStgMedium(&stgm);
    }
    *pdwEffect = DROPEFFECT_COPY;
    return S_OK;
}