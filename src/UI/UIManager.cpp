// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#include "UIManager.h"
#include "GdiPool.h" // Flush() — pooled colours die with the theme they came from
#include "../AppState.h"
#include "../Overlays/OverlayManager.h"
#include "../Platform/ConstantsStrings.h"
#include "../Platform/ConstantsTheme.h"
#include "../Input/Shortcuts.h"
#include "../Rem_TCP_IP/RemoteServer.h"  // EmitToObservers — the panel announcement
#include "../Rem_TCP_IP/RemoteInbound.h" // InboundSource — do not echo to the asker
#include <filesystem>
#include <dwmapi.h>

UI::UIManager uiManager;

namespace UI {
    void UIManager::Init(HINSTANCE hInstance, HWND hMainWnd) {
        m_hInstance = hInstance;
        m_hMainWnd = hMainWnd;
    }

    // -------------------------------------------------------------------------
    // Layout registration — called by ThumbnailPanelWnd on show/hide
    // -------------------------------------------------------------------------

    void UIManager::OnPanelShown(ThumbnailPanelWnd *panel, int8_t position) {
        m_layout.set(position, panel);
        RefreshVerticalPanels();
        // The thumbnail panels' half of the funnel — they override Show/Hide and
        // never reach IPanelWindow's, so this is the only place their visibility
        // change is observable. MovePanel calls Hidden then Shown, which nets to
        // no change and emits nothing, because the announcement compares first.
        AnnouncePanelVisibility();
    }

    void UIManager::OnPanelHidden(ThumbnailPanelWnd *panel) {
        if (dynamic_cast<SpawnedDirWnd *>(panel)) {
            g_overlayManager.PostCenterMessage(m_hMainWnd,
                                               Constants::Messages::SPAWN_DIR_CLOSED);
            m_layout.clearPanel(panel);
            RefreshVerticalPanels();
            RefreshStatsWindowIfVisible();
            AnnouncePanelVisibility();
            return; // pool panel — never deleted
        }
        m_layout.clearPanel(panel);
        RefreshVerticalPanels();
        // Closing one is the case that was missing. See OnPanelShown.
        AnnouncePanelVisibility();
    }

    int8_t UIManager::NextFreePosition(int8_t currentPosition) const {
        return m_layout.nextFreePosition(currentPosition);
    }

    // -------------------------------------------------------------------------
    // Show / Hide / Toggle — notify layout on every state change
    // -------------------------------------------------------------------------

    // =========================================================================
    // "A panel opened or closed here" — told to whoever is watching.
    //
    // ToggleAllPanels reports AnyPanelVisible(), and every command that changes
    // it is refused by the observer echo: an observer EXECUTES what it receives,
    // so replaying a panel toggle would raise a window on a screen nobody is
    // sitting at. Correct, and it left the value itself unreportable — press F6
    // and a phone's Panels button was wrong until the screen was reopened.
    //
    // So the VALUE is announced rather than the command. TogglesChanged
    // instructs nothing, which is what makes it safe to push at a desktop
    // observer, and it is spelled in QueryToggles' own vocabulary so a client
    // hands it to the parser it already has. 1 / 0, matching OnOff — NOT On/Off.
    //
    // ONLY ON A REAL FLIP. This is called from every route that can change panel
    // visibility, and most of those calls change nothing — opening a second
    // panel while one is already up leaves AnyPanelVisible() true. Without the
    // comparison a phone would get a line per panel per keystroke, all saying
    // the same thing.
    //
    // Cheap enough to call everywhere: a walk of a handful of pointers, and the
    // early return happens before HasObservers() is even asked.
    // =========================================================================
    // The free function IPanelWindow.h declares — see the comment there for why
    // it is not simply a call to uiManager from that header.
    void NotifyPanelVisibilityChanged() {
        uiManager.AnnouncePanelVisibility();
    }

    void UIManager::AnnouncePanelVisibility() {
        const bool now = AnyPanelVisible();
        if (now == m_lastPanelsVisible) return;
        m_lastPanelsVisible = now;

        if (!Remote::HasObservers()) return;

        // Excludes the connection that caused it, like every other emit: a phone
        // that sent ToggleAllPanels reads the value off its own reply.
        Remote::EmitToObservers(
            std::wstring(L"TogglesChanged ToggleAllPanels=") + (now ? L"1" : L"0"),
            Remote::InboundSource());
    }

    void UIManager::Show(IPanelWindow &panel) {
        panel.Show();
        // OnPanelShown is called from ThumbnailPanelWnd::Show via Toggle path.
        AnnouncePanelVisibility();
    }

    void UIManager::Hide(IPanelWindow &panel) {
        panel.Hide();
        // OnPanelHidden is called from ThumbnailPanelWnd::Hide.
        AnnouncePanelVisibility();
    }

    void UIManager::Toggle(IPanelWindow &panel) {
        panel.Toggle();
        RefreshStatsWindowIfVisible();
        AnnouncePanelVisibility();
    }

    void UIManager::HideAllPanelWindows() {
        // Snapshot what is visible so RestoreAllPanels() can replay it. Fixed
        // panels first, then the spawned DirWnd pool. Keep the previous snapshot
        // if nothing is currently visible (nothing new to remember).
        //
        // Both the snapshot and the hiding walk m_fixedPanels — see the note on
        // its declaration. Hiding used to be a hand-written run of eight Hide()
        // calls that did not match even the list beside it.
        std::vector<IPanelWindow *> visibleNow;
        for (IPanelWindow *p : m_fixedPanels)
            if (p->IsVisible()) visibleNow.push_back(p);
        for (auto *p : m_spawnedPool)
            if (p->IsPanelVisible()) visibleNow.push_back(p);
        if (!visibleNow.empty())
            m_restoreList = std::move(visibleNow);

        // Unconditional, as before: Hide() on a panel that was never initialised
        // is a no-op, so there is nothing to guard against.
        for (IPanelWindow *p : m_fixedPanels)
            p->Hide();
        HideAllSpawnedDirWnds();
        AnnouncePanelVisibility();
    }

    void UIManager::RestoreAllPanels() {
        for (IPanelWindow *p : m_restoreList)
            if (!p->IsVisible()) p->Show();
        RefreshVerticalPanels();
        RefreshStatsWindowIfVisible();
        AnnouncePanelVisibility();
    }

    bool UIManager::AnyPanelVisible() const {
        // Same list HideAllPanelWindows uses. When these two disagreed,
        // ToggleAllPanels restored panels instead of closing them: only F9 was
        // open, this said "nothing is visible", and the toggle took the
        // restore branch.
        for (const IPanelWindow *p : m_fixedPanels)
            if (p->IsVisible()) return true;
        for (const auto *p : m_spawnedPool)
            if (p->IsPanelVisible()) return true;
        return false;
    }

    // -------------------------------------------------------------------------
    // Lazy panel getters
    // -------------------------------------------------------------------------

    HelpWnd &UIManager::getHelpWindow() {
        if (isInit(helpWnd)) return helpWnd;
        helpWnd.Init(m_hInstance, m_hMainWnd);
        return helpWnd;
    }

    CacheWnd &UIManager::getCacheWindow() {
        if (isInit(cacheWnd)) return cacheWnd;
        cacheWnd.Init(m_hInstance, m_hMainWnd, Constants::CACHE_WINDOW_POSITION);
        return cacheWnd;
    }

    DedicatedWnd &UIManager::getDedicatedWindow() {
        if (isInit(dedicatedWnd)) return dedicatedWnd;
        dedicatedWnd.Init(m_hInstance, m_hMainWnd);
        return dedicatedWnd;
    }

    RemoteWnd &UIManager::getRemoteWindow() {
        if (isInit(remoteWnd)) return remoteWnd;
        remoteWnd.Init(m_hInstance, m_hMainWnd);
        return remoteWnd;
    }

    RemoteClientsWnd &UIManager::getRemoteClientsWindow() {
        if (isInit(remoteClientsWnd)) return remoteClientsWnd;
        remoteClientsWnd.Init(m_hInstance, m_hMainWnd);
        return remoteClientsWnd;
    }

    RemotesWnd &UIManager::getRemotesConsoleWindow() {
        if (isInit(remotesWnd)) return remotesWnd;
        remotesWnd.Init(m_hInstance, m_hMainWnd);
        return remotesWnd;
    }

    MirrorPickerWnd &UIManager::getMirrorPickerWindow() {
        if (isInit(mirrorPickerWnd)) return mirrorPickerWnd;
        mirrorPickerWnd.Init(m_hInstance, m_hMainWnd);
        return mirrorPickerWnd;
    }

    RemoteLogWnd &UIManager::getRemoteLogWindow() {
        if (isInit(remoteLogWnd)) return remoteLogWnd;
        remoteLogWnd.Init(m_hInstance, m_hMainWnd);
        return remoteLogWnd;
    }

    RemoteCmdWnd &UIManager::getRemoteCmdWindow() {
        if (isInit(remoteCmdWnd)) return remoteCmdWnd;
        remoteCmdWnd.Init(m_hInstance, m_hMainWnd);
        return remoteCmdWnd;
    }

    DirWnd &UIManager::getDirWindow() {
        if (isInit(dirWnd)) return dirWnd;
        dirWnd.Init(m_hInstance, m_hMainWnd, Constants::CURRENT_DIR_WINDOW_POSITION);
        return dirWnd;
    }

    // The panel the user last clicked in — but the fallback matters as much as
    // the hit, because a dozen FileHandler sites route folder work through this
    // (ClearDirThumbnailCache, SetPlaylistCopy, UpdateDirView, GetPanelFolder,
    // SyncDirSelectionRectangle).
    //
    // It used to fall straight through to getDirWindow(), which CONSTRUCTS AND
    // INITS F6 on demand. So "F6 was never opened" did not mean there was no F6
    // — it meant a hidden one got created here and quietly received the folder
    // work meant for whatever panel the user was actually looking at. Spawning
    // from the history list never sets the active pointer, so with only spawned
    // panels on screen that was every call until the first click landed.
    //
    // Prefer any VISIBLE dir panel over creating a hidden one. F6 first when it
    // is up, then the spawned pool in slot order. getDirWindow() stays as the
    // last resort so callers still get a reference when nothing is visible.
    ThumbnailPanelWnd &UIManager::getActiveDirWnd() {
        if (m_activeDirWnd && m_activeDirWnd->GetHwnd() && IsWindowVisible(m_activeDirWnd->GetHwnd()))
            return *m_activeDirWnd;

        if (isInit(dirWnd) && dirWnd.IsPanelVisible())
            return dirWnd;

        for (auto *p : m_spawnedPool) {
            if (p->IsPanelVisible()) return *p;
        }

        return getDirWindow();
    }

    void UIManager::SetActiveDirWnd(ThumbnailPanelWnd *panel) {
        m_activeDirWnd = panel;
    }

    HistoryListWnd &UIManager::getHistoryListWindow() {
        if (isInit(historyListWnd)) return historyListWnd;
        historyListWnd.Init(m_hInstance, m_hMainWnd);
        return historyListWnd;
    }

    ExifWnd &UIManager::getInfoWindow() {
        if (isInit(exifWnd)) return exifWnd;
        exifWnd.Init(m_hInstance, m_hMainWnd);
        return exifWnd;
    }

    JumpToWnd &UIManager::getJumpToWindow() {
        if (isInit(jumpToWnd)) return jumpToWnd;
        jumpToWnd.Init(m_hInstance, m_hMainWnd);
        return jumpToWnd;
    }

    ZoomWnd &UIManager::getZoomWindow() {
        if (isInit(zoomWnd)) return zoomWnd;
        zoomWnd.Init(m_hInstance, m_hMainWnd);
        return zoomWnd;
    }

    // Deliberately has no caller. The zoom panel opens on '0', a digit the panel's
    // own input box consumes, so a toggle can never see the second press. Kept for
    // symmetry with the Jump-to / Find pair; see Command::ZoomTo for the full why.
    void UIManager::ToggleZoomWindow() {
        Toggle(getZoomWindow());
    }

    FindWnd &UIManager::getFindWindow() {
        if (isInit(findWnd)) return findWnd;
        findWnd.Init(m_hInstance, m_hMainWnd);
        return findWnd;
    }

    void UIManager::ToggleJumpToWindow() {
        if (isInit(findWnd)) findWnd.Hide();
        Toggle(getJumpToWindow());
    }

    void UIManager::ToggleFindWindow(bool searchEverywhere) {
        if (isInit(jumpToWnd)) jumpToWnd.Hide();

        // THE SCOPE IS SET BEFORE THE TOGGLE, so a panel about to be shown
        // already knows what it searches when Show() rebuilds the list. Setting
        // it afterwards would match the first keystroke against the old scope.
        //
        // And pressing the OTHER key while it is already open re-scopes the
        // search rather than closing the panel: Ctrl+F then Ctrl+Shift+F widens
        // the search you are already typing, which is the whole point of having
        // two keys rather than one that toggles.
        FindWnd &find = getFindWindow();
        const bool rescope = find.IsVisible() && find.SearchesEverywhere() != searchEverywhere;
        find.SetSearchEverywhere(searchEverywhere);
        if (rescope) {
            find.RefreshMatches();
            return;
        }
        Toggle(find);
    }

    StatsWnd &UIManager::getStatsWindow() {
        if (isInit(statsWnd)) return statsWnd;
        statsWnd.Init(m_hInstance, m_hMainWnd);
        return statsWnd;
    }

    void UIManager::RefreshInfoWindowIfVisible() {
        exifWnd.Refresh(); // Refresh() is a no-op if the window is not initialized or not visible
    }

    void UIManager::RefreshStatsWindowIfVisible() {
        if (isInit(statsWnd)) statsWnd.Refresh();
    }

    void UIManager::RefreshRemoteWindowIfVisible() {
        if (isInit(remoteWnd)) remoteWnd.Refresh();
    }

    void UIManager::ApplyAlwaysOnTop(bool onTop) {
        HWND zOrder = onTop ? HWND_TOPMOST : HWND_NOTOPMOST;
        constexpr UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;

        auto applyIfVisible = [&](IPanelWindow &panel) {
            HWND h = panel.GetHwnd();
            if (h && IsWindowVisible(h))
                SetWindowPos(h, zOrder, 0, 0, 0, 0, flags);
        };

        applyIfVisible(helpWnd);
        applyIfVisible(cacheWnd);
        applyIfVisible(dirWnd);
        applyIfVisible(historyListWnd);
applyIfVisible(exifWnd);
        applyIfVisible(jumpToWnd);
        applyIfVisible(zoomWnd);
        applyIfVisible(findWnd);
        applyIfVisible(statsWnd);

        // Spawned DirWnds tracked in layout slots
        SlotInfo *slots[] = {
            &m_layout.center, &m_layout.top, &m_layout.right,
            &m_layout.bottom, &m_layout.left
        };
        for (auto *slot : slots) {
            if (slot->panel) {
                HWND h = slot->panel->GetHwnd();
                if (h && IsWindowVisible(h))
                    SetWindowPos(h, zOrder, 0, 0, 0, 0, flags);
            }
        }
    }

    // -------------------------------------------------------------------------
    // SpawnDirWndForFolder
    // -------------------------------------------------------------------------

    void UIManager::SpawnDirWndForFolder(const std::wstring &folderPath, HWND hHistoryWnd) {
        // Find a free layout position (skip center — reserved for F5 DirWnd/CacheWnd)
        int8_t freePos = -1;
        for (int8_t pos = 1; pos <= 4; ++pos) {
            if (!m_layout.occupied(pos)) { freePos = pos; break; }
        }
        if (freePos < 0) {
            g_overlayManager.PostCenterMessage(m_hMainWnd, Constants::Messages::SPAWN_DIR_NO_SPACE);
            if (hHistoryWnd) { SetForegroundWindow(hHistoryWnd); SetFocus(hHistoryWnd); }
            return;
        }

        // Pick the first hidden panel from the pre-allocated pool
        SpawnedDirWnd *target = nullptr;
        for (auto *p : m_spawnedPool) {
            if (!p->IsPanelVisible()) { target = p; break; }
        }
        if (!target) {
            // All pool entries already visible — layout is actually full; treated as no space
            g_overlayManager.PostCenterMessage(m_hMainWnd, Constants::Messages::SPAWN_DIR_NO_SPACE);
            if (hHistoryWnd) { SetForegroundWindow(hHistoryWnd); SetFocus(hHistoryWnd); }
            return;
        }

        if (!target->GetHwnd())
            target->Init(m_hInstance, m_hMainWnd, static_cast<int8_t>(freePos));
        else
            target->SetPosition(freePos);

        target->LoadFolder(folderPath);
        target->Show(); // registers in layout, positions + shows window

        RefreshVerticalPanels();
        target->UpdateDirView();

        // Claim the active-dir slot when nothing visible holds it. Spawning is
        // not a click, so it must not steal the slot from a panel the user did
        // click — but leaving it unset sent every getActiveDirWnd() call to the
        // hidden F6 until the first click landed here.
        if (!m_activeDirWnd || !m_activeDirWnd->IsPanelVisible())
            SetActiveDirWnd(target);

        const wchar_t *msg = nullptr;
        switch (freePos) {
            case 1: msg = Constants::Messages::SPAWN_DIR_TOP;    break;
            case 2: msg = Constants::Messages::SPAWN_DIR_RIGHT;  break;
            case 3: msg = Constants::Messages::SPAWN_DIR_BOTTOM; break;
            case 4: msg = Constants::Messages::SPAWN_DIR_LEFT;   break;
        }
        if (msg) g_overlayManager.PostCenterMessage(m_hMainWnd, msg);

        RefreshStatsWindowIfVisible();

        if (hHistoryWnd) {
            SetForegroundWindow(hHistoryWnd);
            SetFocus(hHistoryWnd);
        }
    }

    void UIManager::HideAllSpawnedDirWnds() {
        for (auto *p : m_spawnedPool)
            if (p->IsPanelVisible()) p->Hide();
    }

    // -------------------------------------------------------------------------
    // RefreshVerticalPanels
    // Resize all visible vertical panels based on current layout.
    // -------------------------------------------------------------------------

    void UIManager::RefreshVerticalPanels() {
        auto refresh = [](ThumbnailPanelWnd &p) {
            if (!p.GetHwnd() || !IsWindowVisible(p.GetHwnd()))
                return;
            p.RefreshBounds();
        };

        // Refresh vertical panels (positions 2=right, 4=left)
        if (m_layout.right.panel) refresh(*m_layout.right.panel);
        if (m_layout.left.panel) refresh(*m_layout.left.panel);
    }

    bool UIManager::isInit(IPanelWindow &panel) {
        return panel.GetHwnd() != nullptr;
    }

    // -------------------------------------------------------------------------
    // SameFolderPath
    // -------------------------------------------------------------------------
    // Compares two folders AS PATHS, deliberately not as filesystem objects.
    //
    // std::filesystem::equivalent answers "same directory on disk", which is the
    // right question for routing a scan result to the panels that care, and the
    // wrong one for the history list. A symlinked folder and its target are one
    // directory but two history rows, and equivalence made them indistinguishable:
    // spawning from either row put the position label on BOTH, and Shift+Enter on
    // one row hid the panel spawned from the other. A history row is the path the
    // user opened, so the panel belonging to that row is the one spawned with that
    // path — nothing else.
    //
    // Separator style and trailing slashes are noise, and Windows paths are
    // case-insensitive; those three are normalised away, and nothing else is.
    // -------------------------------------------------------------------------
    static std::wstring NormalizeFolderKey(const std::wstring &p) {
        std::wstring out = p;
        for (auto &ch: out) {
            if (ch == L'/') ch = L'\\';
        }
        while (!out.empty() && out.back() == L'\\') out.pop_back();
        return out;
    }

    static bool SameFolderPath(const std::wstring &a, const std::wstring &b) {
        const std::wstring ka = NormalizeFolderKey(a);
        const std::wstring kb = NormalizeFolderKey(b);
        if (ka.size() != kb.size()) return false;
        return CompareStringOrdinal(ka.c_str(), static_cast<int>(ka.size()),
                                    kb.c_str(), static_cast<int>(kb.size()),
                                    TRUE) == CSTR_EQUAL;
    }

    // -------------------------------------------------------------------------
    // GetSpawnedDirWndPositionLabel
    // Returns position label (e.g., " (Left)", " (Right)") if folder has a
    // spawned DirWnd open, else empty string.
    // -------------------------------------------------------------------------
    std::wstring UIManager::GetSpawnedDirWndPositionLabel(const std::wstring &folderPath) const {
        std::wstring allLabels;

        // Check the pool — iterate only visible (active) spawned panels
        for (auto *p : m_spawnedPool) {
            if (!p->IsPanelVisible()) continue;
            const std::wstring panelFolder = p->GetFolderPath();
            if (panelFolder.empty()) continue;
            if (SameFolderPath(folderPath, panelFolder)) {
                const SlotInfo *slot = m_layout.getSlot(p->GetPosition());
                if (slot && !slot->name.empty())
                    allLabels += L" (" + slot->name + L")";
            }
        }

        // Check F5 DirWnd if it's visible
        if (dirWnd.IsVisible() &&
            app.currentIndex >= 0 &&
            app.currentIndex < static_cast<int>(app.playlist.size())) {
            const std::wstring currentFolder =
                std::filesystem::path(app.playlist[app.currentIndex]).parent_path().wstring();
            if (SameFolderPath(folderPath, currentFolder)) {
                const SlotInfo *slot = m_layout.getSlot(dirWnd.GetPosition());
                if (slot && !slot->name.empty()) {
                    const int fNum = static_cast<int>(Shortcuts::SC_PANEL_DIR_TOGGLE) - VK_F1 + 1;
                    allLabels += L" [F" + std::to_wstring(fNum) + L" -> " + slot->name + L"]";
                }
            }
        }

        return allLabels;
    }

    SpawnedDirWnd *UIManager::FindSpawnedDirWnd(const std::wstring &folderPath) const {
        for (auto *p : m_spawnedPool) {
            if (!p->IsPanelVisible()) continue;
            const std::wstring panelFolder = p->GetFolderPath();
            if (panelFolder.empty()) continue;
            // Path match, not equivalence — this drives the Shift+Enter toggle,
            // and a symlinked row must not close the panel its target opened.
            if (SameFolderPath(folderPath, panelFolder))
                return p;
        }
        return nullptr;
    }

    std::pair<std::wstring, int> UIManager::GetSpawnedDirWndSizeInfo(const std::wstring &folderPath) const {
        for (auto *p : m_spawnedPool) {
            if (!p->IsPanelVisible()) continue;
            const std::wstring panelFolder = p->GetFolderPath();
            if (panelFolder.empty()) continue;
            if (SameFolderPath(folderPath, panelFolder)) {
                return {p->GetDirSizeStr(), static_cast<int>(p->m_thumbnails.size())};
            }
        }
        // Also check the F6 DirWnd
        if (dirWnd.IsPanelVisible()) {
            const std::wstring panelFolder = static_cast<const ThumbnailPanelWnd &>(dirWnd).GetPanelFolder();
            if (!panelFolder.empty()) {
                if (SameFolderPath(folderPath, panelFolder)) {
                    return {dirWnd.GetDirSizeStr(), static_cast<int>(dirWnd.m_thumbnails.size())};
                }
            }
        }
        return {{}, 0};
    }

    std::pair<std::wstring, int> UIManager::GetAllOpenDirWndsSummary() const {
        int64_t totalBytes = 0;
        int     totalCount = 0;
        bool    anyOpen    = false;

        auto accumulate = [&](const ThumbnailPanelWnd &p) {
            if (!p.IsPanelVisible()) return;
            anyOpen = true;
            totalBytes += p.GetDirSizeBytes();
            totalCount += static_cast<int>(p.m_thumbnails.size());
        };

        accumulate(static_cast<const ThumbnailPanelWnd &>(dirWnd));
        for (auto *p : m_spawnedPool) accumulate(*p);

        if (!anyOpen) return {{}, 0};
        return {ThumbnailPanelWnd::FormatDirSize(totalBytes), totalCount};
    }

    // -------------------------------------------------------------------------
    // NotifyFolderRefreshed
    // Iterates every layout slot (panels in slots are always visible) and calls
    // the virtual OnFolderRefreshed on each present panel. Each subclass decides
    // what to do: DirWnd updates unconditionally, SpawnedDirWnd checks its folder,
    // CacheWnd refreshes its view.
    // -------------------------------------------------------------------------
    void UIManager::RefreshPanelDirs(const std::wstring &dir1, const std::wstring &dir2) {
        SlotInfo *slots[] = {
            &m_layout.center, &m_layout.top, &m_layout.right,
            &m_layout.bottom, &m_layout.left
        };
        for (auto *slot : slots) {
            if (!slot->panel) continue;
            if (!dir1.empty()) slot->panel->RefreshFromDisk(dir1);
            if (!dir2.empty() && dir2 != dir1) slot->panel->RefreshFromDisk(dir2);
        }
    }

    // -------------------------------------------------------------------------
    // NotifyThemeChanged
    // Called whenever app.isDarkThemed or app.themeFactor changes.
    // Updates the DWM title-bar on every floating panel window and forces a
    // client-area repaint so text/background colors follow the new factor.
    // -------------------------------------------------------------------------
    void UIManager::NotifyThemeChanged() {
        // Drop every pooled brush and pen FIRST. They were built from the old
        // theme colours, and the repaints below would otherwise redraw the
        // panels in exactly the colours the theme just moved away from. Safe
        // here — nothing is mid-paint on this thread.
        Gdi::Flush();

        const BOOL dark = app.isDarkThemed ? TRUE : FALSE;
        const COLORREF capColor = Constants::Theme::ThemedGray(
            Constants::Theme::Panel::BACKGROUND_INACTIVE, app.themeFactor);

        auto applyAndRepaint = [&](IPanelWindow &panel) {
            HWND h = panel.GetHwnd();
            if (!h) return;
            DwmSetWindowAttribute(h, Constants::DWMWA_DARK_MODE, &dark, sizeof(dark));
            DwmSetWindowAttribute(h, Constants::DWMWA_CAPTION_COLOR_ATTR, &capColor, sizeof(capColor));
            SetWindowPos(h, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
            InvalidateRect(h, nullptr, FALSE);
        };

        // Iterate the arrays rather than naming panels one by one. The old
        // hand-written list held seven of them and had fallen eight behind:
        // the thumbnail strips, the Dedicated panel and all six Rem_TCP_IP
        // windows kept a stale caption colour after a theme change, because
        // adding a panel never meant remembering to add it here too.
        for (IPanelWindow *panel : m_fixedPanels)
            if (panel) applyAndRepaint(*panel);

        for (SpawnedDirWnd *panel : m_spawnedPool)
            if (panel) applyAndRepaint(*panel);
    }

    // -------------------------------------------------------------------------
    // NotifyCornerChanged
    // Called when app.cornerPreference changes at runtime (Ctrl+Shift+Numpad*).
    // The DWM corner attribute is per-window, so toggling it on the main window
    // alone left every open panel with the corners it was created with.
    // -------------------------------------------------------------------------
    void UIManager::NotifyCornerChanged() {
        const DWORD corner = app.cornerPreference;

        auto applyCorner = [&](IPanelWindow &panel) {
            HWND h = panel.GetHwnd();
            if (!h) return;
            DwmSetWindowAttribute(h, Constants::DWMWA_WINDOW_CORNER_PREFERENCES,
                                  &corner, sizeof(corner));
            SetWindowPos(h, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
        };

        for (IPanelWindow *panel : m_fixedPanels)
            if (panel) applyCorner(*panel);

        for (SpawnedDirWnd *panel : m_spawnedPool)
            if (panel) applyCorner(*panel);
    }

    void UIManager::RepaintAllPanels() {
        SlotInfo *slots[] = {
            &m_layout.center, &m_layout.top, &m_layout.right,
            &m_layout.bottom, &m_layout.left
        };
        for (auto *slot : slots) {
            if (slot->panel) slot->panel->Repaint();
        }
    }

    void UIManager::NotifySortOrderChanged() {
        SlotInfo *slots[] = {
            &m_layout.center, &m_layout.top, &m_layout.right,
            &m_layout.bottom, &m_layout.left
        };
        for (auto *slot : slots) {
            if (slot->panel) slot->panel->OnSortOrderChanged();
        }

        // A hidden F6 sits in no slot and keeps its playlist across hide/show,
        // so without this it reappears in the order it was built with. Only
        // when it already exists — this must not be the call that constructs it.
        if (isInit(dirWnd) && !dirWnd.IsPanelVisible())
            dirWnd.OnSortOrderChanged();
    }

    void UIManager::NotifyFolderRefreshed(const std::wstring &dir,
                                          const std::vector<std::wstring> &playlist,
                                          bool updatePrimaryDirWnd) {
        SlotInfo *slots[] = {
            &m_layout.center, &m_layout.top, &m_layout.right,
            &m_layout.bottom, &m_layout.left
        };
        for (auto *slot : slots) {
            if (!slot->panel) continue;
            if (!updatePrimaryDirWnd && slot->panel == &getDirWindow()) continue;
            slot->panel->OnFolderRefreshed(dir, playlist);
        }
    }

    // -------------------------------------------------------------------------
    // SlotInfo::OnSlotEmptyChanged
    // Called when a slot's empty state changes (panel added or removed).
    // Notifies HistoryListWnd to repaint immediately so position labels stay in sync.
    // -------------------------------------------------------------------------
    void SlotInfo::OnSlotEmptyChanged(bool /*empty*/) {
        HistoryListWnd &historyWnd = uiManager.getHistoryListWindow();
        HWND hWnd = historyWnd.GetHwnd();
        if (hWnd) {
            // Always invalidate, regardless of visibility, to ensure next show has current state
            InvalidateRect(hWnd, nullptr, FALSE);
            if (IsWindowVisible(hWnd)) {
                UpdateWindow(hWnd); // Force immediate repaint if visible
            }
        }
    }
} // namespace UI
