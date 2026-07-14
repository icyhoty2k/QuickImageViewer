#include "HelpWnd.h"
#include "../Platform/Constants.h"
#include "../AppState.h"
#include "Shortcuts.h"
#include <string>
#include <dwmapi.h>
#include <shellapi.h>
#include <algorithm>
#include <shlobj.h>
#include <fstream>
#include <windowsx.h>

namespace UI {
    void HelpWnd::Init(HINSTANCE hInstance, HWND hParent, int8_t /*position*/) {
        HelpWnd::Init(hInstance, hParent);
    }

    void HelpWnd::Init(HINSTANCE hInstance, HWND hParent) {
        m_fullTitle = std::wstring(Constants::BASE_NAME) + L" v" + Constants::APP_VERSION;
        UINT dpi = static_cast<UINT>(app.dpiScale * 96.0f);
        InitFloating(hInstance, hParent, L"QIV_HelpWindow", Constants::APP_TASKBAR_NAME,
                     MulDiv(600, dpi, 96), MulDiv(800, dpi, 96));
        BuildHelpContent();
    }

    struct Section {
        std::wstring title;
        std::wstring icon;
        COLORREF color;
        std::vector<std::pair<std::wstring, std::wstring> > entries; // shortcut, description
    };

    void HelpWnd::BuildHelpContent() {
        m_entries.clear();

        auto Add = [this](const std::wstring &shortcut, const std::wstring &desc, int section) {
            m_entries.push_back({shortcut, desc, section});
        };

        // Section 0: Viewing & Navigation (Cyan)
        Add(L"Left / Right", L"Previous / Next image", 0);
        Add(L"Space / Shift+Space", L"Next / Previous image", 0);
        Add(L"Mouse Wheel", L"Navigate images", 0);
        Add(L"Backspace / Shift+Backspace", L"Jump to first / last image in folder", 0);
        Add(L"E", L"Toggle between last and current image  (image-level back)", 0);
        Add(L"Q", L"Toggle previous / current folder", 0);
        Add(L"L", L"Open file location in Explorer", 0);
        Add(L"F2", L"Open file dialog", 0);
        Add(L"Ctrl+F", L"Find image by filename  — fuzzy match, or wildcards: * any chars, ? any one char  (e.g. *.jpg, photo_??_2024.*)\n                In Find panel: type # to switch to Jump-to  •  searches current folder + VRAM cache", 0);
        Add(L"Ctrl+G", L"Jump to image by number\n                In Jump-to panel: type @ to switch to Find", 0);
        Add(L"Tab", L"Toggle History panel", 0);
        Add(L"F1", L"Toggle Help window", 0);
        Add(L"M", L"Toggle Image Info / EXIF panel", 0);
        Add(L"K", L"Toggle Statistics panel  (load time, codec, cache info)", 0);
        Add(L"F3 / F4", L"Toggle / Move VRAM cache panel", 0);
        Add(L"F5 / F6", L"Toggle / Move directory panel", 0);
        Add(L"F12", L"Clear VRAM cache", 0);
        Add(L"1 – 5", L"Switch view mode", 0);
        Add(L"Ctrl+F1", L"Slideshow: start / stop", 0);
        Add(L"Space  (slideshow)", L"Slideshow: pause / resume", 0);
        Add(L"R  (slideshow)", L"Slideshow: toggle loop", 0);
        Add(L"S  (slideshow)", L"Slideshow: toggle shuffle", 0);
        Add(L"T  (slideshow)", L"Slideshow: cycle transition  Cut → Fade → Push → Zoom", 0);

        // Section 1: Interaction & Control (Orange)
        Add(L"Left Click + drag", L"Quick 3× zoom and pan", 1);
        Add(L"Right Click + drag", L"Move window", 1);
        Add(L"RMB + Left Click", L"Open current image in Explorer", 1);
        Add(L"Middle Click + drag", L"Resize window", 1);
        Add(L"Middle Click", L"Reset window size and center image", 1);
        Add(L"Up / Down  /  Ctrl+Wheel", L"Zoom in / out", 1);
        Add(L"Numpad +  /  Numpad -  /  Numpad *", L"Zoom in / out / reset", 1);
        Add(L"Shift+Numpad+  /  Shift++", L"Resize window larger (from center)", 1);
        Add(L"Shift+Numpad-  /  Shift+-", L"Resize window smaller (from center)", 1);
        Add(L"Shift+Wheel  /  H-Wheel", L"Adjust window opacity", 1);
        Add(L"RMB + Wheel", L"Zoom while holding RMB", 1);
        Add(L"RMB + H-Wheel", L"Resize from center", 1);
        Add(L"W / A / S / D", L"Pan viewport (when image overflows window)", 1);
        Add(L"Shift+W / A / S / D", L"Move window 20 px per keypress", 1);
        Add(L"Alt+A / Alt+D", L"Snap to left / right half of screen", 1);
        Add(L"Alt+W / Alt+S", L"Snap to top / bottom half of screen", 1);
        Add(L"Alt+Q / Alt+E", L"Snap to top-left / top-right quarter", 1);
        Add(L"Alt+Z / Alt+C", L"Snap to bottom-left / bottom-right quarter", 1);
        Add(L"Alt+X", L"Restore window to defaults", 1);
        Add(L"F / F11 / Enter / Ctrl+Shift+T", L"Toggle fullscreen", 1);
        Add(L"Ctrl+T  /  Ctrl+A", L"Toggle always on top  (applies to all visible panels)", 1);
        Add(L"N / I / Ctrl+0", L"Master overlay toggle", 1);
        Add(L"Ctrl+1 – Ctrl+9", L"Toggle individual overlay slots", 1);
        Add(L"Ctrl+Alt+1 – Ctrl+Alt+9", L"Toggle overlay compact mode", 1);
        Add(L"O", L"Cycle overlay layout", 1);
        Add(L"P", L"Toggle overlay text backgrounds", 1);

        // Section 2: Effects & Customization (Purple)
        Add(L"R / Shift+R", L"Rotate 90° CW / CCW", 2);
        Add(L"H / V", L"Flip horizontally / vertically", 2);
        Add(L"Delete", L"Toggle grayscale", 2);
        Add(L"Insert", L"Toggle invert colors", 2);
        Add(L"Home", L"Toggle sepia", 2);
        Add(L"End", L"Toggle solarize", 2);
        Add(L"Page Up / Page Down", L"Toggle outline / B&W threshold", 2);
        Add(L"[ / ]", L"Saturation − / +", 2);
        Add(L"' / \\", L"Brightness + / −", 2);
        Add(L". / /", L"Contrast + / −", 2);
        Add(L"+ / −", L"Gamma + / −", 2);
        Add(L"Numpad 0", L"Reset all color effects", 2);
        Add(L"`", L"Toggle effects on / off", 2);
        Add(L"Ctrl+C", L"Copy image to clipboard", 2);
        Add(L"Ctrl+S", L"Save image — choose format in dialog  (PNG, JPEG, BMP, TIFF, GIF)\n                Rotation, flip and color effects are baked into the saved file", 2);
        Add(L"Ctrl+N", L"Open new window", 2);
        Add(L"Esc / Ctrl+W", L"Hide to background", 2);
        Add(L"Ctrl+Q", L"Hard quit", 2);
        Add(L"Shift+Delete", L"Reset all  (layout + effects)", 2);
        Add(L"Ctrl+E", L"Export this help to Desktop as text file", 2);
        Add(L"Space  (History panel)", L"Toggle favorite", 2);
        Add(L"Shift+Enter  (History panel)", L"Spawn directory panel", 2);
        Add(L"Del  (History panel)", L"Delete hovered entry", 2);
        Add(L"Ctrl+Z  (History panel)", L"Restore last deleted entry", 2);
        Add(L"Ctrl+Shift+Del  (History panel)", L"Clear history, keep favorites", 2);
        Add(L"Ctrl+Alt+Shift+Del  (History panel)", L"Clear favorites, keep history", 2);

        // Section 3: Advanced & Power User (Green)
        Add(L"Ctrl+Alt+Shift+0", L"Sort by name  (press again: A→Z / Z→A)", 3);
        Add(L"Ctrl+Alt+Shift+9", L"Sort by date modified  (press again: newest ↔ oldest)", 3);
        Add(L"Ctrl+Alt+Shift+8", L"Sort by file size  (press again: largest ↔ smallest)", 3);
        Add(L"Ctrl+Alt+Shift+7", L"Sort by extension  (press again: A→Z / Z→A)", 3);
        Add(L"Ctrl+Alt+Shift+6", L"Sort by physical disk order  (HDD performance mode)", 3);
        Add(L"Ctrl+Alt+Numpad +/-", L"Theme brightness: lighter / darker", 3);
        Add(L"Ctrl+Alt+Numpad 0", L"Theme brightness: reset to default", 3);
        Add(L"Ctrl+Shift+Numpad *", L"Toggle window corners: round ↔ square", 3);
        Add(L"Ctrl+Shift+Numpad /", L"Cycle backdrop: None → Mica → Acrylic → MicaAlt", 3);
    }

    void HelpWnd::ExportToText() const {
        wchar_t path[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOP, nullptr, 0, path))) {
            wcscat_s(path, MAX_PATH, L"\\QIV_Help.txt");

            std::wofstream file(path);
            if (file.is_open()) {
                file << L"Quick Image Viewer - Keyboard Shortcuts\n";
                file << L"========================================\n\n";

                for (const auto &entry: m_entries) {
                    file << entry.shortcut << L"  →  " << entry.description << L"\n";
                }
                file.close();

                MessageBoxW(m_hWnd, (std::wstring(L"Help exported to:\n") + path).c_str(),
                            L"Export Complete", MB_OK | MB_ICONINFORMATION);
            }
        }
    }

    void HelpWnd::Show() {
        if (m_hWnd) {
            if (m_hParent) {
                RECT rcParent, rcHelp;
                GetWindowRect(m_hParent, &rcParent);
                GetWindowRect(m_hWnd, &rcHelp);
                int x = rcParent.left + ((rcParent.right - rcParent.left) - (rcHelp.right - rcHelp.left)) / 2;
                int y = rcParent.top + ((rcParent.bottom - rcParent.top) - (rcHelp.bottom - rcHelp.top)) / 2;
                SetWindowPos(m_hWnd, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
            } else {
                ShowWindow(m_hWnd, SW_SHOW);
            }
            SetForegroundWindow(m_hWnd);
        }
    }

    bool HelpWnd::OnKeyDown(WPARAM vk, bool ctrl, bool /*shift*/, bool /*alt*/) {
        if (vk == Shortcuts::SC_PANEL_HELP_TOGGLE) {
            Hide();
            return true;
        }
        if (vk == 'E' && ctrl) {
            ExportToText();
            return true;
        }
        if (vk == VK_PRIOR) {
            m_scrollOffsetY -= static_cast<int>(300 * app.dpiScale);
            InvalidateRect(m_hWnd, nullptr, FALSE);
            return true;
        }
        if (vk == VK_NEXT) {
            m_scrollOffsetY += static_cast<int>(300 * app.dpiScale);
            InvalidateRect(m_hWnd, nullptr, FALSE);
            return true;
        }
        if (vk == VK_HOME) {
            m_scrollOffsetY = 0;
            InvalidateRect(m_hWnd, nullptr, FALSE);
            return true;
        }
        if (vk == VK_END) {
            RECT rc;
            GetClientRect(m_hWnd, &rc);
            UINT dpi = static_cast<UINT>(app.dpiScale * 96.0f);
            int padding = MulDiv(30, dpi, 96);
            int titleFontSize = MulDiv(32, dpi, 96);
            int subtitleFontSize = MulDiv(14, dpi, 96);
            int contentTop = padding + titleFontSize + MulDiv(30, dpi, 96) + subtitleFontSize;
            int contentHeight = rc.bottom - contentTop - padding;
            m_scrollOffsetY = std::max(0, m_totalContentHeight - contentHeight);
            InvalidateRect(m_hWnd, nullptr, FALSE);
            return true;
        }
        return false;
    }

    LRESULT HelpWnd::HandlePanelMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(m_hWnd, &ps);
                RECT rc;
                GetClientRect(m_hWnd, &rc);

                HBRUSH hBrush = CreateSolidBrush(GetBgColor());
                FillRect(hdc, &rc, hBrush);
                DeleteObject(hBrush);

                UINT dpi = static_cast<UINT>(app.dpiScale * 96.0f);
                int padding = MulDiv(30, dpi, 96);
                int descFontSize = MulDiv(13, dpi, 96);
                int shortcutFontSize = static_cast<int>(descFontSize * SHORTCUT_SIZE_MULTIPLIER);
                int titleFontSize = MulDiv(32, dpi, 96);
                int subtitleFontSize = MulDiv(14, dpi, 96);
                int sectionHeaderSize = MulDiv(18, dpi, 96);
                int lineHeight = shortcutFontSize + descFontSize + MulDiv(12, dpi, 96);
                int sbWidth = MulDiv(10, dpi, 96);

                SetBkMode(hdc, TRANSPARENT);

                // Section colors with theme support
                COLORREF sectionColors[4] = {
                    Constants::Theme::ThemedColor(Constants::Theme::HelpWindow::SECTION_CYAN_R,
                                                  Constants::Theme::HelpWindow::SECTION_CYAN_G,
                                                  Constants::Theme::HelpWindow::SECTION_CYAN_B, app.themeFactor),
                    Constants::Theme::ThemedColor(Constants::Theme::HelpWindow::SECTION_ORANGE_R,
                                                  Constants::Theme::HelpWindow::SECTION_ORANGE_G,
                                                  Constants::Theme::HelpWindow::SECTION_ORANGE_B, app.themeFactor),
                    Constants::Theme::ThemedColor(Constants::Theme::HelpWindow::SECTION_PURPLE_R,
                                                  Constants::Theme::HelpWindow::SECTION_PURPLE_G,
                                                  Constants::Theme::HelpWindow::SECTION_PURPLE_B, app.themeFactor),
                    Constants::Theme::ThemedColor(Constants::Theme::HelpWindow::SECTION_GREEN_R,
                                                  Constants::Theme::HelpWindow::SECTION_GREEN_G,
                                                  Constants::Theme::HelpWindow::SECTION_GREEN_B, app.themeFactor)
                };

                const wchar_t *sectionIcons[4] = {
                    L"⌨️ ",
                    L"🖱️ ",
                    L"🎨 ",
                    L"⚙️ "
                };

                const wchar_t *sectionTitles[4] = {
                    L"VIEWING & NAVIGATION",
                    L"INTERACTION & CONTROL",
                    L"EFFECTS & CUSTOMIZATION",
                    L"ADVANCED & POWER USER"
                };

                COLORREF yellowKey = Constants::Theme::ThemedColor(
                        Constants::Theme::HelpWindow::SHORTCUT_KEY_R,
                        Constants::Theme::HelpWindow::SHORTCUT_KEY_G,
                        Constants::Theme::HelpWindow::SHORTCUT_KEY_B, app.themeFactor);
                COLORREF whiteDesc = Constants::Theme::ThemedGray(
                        Constants::Theme::HelpWindow::DESCRIPTION, app.themeFactor);

                // Fonts — cached per DPI, recreated only on DPI change
                if (static_cast<int>(dpi) != m_cachedFontDpi) {
                    if (m_hFontTitle)    { DeleteObject(m_hFontTitle);    m_hFontTitle    = nullptr; }
                    if (m_hFontSubtitle) { DeleteObject(m_hFontSubtitle); m_hFontSubtitle = nullptr; }
                    if (m_hFontSection)  { DeleteObject(m_hFontSection);  m_hFontSection  = nullptr; }
                    if (m_hFontShortcut) { DeleteObject(m_hFontShortcut); m_hFontShortcut = nullptr; }
                    if (m_hFontDesc)     { DeleteObject(m_hFontDesc);     m_hFontDesc     = nullptr; }
                    if (m_hFontFooter)   { DeleteObject(m_hFontFooter);   m_hFontFooter   = nullptr; }
                    m_hFontTitle    = CreateFontW(titleFontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                                  DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
                                                  CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
                    m_hFontSubtitle = CreateFontW(subtitleFontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                                  DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
                                                  CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
                    m_hFontSection  = CreateFontW(sectionHeaderSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                                  DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
                                                  CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
                    m_hFontShortcut = CreateFontW(shortcutFontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                                  DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
                                                  CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
                    m_hFontDesc     = CreateFontW(descFontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                                  DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
                                                  CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
                    m_hFontFooter   = CreateFontW(MulDiv(11, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                                  DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
                                                  CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
                    m_cachedFontDpi = static_cast<int>(dpi);
                }

                // Draw centered title

                SetTextColor(hdc, Constants::Theme::ThemedColor(
                        Constants::Theme::HelpWindow::TITLE_R,
                        Constants::Theme::HelpWindow::TITLE_G,
                        Constants::Theme::HelpWindow::TITLE_B, app.themeFactor));
                RECT titleRect = {rc.left + padding, rc.top + padding, rc.right - padding - sbWidth, rc.top + padding + titleFontSize};
                DrawTextW(hdc, L"Quick Image Viewer", -1, &titleRect, DT_CENTER | DT_TOP);

                // Draw subtitle
                RECT subtitleRect = {
                    rc.left + padding, titleRect.bottom + MulDiv(5, dpi, 96),
                    rc.right - padding - sbWidth, titleRect.bottom + MulDiv(25, dpi, 96)
                };
                SelectObject(hdc, m_hFontSubtitle);
                SetTextColor(hdc, Constants::Theme::ThemedGray(Constants::Theme::HelpWindow::SUBTITLE, app.themeFactor));
                DrawTextW(hdc, L"Fast keyboard & mouse shortcuts", -1, &subtitleRect, DT_CENTER | DT_TOP);

                // Content area
                int contentTop = subtitleRect.bottom + MulDiv(25, dpi, 96);
              
                int contentHeight = rc.bottom - contentTop - padding;

                // Calculate total height
                if (m_totalContentHeight == 0) {
                    m_totalContentHeight = 0;
                    for (int s = 0; s < 4; ++s) {
                        m_totalContentHeight += sectionHeaderSize + MulDiv(15, dpi, 96); // Section header
                        for (const auto &entry: m_entries) {
                            if (entry.sectionId == s) {
                                m_totalContentHeight += lineHeight;
                            }
                        }
                        m_totalContentHeight += MulDiv(15, dpi, 96); // Gap between sections
                    }
                }

                int maxScroll = std::max(0, m_totalContentHeight - contentHeight);
                m_scrollOffsetY = std::clamp(m_scrollOffsetY, 0, maxScroll);

                // Clipping
                HRGN hrgn = CreateRectRgn(rc.left + padding, contentTop, rc.right - padding - sbWidth, rc.bottom - padding);
                SelectClipRgn(hdc, hrgn);

                int y = contentTop - m_scrollOffsetY;
                // Draw sections with entries
                for (int s = 0; s < 4; ++s) {
                    // Section header
                    SelectObject(hdc, m_hFontSection);
                    SetTextColor(hdc, sectionColors[s]);
                    std::wstring sectionText = std::wstring(sectionIcons[s]) + sectionTitles[s];
                    RECT sectionRect = {rc.left + padding, y, rc.right - padding - sbWidth, y + sectionHeaderSize};
                    DrawTextW(hdc, sectionText.c_str(), -1, &sectionRect, DT_LEFT | DT_TOP);
                    y += sectionHeaderSize + MulDiv(10, dpi, 96);

                    // Entries
                    for (const auto &entry: m_entries) {
                        if (entry.sectionId == s) {
                            // Shortcut (yellow, bigger)
                            SelectObject(hdc, m_hFontShortcut);
                            SetTextColor(hdc, yellowKey);
                            RECT shortcutRect = {
                                rc.left + padding + MulDiv(20, dpi, 96), y,
                                rc.right - padding - sbWidth, y + shortcutFontSize
                            };
                            DrawTextW(hdc, entry.shortcut.c_str(), -1, &shortcutRect, DT_LEFT | DT_TOP);

                            // Description (white, smaller)
                            SelectObject(hdc, m_hFontDesc);
                            SetTextColor(hdc, whiteDesc);
                            RECT descRect = {
                                rc.left + padding + MulDiv(20, dpi, 96), y + shortcutFontSize + MulDiv(2, dpi, 96),
                                rc.right - padding - sbWidth, y + shortcutFontSize + descFontSize + MulDiv(10, dpi, 96)
                            };
                            DrawTextW(hdc, entry.description.c_str(), -1, &descRect, DT_LEFT | DT_TOP);

                            y += lineHeight;
                        }
                    }
                    y += MulDiv(15, dpi, 96); // Gap between sections
                }

                SelectClipRgn(hdc, nullptr);
                DeleteObject(hrgn);

                // Scrollbar
                if (maxScroll > 0) {
                    int sbX = rc.right - sbWidth - MulDiv(5, dpi, 96);
                    int sbTrackTop = contentTop;
                    int sbTrackBottom = rc.bottom - padding;
                    int sbTrackHeight = sbTrackBottom - sbTrackTop;

                    int thumbHeight = std::max(MulDiv(30, dpi, 96),
                                               (sbTrackHeight * contentHeight) / m_totalContentHeight);
                    int thumbY = sbTrackTop + (sbTrackHeight - thumbHeight) * m_scrollOffsetY / maxScroll;

                    HBRUSH hTrack = CreateSolidBrush(Constants::Theme::ThemedColor(
                            Constants::Theme::HelpWindow::SCROLLBAR_TRACK_R,
                            Constants::Theme::HelpWindow::SCROLLBAR_TRACK_G,
                            Constants::Theme::HelpWindow::SCROLLBAR_TRACK_B, app.themeFactor));
                    RECT trackRect = {sbX, sbTrackTop, sbX + sbWidth, sbTrackBottom};
                    FillRect(hdc, &trackRect, hTrack);
                    DeleteObject(hTrack);

                    HBRUSH hThumb = CreateSolidBrush(Constants::Theme::ThemedColor(
                            Constants::Theme::HelpWindow::SCROLLBAR_THUMB_R,
                            Constants::Theme::HelpWindow::SCROLLBAR_THUMB_G,
                            Constants::Theme::HelpWindow::SCROLLBAR_THUMB_B, app.themeFactor));
                    RECT thumbRect = {sbX, thumbY, sbX + sbWidth, thumbY + thumbHeight};
                    FillRect(hdc, &thumbRect, hThumb);
                    DeleteObject(hThumb);
                }

                // Footer
                SelectObject(hdc, m_hFontFooter);
                SetTextColor(hdc, Constants::Theme::ThemedGray(Constants::Theme::HelpWindow::SUBTITLE, app.themeFactor));

                // Copyright text
                RECT copyrightRect = {
                    rc.left + padding, rc.bottom - MulDiv(40, dpi, 96),
                    rc.right - padding - sbWidth, rc.bottom - MulDiv(20, dpi, 96)
                };
                std::wstring copyrightText = std::wstring(Constants::APP_CREATOR) + L" | " + Constants::APP_HELP_FOOTER;
                DrawTextW(hdc, copyrightText.c_str(), -1, &copyrightRect, DT_CENTER | DT_TOP);

                // Facebook link (clickable)
                m_footerLinkRect = {
                    rc.left + padding, rc.bottom - MulDiv(18, dpi, 96),
                    rc.right - padding - sbWidth, rc.bottom - MulDiv(2, dpi, 96)
                };
                SetTextColor(hdc, Constants::Theme::ThemedColor(
                        Constants::Theme::HelpWindow::LINK_R,
                        Constants::Theme::HelpWindow::LINK_G,
                        Constants::Theme::HelpWindow::LINK_B, app.themeFactor));
                DrawTextW(hdc, L"Follow on Facebook - Ivan Hristov Yanev", -1, &m_footerLinkRect, DT_CENTER | DT_TOP);

                EndPaint(m_hWnd, &ps);
                return 0;
            }

            case WM_MOUSEWHEEL: {
                int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                m_scrollOffsetY -= (delta / WHEEL_DELTA) * static_cast<int>(50 * app.dpiScale);
                InvalidateRect(m_hWnd, nullptr, FALSE);
                return 0;
            }

            case WM_LBUTTONDOWN: {
                POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

                // Check if clicking footer link
                if (pt.x >= m_footerLinkRect.left && pt.x <= m_footerLinkRect.right &&
                    pt.y >= m_footerLinkRect.top && pt.y <= m_footerLinkRect.bottom) {
                    ShellExecuteW(nullptr, L"open", L"https://www.facebook.com/IvanHristovYanev", nullptr, nullptr, SW_SHOW);
                    return 0;
                }

                // Scrollbar drag logic
                RECT rc;
                GetClientRect(m_hWnd, &rc);
                UINT dpi = static_cast<UINT>(app.dpiScale * 96.0f);
                int sbWidth = MulDiv(10, dpi, 96);
                int sbX = rc.right - sbWidth - MulDiv(5, dpi, 96);

                if (pt.x >= sbX && pt.x < sbX + sbWidth) {
                    m_sbDragging = true;
                    m_sbDragStartY = pt.y;
                    m_sbDragStartOffset = m_scrollOffsetY;
                }
                return 0;
            }

            case WM_MOUSEMOVE: {
                POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

                // Check if hovering over footer link
                if (pt.x >= m_footerLinkRect.left && pt.x <= m_footerLinkRect.right &&
                    pt.y >= m_footerLinkRect.top && pt.y <= m_footerLinkRect.bottom) {
                    SetCursor(LoadCursor(nullptr, IDC_HAND));
                } else {
                    SetCursor(LoadCursor(nullptr, IDC_ARROW));
                }

                // Scrollbar drag logic
                if (m_sbDragging) {
                    RECT rc;
                    GetClientRect(m_hWnd, &rc);
                    UINT dpi = static_cast<UINT>(app.dpiScale * 96.0f);
                    int padding = MulDiv(30, dpi, 96);
                    int titleFontSize = MulDiv(32, dpi, 96);
                    int subtitleFontSize = MulDiv(14, dpi, 96);
                    int contentTop = padding + titleFontSize + MulDiv(30, dpi, 96) + subtitleFontSize;
                    int contentHeight = rc.bottom - contentTop - padding;

                    int delta = pt.y - m_sbDragStartY;
                    int maxScroll = std::max(0, m_totalContentHeight - contentHeight);
                    m_scrollOffsetY = m_sbDragStartOffset + (delta * maxScroll) / contentHeight;
                    InvalidateRect(m_hWnd, nullptr, FALSE);
                }
                return 0;
            }

            case WM_LBUTTONUP: {
                m_sbDragging = false;
                return 0;
            }

            case WM_CLOSE: {
                Hide();
                return 0;
            }

        }

        return DefWindowProcW(m_hWnd, message, wParam, lParam);
    }
}
