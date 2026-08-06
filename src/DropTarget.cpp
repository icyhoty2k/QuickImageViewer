// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#include "../DropTarget.h"
#include "Platform/Constants.h"
#include <shlobj.h>
#include <string>

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
        UINT needed = DragQueryFileW(hDrop, 0, nullptr, 0);
        if (needed > 0) {
            std::wstring szFile(needed + 1, L'\0');
            if (DragQueryFileW(hDrop, 0, szFile.data(), needed + 1)) {
                szFile.resize(needed);
                // Post asynchronously so the drag animation is released immediately.
                // WM_QIV_OPEN_FILE handler owns the pointer and deletes it.
                PostMessageW(m_hWnd, Constants::WM_QIV_OPEN_FILE, 0,
                             reinterpret_cast<LPARAM>(new std::wstring(std::move(szFile))));
            }
        }
        DragFinish(hDrop);
        ReleaseStgMedium(&stgm);
    }
    *pdwEffect = DROPEFFECT_COPY;
    return S_OK;
}
