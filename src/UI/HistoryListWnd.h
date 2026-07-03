#pragma once

#include <windows.h>
#include <vector>
#include <string>

namespace UI {
    class HistoryListWnd {
        // -------------------------------------------------------------------------
        // HistoryWindow  —  Last-visited folder history panel.
        //
        // Shows the last Constants::HISTORY_MAX_DIRS folders the user opened,
        // most-recent at the top. Click any row to open that folder (first image).
        //
        // Displayed as a centered floating GDI popup — same dark style as the
        // HelpWindow, same WS_POPUP | WS_EX_TOPMOST | WS_EX_LAYERED flags.
        //
        // Shortcut:
        //   F7  —  Toggle history panel (SC_PANEL_HISTORY_TOGGLE)
        //   Esc —  Hide panel (SC_LOCAL_HIDE)
        // -------------------------------------------------------------------------

        // Called by FileHandler after every successful folder load.
        // Pushes the folder path to the front; drops oldest entry when the list
        // exceeds Constants::HISTORY_MAX_DIRS.
        void PushFolderHistory(const std::wstring &folderPath);

        // Returns the current history list (index 0 = most recent).
        const std::vector<std::wstring> &GetFolderHistory();

        // ---- Lifecycle ----------------------------------------------------------
        void InitHistoryWindow(HINSTANCE hInstance, HWND hParent);

        // ---- Visibility ---------------------------------------------------------
        void ToggleHistoryWindow();
    };
}
