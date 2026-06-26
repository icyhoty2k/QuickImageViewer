#pragma once
#include <windows.h>
#include <oleidl.h>

class DropTarget : public IDropTarget {
    HWND m_hWnd;
    ULONG m_refCount;

public:
    DropTarget(HWND hWnd) : m_hWnd(hWnd), m_refCount(1) {}

    // IUnknown methods
    HRESULT __stdcall QueryInterface(REFIID iid, void** ppvObject) override;
    ULONG __stdcall AddRef() override;
    ULONG __stdcall Release() override;

    // IDropTarget methods
    HRESULT __stdcall DragEnter(IDataObject* pDataObj, DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) override;
    HRESULT __stdcall DragOver(DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) override;
    HRESULT __stdcall DragLeave() override;
    HRESULT __stdcall Drop(IDataObject* pDataObj, DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) override;
};