#include "UIManager.h"
#include "../Overlays/OverlayManager.h"
#include "../Platform/ConstantsStrings.h"

UI::UIManager uiManager;

namespace UI {

    void UIManager::Init(HINSTANCE hInstance, HWND hMainWnd) {
        m_hInstance = hInstance;
        m_hMainWnd  = hMainWnd;
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
        // Post a message if this is a spawned DirWnd being individually closed.
        for (int i = 0; i < Constants::DIR_WND_MAX_INSTANCES; ++i) {
            if (m_spawnedDirWnds[i] == panel) {
                g_overlayManager.PostCenterMessage(m_hMainWnd,
                    Constants::Messages::SPAWN_DIR_CLOSED);
                break;
            }
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

    // -------------------------------------------------------------------------
    // SpawnDirWndForFolder
    // -------------------------------------------------------------------------

    void UIManager::SpawnDirWndForFolder(const std::wstring &folderPath, HWND hHistoryWnd) {
        // Try to find a free slot — check struct for occupied positions.
        int chosenSlot = -1;
        for (int i = 0; i < Constants::DIR_WND_MAX_INSTANCES; ++i) {
            int8_t pos = Constants::DIR_WND_SPAWN_POSITIONS[i];
            if (!m_layout.occupied(pos)) {
                chosenSlot = i;
                break;
            }
        }

        // All positions taken — notify user and return.
        if (chosenSlot < 0) {
            g_overlayManager.PostCenterMessage(m_hMainWnd,
                Constants::Messages::SPAWN_DIR_NO_SPACE);
            if (hHistoryWnd) { SetForegroundWindow(hHistoryWnd); SetFocus(hHistoryWnd); }
            return;
        }

        m_nextSpawnSlot = (chosenSlot + 1) % Constants::DIR_WND_MAX_INSTANCES;

        int8_t position = Constants::DIR_WND_SPAWN_POSITIONS[chosenSlot];

        SpawnedDirWnd *target = m_spawnedDirWnds[chosenSlot];
        if (target == nullptr) {
            target = new SpawnedDirWnd(chosenSlot);
            target->Init(m_hInstance, m_hMainWnd, position);
            m_spawnedDirWnds[chosenSlot] = target;
        }

        target->LoadFolder(folderPath);
        ShowWindow(target->GetHwnd(), SW_SHOWNOACTIVATE);
        m_layout.set(position, target);
        RefreshVerticalPanels();
        target->UpdateDirView();

        // Notify user which position was used.
        const wchar_t *msg = nullptr;
        switch (position) {
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
        for (int i = 0; i < Constants::DIR_WND_MAX_INSTANCES; ++i) {
            if (m_spawnedDirWnds[i] != nullptr)
                m_spawnedDirWnds[i]->Hide();
        }
        // OnPanelHidden handles layout cleanup per-panel.
    }

    // -------------------------------------------------------------------------
    // RefreshVerticalPanels
    // Resize all visible vertical panels based on current layout.
    // -------------------------------------------------------------------------

    void UIManager::RefreshVerticalPanels() {
        auto refresh = [](ThumbnailPanelWnd &p) {
            int8_t pos = p.GetPosition();
            if ((pos != 2 && pos != 4) || !p.GetHwnd() || !IsWindowVisible(p.GetHwnd()))
                return;
            p.RefreshBounds();
        };

        refresh(cacheWnd);
        refresh(dirWnd);
        for (int i = 0; i < Constants::DIR_WND_MAX_INSTANCES; ++i)
            if (m_spawnedDirWnds[i]) refresh(*m_spawnedDirWnds[i]);
    }

    bool UIManager::isInit(IPanelWindow &panel) {
        return panel.GetHwnd() != nullptr;
    }

} // namespace UI
