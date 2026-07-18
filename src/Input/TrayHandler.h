#pragma once
#include <windows.h>

namespace UI { class UIManager; }
class OverlayManager;

namespace Input {

class TrayHandler {
public:
    TrayHandler(UI::UIManager &uiManager, OverlayManager &overlayManager);
    LRESULT Handle(HWND hWnd, WPARAM wParam, LPARAM lParam);

private:
    UI::UIManager  &m_uiManager;
    OverlayManager &m_overlayManager;

    void RestoreWindow(HWND hWnd);
    void ShowContextMenu(HWND hWnd, int x, int y);
    void DispatchCommand(HWND hWnd, int cmd);
};

} // namespace Input
