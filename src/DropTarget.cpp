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
#include <vector>

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

// Reads one path out of an HDROP. Empty on failure, so callers test rather than
// carrying a separate success flag.
static std::wstring DroppedPathAt(HDROP hDrop, UINT index) {
    const UINT needed = DragQueryFileW(hDrop, index, nullptr, 0);
    if (needed == 0) return {};
    std::wstring path(needed + 1, L'\0');
    if (!DragQueryFileW(hDrop, index, path.data(), needed + 1)) return {};
    path.resize(needed);
    return path;
}

HRESULT __stdcall DropTarget::Drop(IDataObject* pDataObj, DWORD, POINTL, DWORD* pdwEffect) {
    FORMATETC fmte = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM stgm;
    if (SUCCEEDED(pDataObj->GetData(&fmte, &stgm))) {
        HDROP hDrop = (HDROP)stgm.hGlobal;

        // A TRANSLATOR, and nothing more: every path in the drop is read out and
        // handed over as-is. What opens, what only gets its folder remembered and
        // what is reported as unreachable are decisions the WM_QIV_OPEN_FILE
        // handler makes — it is also the endpoint for the single-instance
        // handoff, and the two must not answer the same question differently.
        //
        // 0xFFFFFFFF asks for the COUNT, the same query PasteFilesFromClipboard
        // uses.
        const UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);

        auto *paths = new std::vector<std::wstring>();
        paths->reserve(count);
        for (UINT i = 0; i < count; ++i) {
            std::wstring p = DroppedPathAt(hDrop, i);
            if (!p.empty()) paths->push_back(std::move(p));
        }

        // Post asynchronously so the drag animation is released immediately.
        // The handler owns the vector and deletes it — but only if it arrives,
        // so an empty drop or a refused post frees it here.
        if (paths->empty() ||
            !PostMessageW(m_hWnd, Constants::WM_QIV_OPEN_FILE, 0,
                          reinterpret_cast<LPARAM>(paths)))
            delete paths;

        DragFinish(hDrop);
        ReleaseStgMedium(&stgm);
    }
    *pdwEffect = DROPEFFECT_COPY;
    return S_OK;
}
