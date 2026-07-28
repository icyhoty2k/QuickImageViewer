#pragma once
#include <windows.h>
#include "Input/Command.h"

// =============================================================================
// AppMenuInternal — what the four AppMenu sources share with each other and
// with nobody else.
//
// Outside this folder only AppMenu.h exists. Everything here is an
// implementation detail of how the menu is assembled and dispatched, and any
// file that reaches for it is doing something the public entry points should be
// doing instead.
// =============================================================================

namespace UI::AppMenu::detail {

    // ── Item flags ──────────────────────────────────────────────────────────
    UINT CheckFlag(bool on);
    UINT RadioFlag(bool on);

    // ── Builders (AppMenuBuilders.cpp) ──────────────────────────────────────
    // Exposed rather than file-static because Show() re-shows the transition
    // popup on its own after each tick — see the re-open loop in AppMenu.cpp.
    HMENU BuildTransitionMenu();

    // True when `id` is a transition row AND ticking it means "toggle
    // membership" rather than "pick this one" — the only case where the user
    // plausibly wants to click again immediately, and therefore the only case
    // where the menu is put straight back up.
    bool IsTransitionListToggle(int id);

    // Viewer id → Command. Contiguous blocks resolve by offset.
    Command CommandForId(int id);

    // ── Settings dispatch (AppMenuSettings.cpp) ─────────────────────────────
    // Everything below VIEWER_BASE: toggles, numeric prompts, overlay state,
    // sort order.
    void DispatchSetting(HWND hWnd, int id);

    // ── File-dialog actions (AppMenuIO.cpp) ─────────────────────────────────
    // Split out because they are a different job from flipping a setting: each
    // opens a shell dialog, touches the filesystem, and reports success or
    // failure to the user. Called from DispatchSetting's switch.
    void ExportSettings(HWND hWnd);
    void ImportSettings(HWND hWnd);
    void RestoreDefaults(HWND hWnd);
    void BackupHistoryAndFavorites(HWND hWnd);
    void RestoreHistoryAndFavorites(HWND hWnd);

} // namespace UI::AppMenu::detail
