#pragma once
#include <windows.h>

namespace UI { class UIManager; }
class OverlayManager;

namespace Input {

class TrayHandler {
public:
    TrayHandler(UI::UIManager &uiManager, OverlayManager &overlayManager);
    LRESULT Handle(HWND hWnd, WPARAM wParam, LPARAM lParam);

    // Owns the SETTINGS half of the shared menu's id space (toggles, prompts,
    // view mode, sort, backup, export/import/restore). Public because
    // ContextMenuHandler::Dispatch forwards those ids here — the menu itself is
    // built in one place (ContextMenuHandler), this only executes.
    void DispatchCommand(HWND hWnd, int cmd);

private:
    UI::UIManager  &m_uiManager;
    OverlayManager &m_overlayManager;

    void RestoreWindow(HWND hWnd);
};

} // namespace Input
