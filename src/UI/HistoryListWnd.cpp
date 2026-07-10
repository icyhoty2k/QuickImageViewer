#include "HistoryListWnd.h"
#include "../Platform/Constants.h"
#include "../Platform/FileHandler.h"
#include "../AppState.h"
#include "../Input/Shortcuts.h"
#include <algorithm>
#include <windowsx.h>
#include <filesystem>

#include "HistoryFoldersManager.h"


// ---------------------------------------------------------------------------
// HistoryListWnd.cpp  —  Last-visited folder history panel.
// ---------------------------------------------------------------------------

namespace UI {
    // -------------------------------------------------------------------------
    // File-scope state
    // -------------------------------------------------------------------------
    static HWND g_hHistOwner = nullptr;
    static int g_hoverRow = -1;

    // The single source of truth for history data
    static HistoryFoldersManager historyFoldersManager;
    static std::vector<RECT> g_rowRects;

    // -------------------------------------------------------------------------
    // PushFolderHistory  —  called by FileHandler after every folder load
    // -------------------------------------------------------------------------
    void PushFolderHistory(const std::wstring &folderPath) {
        if (folderPath.empty())
            return;

        // Create a quick reference to keep the code clean
        auto &history = historyFoldersManager.folderHistory;

        auto it = std::find(history.begin(), history.end(), folderPath);
        if (it != history.end())
            history.erase(it);

        history.insert(history.begin(), folderPath);

        if (history.size() > Constants::History::HISTORY_MAX_DIRS_TO_SHOW)
            history.resize(Constants::History::HISTORY_MAX_DIRS_TO_SHOW);

        // Immediately save state to disk
        historyFoldersManager.SaveHistoryToDisk();
    }

    const std::vector<std::wstring> &GetFolderHistory() {
        return historyFoldersManager.folderHistory;
    }

    // -------------------------------------------------------------------------
    // GetHistoryWindowBounds  —  centered on the parent's monitor
    // -------------------------------------------------------------------------
    static void GetHistoryWindowBounds(HWND hRef, int &x, int &y, int &w, int &h) {
        HMONITOR hMonitor = MonitorFromWindow(hRef, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {sizeof(mi)};
        GetMonitorInfoW(hMonitor, &mi);

        int monX = mi.rcMonitor.left;
        int monY = mi.rcMonitor.top;
        int monW = mi.rcMonitor.right - mi.rcMonitor.left;
        int monH = mi.rcMonitor.bottom - mi.rcMonitor.top;

        UINT dpi = GetDpiForWindow(hRef);
        int rowH = MulDiv(Constants::History::HISTORY_ROW_HEIGHT, dpi, 96);
        int padding = MulDiv(Constants::History::HISTORY_PADDING, dpi, 96);

        int entries = std::max(1, static_cast<int>(historyFoldersManager.folderHistory.size()));
        int totalH = padding * 2
                     + MulDiv(30, dpi, 96) // title row
                     + MulDiv(8, dpi, 96) // gap below title
                     + entries * rowH;

        w = static_cast<int>(monW * 0.30f);
        h = std::min(totalH, static_cast<int>(monH * 0.80f));
        x = monX + (monW - w) / 2;
        y = monY + (monH - h) / 2;
    }

    // -------------------------------------------------------------------------
    // Window procedure
    // -------------------------------------------------------------------------
    LRESULT HistoryListWnd::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(m_hWnd, &ps);
                RECT rc;
                GetClientRect(m_hWnd, &rc);

                UINT dpi = GetDpiForWindow(m_hWnd);
                int padding = MulDiv(Constants::History::HISTORY_PADDING, dpi, 96);
                int rowH = MulDiv(Constants::History::HISTORY_ROW_HEIGHT, dpi, 96);
                int fontSize = MulDiv(Constants::History::HISTORY_FONT_SIZE, dpi, 96);
                int titleSz = MulDiv(Constants::History::HISTORY_FONT_SIZE + 2, dpi, 96);
                int indexW = MulDiv(28, dpi, 96);

                HBRUSH hBg = CreateSolidBrush(RGB(18, 18, 18));
                FillRect(hdc, &rc, hBg);
                DeleteObject(hBg);

                SetBkMode(hdc, TRANSPARENT);

                HFONT hTitleFont = CreateFontW(
                        titleSz, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
                        CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");

                HFONT hBodyFont = CreateFontW(
                        fontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
                        CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");

                SelectObject(hdc, hTitleFont);
                SetTextColor(hdc, RGB(100, 200, 255));

                std::wstring title = L"Folder History  (last "
                                     + std::to_wstring(Constants::History::HISTORY_MAX_DIRS_TO_SHOW) + L" folders)";
                RECT titleRect = {
                    rc.left + padding, rc.top + padding,
                    rc.right - padding, rc.top + padding + titleSz + 4
                };
                DrawTextW(hdc, title.c_str(), -1, &titleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                int sepY = titleRect.bottom + MulDiv(4, dpi, 96);
                HPEN hPen = CreatePen(PS_SOLID, 1, RGB(50, 50, 50));
                HPEN hOldPen = (HPEN) SelectObject(hdc, hPen);
                MoveToEx(hdc, rc.left + padding, sepY, nullptr);
                LineTo(hdc, rc.right - padding, sepY);
                SelectObject(hdc, hOldPen);
                DeleteObject(hPen);

                SelectObject(hdc, hBodyFont);
                g_rowRects.clear();
                int y = sepY + MulDiv(6, dpi, 96);

                const auto &history = historyFoldersManager.folderHistory;

                if (history.empty()) {
                    SetTextColor(hdc, RGB(100, 100, 100));
                    RECT emptyRect = {rc.left + padding, y, rc.right - padding, y + rowH};
                    DrawTextW(hdc, L"No folders visited yet.", -1, &emptyRect,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                } else {
                    for (int i = 0; i < static_cast<int>(history.size()); ++i) {
                        RECT rowRect = {rc.left, y, rc.right, y + rowH};
                        g_rowRects.push_back(rowRect);

                        if (i == g_hoverRow) {
                            HBRUSH hHover = CreateSolidBrush(RGB(40, 60, 80));
                            FillRect(hdc, &rowRect, hHover);
                            DeleteObject(hHover);
                        }

                        SetTextColor(hdc, RGB(255, 204, 0));
                        std::wstring idxStr = std::to_wstring(i + 1);
                        RECT idxRect = {
                            rc.left + padding, y,
                            rc.left + padding + indexW, y + rowH
                        };
                        DrawTextW(hdc, idxStr.c_str(), -1, &idxRect,
                                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

                        SetTextColor(hdc, (i == g_hoverRow) ? RGB(255, 255, 255) : RGB(200, 200, 200));
                        RECT pathRect = {
                            rc.left + padding + indexW + MulDiv(10, dpi, 96),
                            y,
                            rc.right - padding,
                            y + rowH
                        };
                        DrawTextW(hdc, history[i].c_str(), -1, &pathRect,
                                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

                        y += rowH;
                    }
                }

                SelectObject(hdc, GetStockObject(SYSTEM_FONT));
                DeleteObject(hTitleFont);
                DeleteObject(hBodyFont);
                EndPaint(m_hWnd, &ps);
                return 0;
            }

            case WM_MOUSEMOVE: {
                int mx = GET_X_LPARAM(lParam);
                int my = GET_Y_LPARAM(lParam);

                int newHover = -1;
                for (int i = 0; i < static_cast<int>(g_rowRects.size()); ++i) {
                    const RECT &r = g_rowRects[i];
                    if (mx >= r.left && mx < r.right && my >= r.top && my < r.bottom) {
                        newHover = i;
                        break;
                    }
                }
                if (newHover != g_hoverRow) {
                    g_hoverRow = newHover;
                    SetCursor(LoadCursor(nullptr, (g_hoverRow >= 0) ? IDC_HAND : IDC_ARROW));
                    InvalidateRect(m_hWnd, nullptr, FALSE);
                }
                return 0;
            }

            case WM_LBUTTONUP: {
                int mx = GET_X_LPARAM(lParam);
                int my = GET_Y_LPARAM(lParam);

                for (int i = 0; i < static_cast<int>(g_rowRects.size()); ++i) {
                    const RECT &r = g_rowRects[i];
                    if (mx >= r.left && mx < r.right && my >= r.top && my < r.bottom) {
                        const std::wstring &folder = historyFoldersManager.folderHistory[i];
                        ShowWindow(m_hWnd, SW_HIDE);

                        // Pass the folder path directly
                        OpenDirectory(g_hHistOwner, folder);

                        return 0;
                    }
                }
                return 0;
            }

            case WM_MOUSELEAVE: {
                g_hoverRow = -1;
                InvalidateRect(m_hWnd, nullptr, FALSE);
                return 0;
            }

            case WM_KEYDOWN: {
                const auto &history = historyFoldersManager.folderHistory;
                switch (wParam) {
                    case Shortcuts::SC_PANEL_HISTORY_TOGGLE:
                        ToggleHistoryWindow();
                        return 0;

                    case VK_UP: {
                        if (!history.empty()) {
                            g_hoverRow = (g_hoverRow <= 0)
                                             ? static_cast<int>(history.size()) - 1
                                             : g_hoverRow - 1;
                            InvalidateRect(m_hWnd, nullptr, FALSE);
                        }
                        return 0;
                    }

                    case VK_DOWN: {
                        if (!history.empty()) {
                            g_hoverRow = (g_hoverRow < static_cast<int>(history.size()) - 1)
                                             ? g_hoverRow + 1
                                             : 0;
                            InvalidateRect(m_hWnd, nullptr, FALSE);
                        }
                        return 0;
                    }

                    case VK_RETURN: {
                        if (g_hoverRow >= 0 && g_hoverRow < static_cast<int>(history.size())) {
                            const std::wstring &folder = history[g_hoverRow];
                            ShowWindow(m_hWnd, SW_HIDE);

                            // Pass the folder path directly
                            OpenDirectory(g_hHistOwner, folder);
                        }
                        return 0;
                    }

                    default:
                        if (g_hHistOwner) return SendMessageW(g_hHistOwner, message, wParam, lParam);
                        break;
                }
                break;
            }

            case WM_CLOSE:
                ShowWindow(m_hWnd, SW_HIDE);
                return 0;
        }

        return DefWindowProcW(m_hWnd, message, wParam, lParam);
    }

    // -------------------------------------------------------------------------
    // Public API
    // -------------------------------------------------------------------------
    void HistoryListWnd::Init(HINSTANCE hInstance, HWND hParent, int8_t /*position*/) {
        Init(hInstance, hParent);
    }

    void HistoryListWnd::Init(HINSTANCE hInstance, HWND hParent) {
        g_hHistOwner = hParent;

        // LOAD DATA FROM DISK BEFORE CREATING THE WINDOW
        historyFoldersManager.LoadHistoryFromDisk();

        WNDCLASSW wc{};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = IPanelWindow::WindowRouter;
        wc.hInstance = hInstance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"QIV_HistoryWindow";
        RegisterClassW(&wc);

        int x, y, w, h;
        GetHistoryWindowBounds(hParent, x, y, w, h);

        CreateWindowExW(
                WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
                wc.lpszClassName,
                L"Folder History",
                WS_POPUP,
                x, y, w, h,
                hParent, nullptr, hInstance,
                this // passed to WM_NCCREATE so WindowRouter can store the this-ptr
                );

        if (!m_hWnd) return;

        SetLayeredWindowAttributes(m_hWnd, 0, Constants::THUMBNAIL_PANEL_WINDOW_OPACITY, LWA_ALPHA);
        ShowWindow(m_hWnd, SW_HIDE);
    }

    void HistoryListWnd::Show() {
        if (!m_hWnd) return;

        int x, y, w, h;
        GetHistoryWindowBounds(g_hHistOwner ? g_hHistOwner : m_hWnd, x, y, w, h);
        SetWindowPos(m_hWnd, HWND_TOPMOST, x, y, w, h, SWP_FRAMECHANGED);

        g_hoverRow = 0;
        ShowWindow(m_hWnd, SW_SHOW);
        SetForegroundWindow(m_hWnd);
        InvalidateRect(m_hWnd, nullptr, TRUE);
    }

    void HistoryListWnd::Toggle() {
        if (!m_hWnd) return;
        IsWindowVisible(m_hWnd) ? Hide() : Show();
    }

    void HistoryListWnd::ToggleHistoryWindow() {
        Toggle(); // delegates — one place to maintain
    }

    void HistoryListWnd::PushFolderHistory(const std::wstring &folderPath) {
        UI::PushFolderHistory(folderPath); // delegates to file-scope function
    }

    const std::vector<std::wstring> &HistoryListWnd::GetFolderHistory() {
        return UI::GetFolderHistory();
    }
} // namespace UI
