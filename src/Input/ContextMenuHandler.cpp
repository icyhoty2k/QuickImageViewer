#include "ContextMenuHandler.h"
#include "Command.h"
#include "Platform/Constants.h"
#include <string>

namespace Input {

namespace {
    // Local TrackPopupMenu command IDs — scoped to this popup only, so they can
    // never collide with the tray menu's IDs. Each maps to a Command below.
    enum MenuId : int {
        ID_HISTORY      = 1,   // toggle HistoryListWnd            (Tab)
        ID_THUMB_STRIP,        // toggle DirWnd thumbnail strip    (F6)
        ID_PREV_FOLDER,        // switch to previous folder        (Q)
        ID_PREV_IMAGE,         // switch to previously viewed image(E)
        ID_STATS,              // toggle StatsWnd                  (K)
        ID_METADATA,           // toggle image info / metadata     (M)
        ID_CLOSE_APP,          // hide to tray if kept in bg, else quit (Esc)
        ID_CLOSE_PANELS,       // hide all panels + spawned DirWnds
        ID_RESTORE_PANELS,     // re-open the panels Close All Panels hid
        ID_HARD_QUIT,          // full process exit               (Ctrl+Q)
    };

    // Single table: menu label (with right-aligned shortcut hint) → Command.
    Command CommandForId(int id) {
        switch (id) {
            case ID_HISTORY:      return Command::ToggleHistory;
            case ID_THUMB_STRIP:  return Command::ToggleDir;
            case ID_PREV_FOLDER:  return Command::ToggleLastDir;
            case ID_PREV_IMAGE:   return Command::ToggleLastImage;
            case ID_STATS:        return Command::ToggleStats;
            case ID_METADATA:     return Command::ShowInfo;
            case ID_CLOSE_APP:      return Command::HideToTray;
            case ID_CLOSE_PANELS:   return Command::CloseAllPanels;
            case ID_RESTORE_PANELS: return Command::RestoreAllPanels;
            case ID_HARD_QUIT:      return Command::HardQuit;
            default:              return Command::None;
        }
    }
}

void ContextMenuHandler::Show(HWND hWnd, int x, int y) {
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    AppendMenuW(hMenu, MF_STRING, ID_HISTORY,     L"History\tTab");
    AppendMenuW(hMenu, MF_STRING, ID_THUMB_STRIP, L"Thumbnail Strip\tF6");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, ID_PREV_FOLDER, L"Previous Folder\tQ");
    AppendMenuW(hMenu, MF_STRING, ID_PREV_IMAGE,  L"Previous Image\tE");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, ID_STATS,       L"Statistics\tK");
    AppendMenuW(hMenu, MF_STRING, ID_METADATA,    L"Metadata\tM");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, ID_CLOSE_APP,    L"Close App\tEsc");
    AppendMenuW(hMenu, MF_STRING, ID_CLOSE_PANELS,   L"Close All Panels\tN");
    AppendMenuW(hMenu, MF_STRING, ID_RESTORE_PANELS, L"Restore All Panels\tN");
    AppendMenuW(hMenu, MF_STRING, ID_HARD_QUIT,      L"Hard Quit\tCtrl+Q");

    // Footer branding — disabled caption, no command. std::wstring keeps it in
    // scope until AppendMenuW copies the text into the menu.
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    const std::wstring brand = std::wstring(L"qIV  ") + Constants::APP_VERSION;
    AppendMenuW(hMenu, MF_STRING | MF_DISABLED | MF_GRAYED, 0, brand.c_str());

    // TPM_RETURNCMD: TrackPopupMenu returns the chosen id instead of posting
    // WM_COMMAND, so we dispatch inline. SetForegroundWindow + the WM_NULL kick
    // are the documented workaround so the popup dismisses on first outside click.
    SetForegroundWindow(hWnd);
    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                             x, y, 0, hWnd, nullptr);
    PostMessage(hWnd, WM_NULL, 0, 0);
    DestroyMenu(hMenu);

    Command c = CommandForId(cmd);
    if (c != Command::None)
        InputManager::ExecuteCommand(hWnd, c);
}

} // namespace Input
