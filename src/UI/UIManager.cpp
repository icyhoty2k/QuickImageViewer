#include "UIManager.h"
#include "../AppState.h"
#include "../Overlays/OverlayManager.h"
#include "../Platform/ConstantsStrings.h"
#include <filesystem>

UI::UIManager uiManager;

namespace UI {
    void UIManager::Init(HINSTANCE hInstance, HWND hMainWnd) {
        m_hInstance = hInstance;
        m_hMainWnd = hMainWnd;
        LoadFolderHistoryFromDisk();
    }

    // -------------------------------------------------------------------------
    // Layout registration — called by ThumbnailPanelWnd on show/hide
    // -------------------------------------------------------------------------

    void UIManager::OnPanelShown(ThumbnailPanelWnd *panel, int8_t position) {
        m_layout.set(position, panel);
        RefreshVerticalPanels();
    }

    void UIManager::OnPanelHidden(ThumbnailPanelWnd *panel) {
        if (dynamic_cast<SpawnedDirWnd *>(panel)) {
            g_overlayManager.PostCenterMessage(m_hMainWnd,
                                               Constants::Messages::SPAWN_DIR_CLOSED);
            m_layout.clearPanel(panel);
            RefreshVerticalPanels();
            return; // pool panel — never deleted
        }
        m_layout.clearPanel(panel);
        RefreshVerticalPanels();
    }

    int8_t UIManager::NextFreePosition(int8_t currentPosition) const {
        return m_layout.nextFreePosition(currentPosition);
    }

    // -------------------------------------------------------------------------
    // Show / Hide / Toggle — notify layout on every state change
    // -------------------------------------------------------------------------

    void UIManager::Show(IPanelWindow &panel) {
        panel.Show();
        // OnPanelShown is called from ThumbnailPanelWnd::Show via Toggle path.
    }

    void UIManager::Hide(IPanelWindow &panel) {
        panel.Hide();
        // OnPanelHidden is called from ThumbnailPanelWnd::Hide.
    }

    void UIManager::Toggle(IPanelWindow &panel) {
        panel.Toggle();
    }

    void UIManager::HideAllPanelWindows() {
        helpWnd.Hide();
        cacheWnd.Hide();
        dirWnd.Hide();
        historyListWnd.Hide();
        exifWnd.Hide();
        jumpToWnd.Hide();
        statsWnd.Hide();
        HideAllSpawnedDirWnds();
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

    DirWnd &UIManager::getDirWindow() {
        if (isInit(dirWnd)) return dirWnd;
        dirWnd.Init(m_hInstance, m_hMainWnd, Constants::CURRENT_DIR_WINDOW_POSITION);
        return dirWnd;
    }

    ThumbnailPanelWnd &UIManager::getActiveDirWnd() {
        if (m_activeDirWnd && m_activeDirWnd->GetHwnd() && IsWindowVisible(m_activeDirWnd->GetHwnd()))
            return *m_activeDirWnd;
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

    FindWnd &UIManager::getFindWindow() {
        if (isInit(findWnd)) return findWnd;
        findWnd.Init(m_hInstance, m_hMainWnd);
        return findWnd;
    }

    void UIManager::ToggleJumpToWindow() {
        if (isInit(findWnd)) findWnd.Hide();
        Toggle(getJumpToWindow());
    }

    void UIManager::ToggleFindWindow() {
        if (isInit(jumpToWnd)) jumpToWnd.Hide();
        Toggle(getFindWindow());
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

        const wchar_t *msg = nullptr;
        switch (freePos) {
            case 1: msg = Constants::Messages::SPAWN_DIR_TOP;    break;
            case 2: msg = Constants::Messages::SPAWN_DIR_RIGHT;  break;
            case 3: msg = Constants::Messages::SPAWN_DIR_BOTTOM; break;
            case 4: msg = Constants::Messages::SPAWN_DIR_LEFT;   break;
        }
        if (msg) g_overlayManager.PostCenterMessage(m_hMainWnd, msg);

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
            try {
                if (std::filesystem::equivalent(
                        std::filesystem::path(folderPath),
                        std::filesystem::path(panelFolder))) {
                    const SlotInfo *slot = m_layout.getSlot(p->GetPosition());
                    if (slot && !slot->name.empty())
                        allLabels += L" (" + slot->name + L")";
                }
            } catch (...) {}
        }

        // Check F5 DirWnd if it's visible
        if (dirWnd.IsVisible() &&
            app.currentIndex >= 0 &&
            app.currentIndex < static_cast<int>(app.playlist.size())) {
            const std::wstring currentFolder =
                std::filesystem::path(app.playlist[app.currentIndex]).parent_path().wstring();
            try {
                if (std::filesystem::equivalent(
                        std::filesystem::path(folderPath),
                        std::filesystem::path(currentFolder))) {
                    const SlotInfo *slot = m_layout.getSlot(dirWnd.GetPosition());
                    if (slot && !slot->name.empty())
                        allLabels += L" [F5 -> " + slot->name + L"]";
                }
            } catch (...) {}
        }

        return allLabels;
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

    void UIManager::RepaintAllPanels() {
        SlotInfo *slots[] = {
            &m_layout.center, &m_layout.top, &m_layout.right,
            &m_layout.bottom, &m_layout.left
        };
        for (auto *slot : slots) {
            if (slot->panel) slot->panel->Repaint();
        }
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
