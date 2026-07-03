#include "HistoryListWnd.h"
#include "../Platform/Constants.h"
#include "../Platform/FileHandler.h"
#include "../AppState.h"
#include "../Input/Shortcuts.h"
#include <algorithm>
#include <windowsx.h>
#include <filesystem>

// ---------------------------------------------------------------------------
// HistoryWindow.cpp  —  Last-visited folder history panel.
//
// Architecture:
//   - Standalone WS_POPUP | WS_EX_TOPMOST | WS_EX_LAYERED window
//   - Rendered with GDI (same approach as HelpWindow — no separate D2D swap chain needed)
//   - g_folderHistory  : deque of folder paths, max Constants::HISTORY_MAX_DIRS entries
//   - PushFolderHistory(): called by FileHandler after every folder load
//   - Click on a row   : calls OpenSpecificImage with first image in that folder
//   - F7 / Esc         : toggle / hide
//
// Layout (centered on the parent monitor):
//   - Fixed width: 70% of monitor width
//   - Fixed height: header row + N entry rows + footer row (all DPI-scaled)
//   - Each row: index number (yellow) + full folder path (white)
//   - Hovered row: highlighted background
// ---------------------------------------------------------------------------

namespace UI {
    // -------------------------------------------------------------------------
    // File-scope state
    // -------------------------------------------------------------------------
    static HWND g_hHistWnd = nullptr;
    static HWND g_hHistOwner = nullptr;
    static int g_hoverRow = -1; // which history row the mouse is over

    // The history list: index 0 = most recently visited
    static std::vector<std::wstring> g_folderHistory;

    // Per-row hit rectangles rebuilt on every WM_PAINT (in client coords)
    static std::vector<RECT> g_rowRects;

    // -------------------------------------------------------------------------
    // PushFolderHistory  —  called by FileHandler after every folder load
    // -------------------------------------------------------------------------
    void PushFolderHistory(const std::wstring &folderPath) {
        if (folderPath.empty()) return;

        // Remove duplicate if already present (promote to front)
        auto it = std::find(g_folderHistory.begin(), g_folderHistory.end(), folderPath);
        if (it != g_folderHistory.end()) {
            g_folderHistory.erase(it);
        }

        g_folderHistory.insert(g_folderHistory.begin(), folderPath);

        // Trim to configured maximum
        if (static_cast<int>(g_folderHistory.size()) > Constants::HISTORY_MAX_DIRS) {
            g_folderHistory.resize(static_cast<size_t>(Constants::HISTORY_MAX_DIRS));
        }
    }

    const std::vector<std::wstring> &GetFolderHistory() {
        return g_folderHistory;
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
        int rowH = MulDiv(Constants::HISTORY_ROW_HEIGHT, dpi, 96);
        int padding = MulDiv(Constants::HISTORY_PADDING, dpi, 96);

        // Height: header + one row per history entry + footer/padding
        int entries = std::max(1, static_cast<int>(g_folderHistory.size()));
        int totalH = padding * 2 // top + bottom padding
                     + MulDiv(30, dpi, 96) // title row
                     + MulDiv(8, dpi, 96) // gap below title
                     + entries * rowH; // content rows

        w = static_cast<int>(monW * 0.30f);
        h = std::min(totalH, static_cast<int>(monH * 0.80f));
        x = monX + (monW - w) / 2;
        y = monY + (monH - h) / 2;
    }

    // -------------------------------------------------------------------------
    // Window procedure
    // -------------------------------------------------------------------------
    LRESULT CALLBACK HistoryListWnd::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
            // -----------------------------------------------------------------
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(hWnd, &ps);
                RECT rc;
                GetClientRect(hWnd, &rc);

                UINT dpi = GetDpiForWindow(hWnd);
                int padding = MulDiv(Constants::HISTORY_PADDING, dpi, 96);
                int rowH = MulDiv(Constants::HISTORY_ROW_HEIGHT, dpi, 96);
                int fontSize = MulDiv(Constants::HISTORY_FONT_SIZE, dpi, 96);
                int titleSz = MulDiv(Constants::HISTORY_FONT_SIZE + 2, dpi, 96);
                int indexW = MulDiv(28, dpi, 96); // width of the "#N" column

                // Dark background
                HBRUSH hBg = CreateSolidBrush(RGB(18, 18, 18));
                FillRect(hdc, &rc, hBg);
                DeleteObject(hBg);

                SetBkMode(hdc, TRANSPARENT);

                // Title font (slightly larger, bold)
                HFONT hTitleFont = CreateFontW(
                        titleSz, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
                        CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");

                // Body font
                HFONT hBodyFont = CreateFontW(
                        fontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
                        CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");

                // ---- Title row ----
                SelectObject(hdc, hTitleFont);
                SetTextColor(hdc, RGB(100, 200, 255));

                std::wstring title = L"Folder History  (last "
                                     + std::to_wstring(Constants::HISTORY_MAX_DIRS) + L" folders)";
                RECT titleRect = {
                    rc.left + padding, rc.top + padding,
                    rc.right - padding, rc.top + padding + titleSz + 4
                };
                DrawTextW(hdc, title.c_str(), -1, &titleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                // Thin separator line below title
                int sepY = titleRect.bottom + MulDiv(4, dpi, 96);
                HPEN hPen = CreatePen(PS_SOLID, 1, RGB(50, 50, 50));
                HPEN hOldPen = (HPEN) SelectObject(hdc, hPen);
                MoveToEx(hdc, rc.left + padding, sepY, nullptr);
                LineTo(hdc, rc.right - padding, sepY);
                SelectObject(hdc, hOldPen);
                DeleteObject(hPen);

                // ---- History rows ----
                SelectObject(hdc, hBodyFont);

                g_rowRects.clear();
                int y = sepY + MulDiv(6, dpi, 96);

                if (g_folderHistory.empty()) {
                    // Empty state message
                    SetTextColor(hdc, RGB(100, 100, 100));
                    RECT emptyRect = {rc.left + padding, y, rc.right - padding, y + rowH};
                    DrawTextW(hdc, L"No folders visited yet.", -1, &emptyRect,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                } else {
                    for (int i = 0; i < static_cast<int>(g_folderHistory.size()); ++i) {
                        RECT rowRect = {rc.left, y, rc.right, y + rowH};
                        g_rowRects.push_back(rowRect);

                        // Hover highlight
                        if (i == g_hoverRow) {
                            HBRUSH hHover = CreateSolidBrush(RGB(40, 60, 80));
                            FillRect(hdc, &rowRect, hHover);
                            DeleteObject(hHover);
                        }

                        // Index number column (yellow)
                        SetTextColor(hdc, RGB(255, 204, 0));
                        std::wstring idxStr = std::to_wstring(i + 1);
                        RECT idxRect = {
                            rc.left + padding, y,
                            rc.left + padding + indexW, y + rowH
                        };
                        DrawTextW(hdc, idxStr.c_str(), -1, &idxRect,
                                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

                        // Folder path (white, clipped to right edge with ellipsis)
                        SetTextColor(hdc, (i == g_hoverRow) ? RGB(255, 255, 255) : RGB(200, 200, 200));
                        RECT pathRect = {
                            rc.left + padding + indexW + MulDiv(10, dpi, 96),
                            y,
                            rc.right - padding,
                            y + rowH
                        };
                        DrawTextW(hdc, g_folderHistory[i].c_str(), -1, &pathRect,
                                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

                        y += rowH;
                    }
                }

                // Cleanup fonts
                SelectObject(hdc, GetStockObject(SYSTEM_FONT));
                DeleteObject(hTitleFont);
                DeleteObject(hBodyFont);

                EndPaint(hWnd, &ps);
                return 0;
            }

            // -----------------------------------------------------------------
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
                    // Update cursor: hand on a valid row, arrow otherwise
                    SetCursor(LoadCursor(nullptr, (g_hoverRow >= 0) ? IDC_HAND : IDC_ARROW));
                    InvalidateRect(hWnd, nullptr, FALSE);
                }
                return 0;
            }

            // -----------------------------------------------------------------
            case WM_LBUTTONUP: {
                int mx = GET_X_LPARAM(lParam);
                int my = GET_Y_LPARAM(lParam);

                for (int i = 0; i < static_cast<int>(g_rowRects.size()); ++i) {
                    const RECT &r = g_rowRects[i];
                    if (mx >= r.left && mx < r.right && my >= r.top && my < r.bottom) {
                        // Navigate to the first image in the selected folder
                        const std::wstring &folder = g_folderHistory[i];
                        ShowWindow(hWnd, SW_HIDE);

                        // Find the first image file in the folder and open it
                        try {
                            bool imageFound = false;
                            for (const auto &entry: std::filesystem::directory_iterator(folder)) {
                                if (entry.is_regular_file() && is_image_ext(entry.path())) {
                                    OpenSpecificImage(g_hHistOwner, entry.path().wstring());
                                    imageFound = true;
                                    break; // Valid image found and passed to viewer
                                }
                            }

                            if (!imageFound) {
                                // Optional: Log or display a message that no images were found in this folder
                            }
                        } catch (...) {
                            // Folder may be inaccessible or deleted
                        }
                        return 0;
                    }
                }
                return 0;
            }

            // -----------------------------------------------------------------
            case WM_MOUSELEAVE: {
                g_hoverRow = -1;
                InvalidateRect(hWnd, nullptr, FALSE);
                return 0;
            }

            // -----------------------------------------------------------------
            case WM_KEYDOWN: {
                switch (wParam) {
                    case Shortcuts::SC_LOCAL_HIDE:
                        ShowWindow(hWnd, SW_HIDE);
                        return 0;
                    case Shortcuts::SC_PANEL_HISTORY_TOGGLE:
                        ToggleHistoryWindow();
                        return 0;

                    case VK_UP: {
                        if (!g_folderHistory.empty()) {
                            g_hoverRow = (g_hoverRow <= 0) ? (int) g_folderHistory.size() - 1 : g_hoverRow - 1;
                            InvalidateRect(hWnd, nullptr, FALSE);
                        }
                        return 0;
                    }


                    case VK_DOWN: {
                        if (!g_folderHistory.empty()) {
                            g_hoverRow = (g_hoverRow < (int) g_folderHistory.size() - 1) ? g_hoverRow + 1 : 0;
                            InvalidateRect(hWnd, nullptr, FALSE);
                        }
                        return 0;
                    }

                    case VK_RETURN: // Select the highlighted folder
                    case VK_SPACE: {
                        if (g_hoverRow >= 0 && g_hoverRow < (int) g_folderHistory.size()) {
                            // Trigger the same logic as LBUTTONUP
                            const std::wstring &folder = g_folderHistory[g_hoverRow];
                            ShowWindow(hWnd, SW_HIDE);
                            bool loaded = false;
                            try {
                                for (const auto &entry: std::filesystem::directory_iterator(folder)) {
                                    if (entry.is_regular_file() && is_image_ext(entry.path().extension().wstring())) {
                                        // Found a valid image — tell the main viewer to load it
                                        OpenSpecificImage(g_hHistOwner, entry.path().wstring());
                                        loaded = true;
                                        break; // Stop looking after the first image is found
                                    }
                                }
                            } catch (...) {
                                // Handle inaccessible directory
                            }
                        }
                        return 0;
                    }

                    default:
                        if (g_hHistOwner) return SendMessageW(g_hHistOwner, message, wParam, lParam);
                        break;
                }
                break;
            }

            // -----------------------------------------------------------------
            case WM_CLOSE:
                ShowWindow(hWnd, SW_HIDE);
                return 0;
        }

        return DefWindowProcW(hWnd, message, wParam, lParam);
    }

    // -------------------------------------------------------------------------
    // Public API
    // -------------------------------------------------------------------------
    void HistoryListWnd::Init(HINSTANCE hInstance, HWND hParent, int8_t position) {}

    void HistoryListWnd::Init(HINSTANCE hInstance, HWND hParent) {
        g_hHistOwner = hParent;

        WNDCLASSW wc{};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = HistWndProc;
        wc.hInstance = hInstance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"QIV_HistoryWindow";
        RegisterClassW(&wc);

        int x, y, w, h;
        GetHistoryWindowBounds(hParent, x, y, w, h);

        g_hHistWnd = CreateWindowExW(
                WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
                wc.lpszClassName,
                L"Folder History",
                WS_POPUP,
                x, y, w, h,
                hParent, nullptr, hInstance, nullptr
                );

        if (!g_hHistWnd) return;

        SetLayeredWindowAttributes(g_hHistWnd, 0, Constants::CACHE_WINDOW_OPACITY, LWA_ALPHA);
        ShowWindow(g_hHistWnd, SW_HIDE);
    }

    void ToggleHistoryWindow() {
        if (!g_hHistWnd) return;

        if (IsWindowVisible(g_hHistWnd)) {
            ShowWindow(g_hHistWnd, SW_HIDE);
        } else {
            // Find current folder index in the history list
            // g_hoverRow = -1;
            // const std::wstring &currentFolder = std::filesystem::path(g_app.playlist[g_app.currentIndex]).parent_path().wstring();
            //
            // const auto &history = GetFolderHistory();
            // for (int i = 0; i < (int) history.size(); ++i) {
            //     if (history[i] == currentFolder) {
            //         g_hoverRow = i;
            //         break;
            //     }
            // }
            // ----
            // Recalculate size every open (history list may have grown)
            int x, y, w, h;
            GetHistoryWindowBounds(g_hHistOwner ? g_hHistOwner : g_hHistWnd, x, y, w, h);
            SetWindowPos(g_hHistWnd, HWND_TOPMOST, x, y, w, h, SWP_FRAMECHANGED);

            g_hoverRow = 0;
            ShowWindow(g_hHistWnd, SW_SHOW);
            SetForegroundWindow(g_hHistWnd);
            InvalidateRect(g_hHistWnd, nullptr, TRUE);
        }
    }
} // namespace UI
