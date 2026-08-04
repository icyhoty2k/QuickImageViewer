#include "HelpWnd.h"
#include "UI/GdiPool.h" // pooled brushes and pens — never DeleteObject them
#include "UI/CustomControls/ScrollView.h" // WheelDeltaToPixels — the one wheel rule
#include "../../Platform/Constants.h"
#include "../../AppState.h"
#include "Shortcuts.h"
#include <string>
#include <vector>
#include <dwmapi.h>
#include <shellapi.h>
#include <algorithm>
#include <shlobj.h>
#include <commctrl.h>
#include <fstream>
#include <windowsx.h>
#include <cwchar>
#include <cwctype>

namespace UI {
    // =========================================================================
    // Key-name helpers — every shortcut label is derived from the VK constants
    // in Shortcuts.h. Remap a key there and the help text follows automatically.
    // OEM keys are resolved through MapVirtualKeyW so they always show the
    // character of the ACTIVE keyboard layout.
    // =========================================================================
    namespace {
        std::wstring KeyName(UINT vk) {
            if (vk >= VK_F1 && vk <= VK_F24)
                return L"F" + std::to_wstring(vk - VK_F1 + 1);
            if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
                return L"Num " + std::to_wstring(vk - VK_NUMPAD0);

            switch (vk) {
                case VK_ESCAPE: return L"Esc";
                case VK_TAB: return L"Tab";
                case VK_RETURN: return L"Enter";
                case VK_SPACE: return L"Space";
                case VK_BACK: return L"Backspace";
                case VK_DELETE: return L"Delete";
                case VK_INSERT: return L"Insert";
                case VK_HOME: return L"Home";
                case VK_END: return L"End";
                case VK_PRIOR: return L"Page Up";
                case VK_NEXT: return L"Page Down";
                case VK_LEFT: return L"Left";
                case VK_RIGHT: return L"Right";
                case VK_UP: return L"Up";
                case VK_DOWN: return L"Down";
                case VK_ADD: return L"Num +";
                case VK_SUBTRACT: return L"Num −";
                case VK_MULTIPLY: return L"Num *";
                case VK_DIVIDE: return L"Num /";
                default: break;
            }

            // Letters, digits and OEM punctuation — ask the active layout
            UINT ch = MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR) & 0x7FFF;
            if (ch >= 32)
                return std::wstring(1, static_cast<wchar_t>(
                                        towupper(static_cast<wint_t>(ch))));
            return L"?";
        }

        std::wstring K(UINT vk) {
            return KeyName(vk);
        }

        std::wstring Ctrl(UINT vk) {
            return L"Ctrl+" + KeyName(vk);
        }

        std::wstring Shift(UINT vk) {
            return L"Shift+" + KeyName(vk);
        }

        std::wstring Alt(UINT vk) {
            return L"Alt+" + KeyName(vk);
        }

        std::wstring CtrlShift(UINT vk) {
            return L"Ctrl+Shift+" + KeyName(vk);
        }

        std::wstring CtrlAlt(UINT vk) {
            return L"Ctrl+Alt+" + KeyName(vk);
        }

        std::wstring CtrlAltShift(UINT vk) {
            return L"Ctrl+Alt+Shift+" + KeyName(vk);
        }

        std::wstring NumI(int v) {
            return std::to_wstring(v);
        }

        std::wstring NumF(float v) {
            wchar_t buf[32];
            swprintf(buf, 32, L"%g", static_cast<double>(v));
            return buf;
        }
    } // namespace

    // =========================================================================
    // Init
    // =========================================================================
    void HelpWnd::Init(HINSTANCE hInstance, HWND hParent, int8_t /*position*/) {
        HelpWnd::Init(hInstance, hParent);
    }

    void HelpWnd::Init(HINSTANCE hInstance, HWND hParent) {
        m_fullTitle = std::wstring(Constants::BASE_NAME) + L" v" + Constants::APP_VERSION;
        UINT dpi = static_cast<UINT>(app.dpiScale * 96.0f);
        InitFloating(hInstance, hParent, L"QIV_HelpWindow", Constants::APP_TASKBAR_NAME,
                     MulDiv(680, dpi, 96), MulDiv(840, dpi, 96));
        BuildHelpContent();

        m_filter.SetPlaceholder(L"type to filter shortcuts…");
        m_filter.OnChanged = [this](const std::wstring& text) {
            m_query = text;
            for (auto& c : m_query) c = static_cast<wchar_t>(towlower(c));
            RebuildFilter();
            m_view.scrollY = 0; // filtered content restarts at the top
            InvalidateRect(m_hWnd, nullptr, FALSE);
        };
    }

    // Recompute per-entry visibility + shortcut highlight positions from m_query.
    // Same matcher as Find/History: wildcard when the query has * or ?, else fuzzy
    // subsequence. An entry is visible if EITHER the shortcut or the description
    // matches; only the shortcut's match positions are stored (single-line).
    void HelpWnd::RebuildFilter() {
        m_entryMatch.assign(m_entries.size(), EntryMatch{});
        if (m_query.empty()) return; // all visible, no highlight

        const int  qLen = static_cast<int>(std::min(m_query.size(),
                                                    static_cast<size_t>(Common::FUZZY_MAX_QUERY)));
        const bool wild = Common::IsWildcardQuery(m_query.c_str(), qLen);

        auto matchField = [&](const std::wstring& s, Common::FuzzyMatchResult& out) -> bool {
            const int len = static_cast<int>(std::min(s.size(), static_cast<size_t>(1023)));
            wchar_t low[1024];
            Common::LowerCopy(s.c_str(), len, low);
            return wild ? Common::WildcardMatch(m_query.c_str(), low, &out)
                        : Common::FuzzyMatch(m_query.c_str(), qLen, low, len, out);
        };

        for (size_t i = 0; i < m_entries.size(); ++i) {
            Common::FuzzyMatchResult scM, deM;
            const bool sc = matchField(m_entries[i].shortcut,    scM);
            const bool de = matchField(m_entries[i].description, deM);
            EntryMatch& m = m_entryMatch[i];
            m.visible = sc || de;
            if (sc) {
                m.scCount = scM.posCount;
                for (int k = 0; k < scM.posCount; ++k) m.scPos[k] = scM.positions[k];
            }
        }
    }

    bool HelpWnd::EntryVisible(size_t i) const {
        return m_query.empty() || (i < m_entryMatch.size() && m_entryMatch[i].visible);
    }

    bool HelpWnd::SectionHasVisible(int sectionId) const {
        if (m_query.empty()) return true;
        for (size_t i = 0; i < m_entries.size(); ++i)
            if (m_entries[i].sectionId == sectionId && EntryVisible(i)) return true;
        return false;
    }

    // =========================================================================
    // BuildHelpContent — sections + entries, all wired to Shortcuts.h /
    // Constants.h. No hardcoded key names for anything that has a constant.
    // =========================================================================
    void HelpWnd::BuildHelpContent() {
        namespace SC = Shortcuts;
        namespace FX = Shortcuts::ImageEffects;

        m_sections.clear();
        m_entries.clear();
        m_measuredWidth = 0; // invalidate measurement cache
        m_totalContentHeight = 0;

        auto Sec = [this](const wchar_t *icon, const wchar_t *title, const wchar_t *subtitle) {
            m_sections.push_back({icon, title, subtitle});
            return static_cast<int>(m_sections.size()) - 1;
        };
        auto Add = [this](const std::wstring &shortcut, const std::wstring &desc, int section) {
            m_entries.push_back({shortcut, desc, section});
        };

        // ---------------------------------------------------------------
        const int sNav = Sec(Constants::ThemeIcons::ICON_SECTION_COMPASS, L"IMAGE NAVIGATION",
                             L"Moving between images and folders");

        Add(K(SC::SC_NAV_PREV) + L" / " + K(SC::SC_NAV_NEXT),
            L"Go to the previous / next image in the current folder. Neighbouring images "
            L"are pre-decoded into the VRAM cache, so switching is instant.", sNav);
        Add(K(SC::SC_NAV_NEXT_SPACE) + L" / " + Shift(SC::SC_NAV_NEXT_SPACE),
            L"Next / previous image. While a slideshow is running, " + K(SC::SC_SLIDESHOW_PAUSE_RESUME) +
            L" pauses and resumes the slideshow instead.", sNav);
        Add(L"Mouse Wheel",
            L"Previous / next image when scrolled over the main window. Direction can be "
            L"reversed with MOUSE_VERTICAL_REVERSE_SCROLL_DIRECTION in Constants.h.", sNav);
        Add(K(FX::SC_COLOR_SEPIA_OR_SC_NAV_FIRST_IMAGE) + L"  /  " + K(FX::SC_COLOR_SOLARIZE_OR_SC_NAV_LAST_IMAGE),
            L"Jump to the first / last image of the folder.", sNav);
        Add(K(SC::SC_NAV_TOGGLE_FIRST_LAST_IMAGE_IN_CURR_FOLDER),
            L"Smart jump between the first and the last image of the folder — goes to "
            L"whichever end is further away from the current position.", sNav);
        Add(Shift(SC::SC_NAV_TOGGLE_FIRST_LAST_IMAGE_IN_CURR_FOLDER),
            L"Return to the image you were viewing before the first / last jump.", sNav);
        Add(K(SC::SC_TOGGLE_LAST_IMAGE),
            L"Toggle between the current image and the previously viewed one — an "
            L"image-level Back button.", sNav);
        Add(K(SC::SC_TOGGLE_LAST_DIR),
            L"Toggle between the current folder and the previously opened folder.", sNav);
        Add(K(FX::SC_COLOR_OUTLINE_OR_NAV_PREV_HISTORY_FOLDER) + L"  /  " + K(FX::SC_COLOR_THRESHOLD_OR_NAV_NEXT_HISTORY_FOLDER),
            L"Walk the History panel's list one folder up / down, visiting only the "
            L"non-favorite entries. The list order is never changed — the centre overlay "
            L"reports the same row number the panel shows.", sNav);
        Add(K(FX::SC_COLOR_INVERT_OR_NAVIGATE_FAVS_PREV) + L"  /  " + K(FX::SC_COLOR_GRAYSCALE_OR_NAVIGATE_FAVS_NEXT),
            L"Same walk restricted to the favorites, so the two pairs of keys never "
            L"land on each other's entries.", sNav);
        Add(K(SC::SC_NAV_JUMP_TO_IMAGE) + L"  /  " + Ctrl(SC::SC_NAV_JUMP_TO_IMAGE_ALT),
            L"Open the Jump-to panel: type an image number and press Enter to go straight "
            L"to it. Typing @ inside the panel switches to Find mode.", sNav);
        Add(Ctrl(SC::SC_NAV_FIND),
            L"Open the Find panel — fuzzy filename search with wildcard support: "
            L"* matches any characters, ? matches exactly one (e.g. *.jpg or photo_??_2024.*). "
            L"Searches the current folder and the VRAM cache. Typing # inside switches to Jump-to.", sNav);
        Add(K(SC::SC_NAV_SHOW_IN_EXPLORER),
            L"Reveal the current file in Windows Explorer (opens its folder with the file "
            L"pre-selected).", sNav);
        Add(L"Horizontal Wheel",
            L"Cycle through the folders of your navigation history — one folder change per " +
            NumI(Constants::MOUSE_HSCROLL_FOLDER_TICKS) +
            L" wheel notches. The folder snapshot is refreshed with " + Ctrl(SC::SC_PANEL_HISTORY_TOGGLE) +
            L" in the History panel.", sNav);
        Add(K(SC::SC_PANEL_OPEN_FILE), L"Open-file dialog.", sNav);
        Add(L"Drag & Drop",
            L"Drop an image file or an entire folder onto the window to open it.", sNav);

        // ---------------------------------------------------------------
        const int sZoom = Sec(Constants::ThemeIcons::ICON_SECTION_MAGNIFIER, L"ZOOM, PAN & VIEW MODES",
                              L"How the image fits and moves inside the window");

        Add(L"Up / Down",
            L"Zoom in / out by ×" + NumF(Constants::ZoomPanel::ZOOM_STEP) + L" per step.", sZoom);
        Add(K(SC::SC_ZOOM_IN_NUMPAD) + L" / " + K(SC::SC_ZOOM_OUT_NUMPAD),
            L"Zoom in / out — same steps as Up / Down.", sZoom);
        Add(K(SC::SC_ZOOM_RESET),
            L"Reset zoom and pan back to the active view mode's default fit.", sZoom);
        Add(L"Ctrl+Wheel",
            L"Zoom in / out centered on the mouse cursor.", sZoom);
        Add(K(SC::SC_APP_HIDE_ALT) + L" / " + K(SC::SC_PAN_LEFT) + L" / " +
            K(FX::SC_COLOR_SAVE_TO_DISK) + L" / " + K(SC::SC_PAN_RIGHT),
            L"Pan the viewport when the image is larger than the window — " +
            NumI(Constants::KEYBOARD_PAN_STEP) + L" px per press (DPI-scaled).", sZoom);
        Add(K(SC::SC_VIEW_MODE_FIRST) + L" – " + K(SC::SC_VIEW_MODE_LAST),
            L"Select the view mode:  1 Fit to view (keeps aspect ratio)  •  "
            L"2 Fit to width (stretches)  •  3 Fit to height (stretches)  •  "
            L"4 Fill window (stretches)  •  5 Original size, 1:1 pixels (keeps aspect ratio).", sZoom);
        Add(K(SC::SC_VIEWPORT_LOCK),
            std::wstring(L"Toggle viewport lock. When ON, zoom and pan carry over to the next image "
                    L"instead of resetting — so flipping through same-framed shots keeps the same "
                    L"detail on screen at the same magnification. Rotation and flips still reset, "
                    L"because each file's EXIF orientation tag owns those. Startup default: ") +
            (Constants::IS_LOCK_VIEWPORT ? L"ON" : L"OFF") +
            L" (IS_LOCK_VIEWPORT in Constants.h).", sZoom);

        // ---------------------------------------------------------------
        const int sMouse = Sec(Constants::ThemeIcons::ICON_SECTION_MOUSE, L"MOUSE CONTROLS",
                               L"All pointer actions on the main window");

        Add(L"ℹ Button roles",
            L"The roles below assume SWAP_MOUSE_BUTTONS = true in Constants.h (the shipped "
            L"default). Set it to false to exchange the left and right button functions.", sMouse);
        Add(L"LMB hold / drag",
            L"Quick " + NumF(app.zoomClickMultiplier) + L"× zoom centered on the cursor; "
            L"dragging pans while zoomed. Zoom and pan revert the moment the button is released.", sMouse);
        Add(L"LMB double-click", L"Toggle fullscreen.", sMouse);
        Add(L"RMB drag", L"Move the window.", sMouse);
        Add(L"RMB + LMB", L"Reveal the current file in Windows Explorer.", sMouse);
        Add(L"RMB + Wheel", L"Zoom in / out while the right button is held.", sMouse);
        Add(L"RMB + Horizontal Wheel",
            L"Live-resize the window from its center — 20 px per wheel notch.", sMouse);
        Add(L"MMB click",
            L"Full visual reset: zoom, pan and opacity return to defaults, the window is "
            L"resized to its default dimensions and centered on the current monitor.", sMouse);
        Add(L"MMB drag", L"Live-resize the window.", sMouse);
        Add(L"Shift+Wheel",
            L"Adjust window opacity in " + NumI(Constants::OPACITY_STEP) + L"% steps.", sMouse);

        // ---------------------------------------------------------------
        const int sWin = Sec(Constants::ThemeIcons::ICON_SECTION_WINDOW, L"WINDOW MANAGEMENT",
                             L"Move, resize, snap, fullscreen and stacking");

        Add(L"Shift+" + K(SC::SC_APP_HIDE_ALT) + L" / " + K(SC::SC_PAN_LEFT) + L" / " +
            K(FX::SC_COLOR_SAVE_TO_DISK) + L" / " + K(SC::SC_PAN_RIGHT),
            L"Nudge the window " + NumI(Constants::KEYBOARD_WINDOW_MOVE_STEP) +
            L" px up / left / down / right per keypress (DPI-scaled).", sWin);
        Add(L"Alt+" + K(SC::SC_APP_HIDE_ALT) + L" / " + K(SC::SC_PAN_LEFT) + L" / " +
            K(FX::SC_COLOR_SAVE_TO_DISK) + L" / " + K(SC::SC_PAN_RIGHT),
            L"Snap the window to the top / left / bottom / right half of the work area.", sWin);
        Add(L"Alt+" + K(SC::SC_APP_HARD_QUIT) + L" / " + K(SC::SC_TOGGLE_LAST_IMAGE) + L" / " +
            K(SC::SC_SNAP_QUARTER_BOTTOM_LEFT) + L" / " + K(SC::SC_COPY_TO_CLIPBOARD),
            L"Snap the window to the top-left / top-right / bottom-left / bottom-right "
            L"quarter of the work area.", sWin);
        Add(Alt(SC::SC_WINDOW_RESET_DEFAULTS),
            L"Reset to defaults — window size, position and all effects "
            L"(same as " + Shift(SC::SC_APP_RESET_DEFAULTS) + L").", sWin);
        Add(L"Shift+Num + / −   •   Shift+" + K(FX::SC_COLOR_GAMMA_UP) + L" / " +
            K(FX::SC_COLOR_GAMMA_DOWN),
            L"Grow / shrink the window by " + NumI(Constants::KEYBOARD_WINDOW_RESIZE_STEP) +
            L" px per side while keeping it centered.", sWin);
        Add(L"Ctrl+Space",
            L"Toggle: first press fits the viewer to the work area (monitor minus taskbar), "
            L"shrinking further to avoid any visible panel (F3 cache strip, F6 directory strip, "
            L"spawned directory panels). Second press restores the default window size "
            L"(baseWidth × baseHeight from Constants.h), centered on the current monitor.", sWin);
        Add(Ctrl(SC::SC_MOVE_TO_NEXT_MONITOR),
            L"Move the window to the next monitor, wrapping round at the last. Monitors "
            L"are ordered left to right by their desktop coordinates. Position and size "
            L"are carried across proportionally, so a screen of a different resolution "
            L"does not leave the window half off the edge; in fullscreen the window "
            L"simply fills the new monitor.", sWin);
        Add(L"Drag near screen edge",
            L"Releasing a window drag within " + NumI(Constants::WINDOW_SNAP_DISTANCE) +
            L" px of a screen edge snaps the window to that edge.", sWin);
        Add(K(SC::SC_PANEL_FULLSCREEN_F) + L" / " +
            K(SC::SC_PANEL_FULLSCREEN_ENTER) + L" / " + CtrlShift(SC::SC_PANEL_FULLSCREEN_T),
            L"Toggle borderless fullscreen on the current monitor. "
            L"(F11 no longer does this — it is the mirroring toggle.)", sWin);
        Add(Ctrl(SC::SC_ALWAYS_ON_TOP) + L"  /  " + Ctrl(SC::SC_ALWAYS_ON_TOP_A),
            L"Toggle always-on-top — the main window and every visible panel stay above "
            L"all other windows.", sWin);

        // ---------------------------------------------------------------
        const int sPanels = Sec(Constants::ThemeIcons::ICON_SECTION_TOOLBOX, L"PANELS & TOOLS",
                                L"Help, info, statistics and the thumbnail panels");

        Add(K(SC::SC_PANEL_HELP_TOGGLE),
            L"Toggle this help window. Scroll with the wheel, Page Up / Page Down, Home and "
            L"End. " + Ctrl('E') + L" exports the full help to a text file on the Desktop.", sPanels);
        Add(K(SC::SC_SHOW_INFO),
            L"Toggle the Image Info panel — full EXIF metadata (camera, lens, exposure, GPS) "
            L"including the embedded preview thumbnail.", sPanels);
        Add(K(SC::SC_TOGGLE_STATS),
            L"Toggle the Statistics panel — decode time, codec used, file details and "
            L"cache information for the current image.", sPanels);
        Add(K(SC::SC_PANEL_CACHE_TOGGLE) + L" / " + K(SC::SC_PANEL_CACHE_MOVE),
            L"Toggle the VRAM cache strip / move it to the next free screen edge. It shows "
            L"every image currently decoded in GPU memory and updates live as you browse.", sPanels);
        Add(K(SC::SC_PANEL_DIR_TOGGLE) + L" / " + K(SC::SC_PANEL_DIR_MOVE) + L"  •  Right Shift",
            L"Toggle the current-directory strip / move it to the next free screen edge. "
            L"Right Shift is an alternative toggle key for " + K(SC::SC_PANEL_DIR_TOGGLE) + L".", sPanels);
        Add(K(SC::SC_APP_RELOAD_CURRENT_DIR),
            L"Reload the current directory from disk — picks up newly added, deleted or "
            L"renamed files without leaving the folder.", sPanels);
        Add(Ctrl(SC::SC_PANEL_CACHE_CLEAR),
            L"Clear the VRAM cache. Images are re-decoded on demand afterwards. "
            L"(Moved from F12, which is now the mirroring toggle.)", sPanels);
        Add(K(SC::IPANNEL_WINDOW_LOCAL_HIDE) + L"  /  " + Ctrl(SC::SC_APP_HIDE_ALT),
            L"Close the focused panel — works in every panel window.", sPanels);
        Add(L"MMB click",
            L"Middle-mouse-button click on any floating panel (Help, EXIF, Stats, Jump-to, Find) "
            L"or on any directory / cache strip closes that panel immediately.", sPanels);

        // ---------------------------------------------------------------
        const int sThumbs = Sec(Constants::ThemeIcons::ICON_SECTION_PICTURE, L"THUMBNAIL STRIPS",
                                L"Cache, directory and spawned directory panels");

        Add(L"Mouse Wheel",
            L"Scroll the strip. Hold Shift to scroll 3× faster.", sThumbs);
        Add(K(SC::SC_THUMBNAIL_WRAP_TOGGLE),
            std::wstring(L"Toggle wheel wrap-around. When ON, scrolling past the last thumbnail "
                    L"jumps back to the first — and scrolling before the first jumps to the last. "
                    L"Each wrap shows a message in the center overlay. Startup default: ") +
            (Constants::THUMBNAIL_PANEL_WHEEL_WRAP_AROUND ? L"ON" : L"OFF") +
            L" (THUMBNAIL_PANEL_WHEEL_WRAP_AROUND in Constants.h).", sThumbs);
        Add(K(SC::SC_THUMBNAIL_EFFECTS_TOGGLE),
            std::wstring(L"Toggle thumbnail strip visual effects on/off (master runtime switch). "
                    L"Effects include: rounded corners (corner overdraw), accent-color glow border on the "
                    L"selected thumbnail, and a subtle hover-scale enlarge on the hovered thumbnail. "
                    L"Each effect can also be individually disabled in Constants.h "
                    L"(ThumbnailPanel::ThumbnailEffects). Startup default: ") +
            (Constants::ThumbnailPanel::ThumbnailEffects::EFFECTS_MASTER_ENABLED ? L"ON" : L"OFF") +
            L".", sThumbs);
        Add(L"Left Click",
            L"Open the clicked thumbnail in the main viewer. Thumbnails from another folder "
            L"(e.g. in the cache strip) are opened as new files.", sThumbs);
        Add(L"Click + drag",
            L"Grab the strip and drag it to scroll freely.", sThumbs);
        Add(L"Scrollbar edge",
            L"Every strip has a thin scrollbar on its inner edge — click and drag it to "
            L"scrub through large folders quickly.", sThumbs);
        Add(L"Drag thumbnail → other strip",
            L"Drag a thumbnail from one directory strip onto another to MOVE the file "
            L"there. Hold Ctrl while dropping to COPY instead — the mouse cursor shows "
            L"which operation is active. Both strips refresh automatically and files can "
            L"be restored from the Recycle Bin.", sThumbs);
        Add(L"MMB click",
            L"Close the strip. Works on the " + K(SC::SC_PANEL_DIR_TOGGLE) +
            L" directory strip and every spawned directory panel.", sThumbs);
        Add(L"Click to make active",
            L"Clicking any directory strip makes it the active panel. All subsequent "
            L"folder navigation — including History Enter and folder changes — targets "
            L"the last-clicked strip. Only one strip is active at a time; the "
            L"primary " + K(SC::SC_PANEL_DIR_TOGGLE) + L" strip is the fallback when "
            L"no spawned panel has been clicked.", sThumbs);
        Add(L"Right Click",
            L"Context menu on any thumbnail: Copy, Cut, Delete and Paste. Copy / Cut use "
            L"the Windows clipboard, so files can also be pasted in Explorer. A cut file "
            L"is shown dimmed until it is pasted. Delete sends the file to the Recycle "
            L"Bin. Paste drops clipboard files into the strip's folder. Also: Select All, "
            L"Select Inverse (flips the current selection) and Select None.", sThumbs);

        // ---------------------------------------------------------------
        const int sHist = Sec(Constants::ThemeIcons::ICON_SECTION_SCROLL, L"HISTORY PANEL",
                              L"Recently visited folders with favorites");

        Add(K(SC::SC_PANEL_HISTORY_TOGGLE),
            L"Toggle the History panel — the list of recently opened folders. Navigate with "
            L"the mouse or arrow keys.", sHist);
        Add(Ctrl(SC::SC_PANEL_HISTORY_TOGGLE),
            L"Toggle the full (uncapped) history view and refresh the folder snapshot used "
            L"by horizontal-wheel navigation.", sHist);
        Add(K(SC::HISTORY_OPEN_IN_DIR_WND),
            L"Open the hovered folder in the main viewer.", sHist);
        Add(Shift(SC::HISTORY_OPEN_IN_DIR_WND),
            L"Spawn a floating directory strip for the hovered folder without leaving the "
            L"current one — up to " + NumI(Constants::DIR_WND_MAX_INSTANCES) +
            L" strips (top, left, right, bottom). If a strip is already open for that "
            L"folder, this hides it instead (toggle). Great for comparing folders.", sHist);
        Add(L"MMB click on a row",
            L"Toggle a floating directory strip for the hovered folder: spawns one if none "
            L"is open, or hides the existing one. The History panel itself stays open.", sHist);
        Add(K(SC::HISTORY_FAVORITES_TOGGLE_KEY),
            L"Toggle favorite on the hovered entry. Favorites survive history clears.", sHist);
        Add(Ctrl(VK_DELETE),
            L"Delete the hovered entry. " + Ctrl('Z') + L" restores the last deleted one.", sHist);
        Add(L"Home / End / Page Up / Page Down / Insert / Delete",
            L"Not captured by this panel — they reach the viewer as usual, so you can walk "
            L"images and folders while watching the list highlight follow you.", sHist);
        Add(CtrlShift(SC::HISTORY_CLEAR_ALL_HISTORY_BUT_NOT_FAVORITES),
            L"Clear the entire history but keep all favorites.", sHist);
        Add(CtrlAltShift(SC::HISTORY_CLEAR_ALL_FAVORITES_BUT_NOT_HISTORY),
            L"Clear all favorites but keep the history.", sHist);

        // ---------------------------------------------------------------
        const int sSlide = Sec(Constants::ThemeIcons::ICON_SECTION_PLAY, L"SLIDESHOW",
                               L"Automatic playback with transitions");

        Add(Ctrl(SC::SC_SLIDESHOW_TOGGLE), L"Start / stop the slideshow.", sSlide);
        Add(K(SC::SC_SLIDESHOW_PAUSE_RESUME) + L"  (while running)",
            L"Pause / resume the slideshow.", sSlide);
        Add(K(SC::SC_SLIDESHOW_LOOP_TOGGLE) + L"  (while running)",
            L"Toggle loop — restart from the first image after the last.", sSlide);
        Add(K(SC::SC_SLIDESHOW_SHUFFLE_TOGGLE) + L"  (while running)",
            L"Toggle shuffle — show the playlist in random order.", sSlide);
        Add(K(SC::SC_SLIDESHOW_TRANSITION_CYCLE) + L"  (while running)",
            L"Step to the next slide transition (" + NumI(Constants::Slideshow::TRANSITION_COUNT) +
            L" in total, wraps around). Slideshow › Transition in the tray or right-click "
            L"menu controls this in two parts: which transitions are in play — None "
            L"(just the one you tick), All, or List (only the ones you tick) — and the "
            L"order they play in, Sequential (the menu's numbered order) or Random. "
            L"In List mode the numbered rows become checkboxes.", sSlide);

        // ---------------------------------------------------------------
        const int sOverlay = Sec(Constants::ThemeIcons::ICON_SECTION_INFO, L"INFO OVERLAYS",
                                 L"The 3×3 on-screen information grid");

        Add(K(SC::SC_PANEL_OVERLAY_MASTER) + L" / " + Ctrl(SC::SC_PANEL_OVERLAY_MASTER_CTRL0),
            L"Master toggle — show / hide all overlay slots at once.", sOverlay);
        Add(Ctrl(SC::SC_OVERLAY_SLOT_1) + L" – " + Ctrl(SC::SC_OVERLAY_SLOT_9),
            L"Cycle a slot of the 3×3 grid: Compact (one line) → Full (two lines) → Off. "
            L"Assignments:  1 index + filename  •  3 zoom %  •  5 center message area  •  "
            L"7 active effects + folder name  •  9 dimensions + file size. "
            L"Slots 2, 4, 6, 8 show the thumbnail-strip selection count for the panel on "
            L"that edge. Slot 5 has no compact form, so it cycles On → Off.", sOverlay);
        Add(K(SC::SC_OVERLAY_LAYOUT_CYCLE),
            L"Cycle the overlay layout: Grid → Stacked → Summary.", sOverlay);
        Add(K(SC::SC_OVERLAY_BG_TOGGLE),
            L"Toggle the semi-transparent background behind overlay text — the text itself "
            L"always stays visible.", sOverlay);

        // ---------------------------------------------------------------
        const int sFx = Sec(Constants::ThemeIcons::ICON_SECTION_PALETTE, L"EFFECTS & COLOR",
                            L"Effects stack in the order you switch them on — each one works on "
                            L"the result of the previous. Non-destructive: the file on disk is "
                            L"never touched.");

        Add(K(SC::SC_TRANSFORM_ROTATE) + L" / " + Shift(SC::SC_TRANSFORM_ROTATE),
            L"Rotate 90° clockwise / counter-clockwise.", sFx);
        Add(K(SC::SC_TRANSFORM_FLIP_H) + L" / " + K(SC::SC_TRANSFORM_FLIP_V),
            L"Flip horizontally / vertically.", sFx);
        Add(Ctrl(FX::SC_COLOR_GRAYSCALE_OR_NAVIGATE_FAVS_NEXT),
            L"Toggle desaturate — drops all colour. Switch Sepia on afterwards to tint "
            L"the result.", sFx);
        Add(Ctrl(FX::SC_COLOR_INVERT_OR_NAVIGATE_FAVS_PREV), L"Toggle color inversion (negative).", sFx);
        Add(Ctrl(FX::SC_COLOR_SEPIA_OR_SC_NAV_FIRST_IMAGE), L"Toggle sepia tone.", sFx);
        Add(Ctrl(FX::SC_COLOR_SOLARIZE_OR_SC_NAV_LAST_IMAGE),
            L"Toggle solarize — inverts only tones above " +
            NumF(Constants::SOLARIZE_THRESHOLD * 100.0f) + L"% brightness.", sFx);
        Add(Ctrl(FX::SC_COLOR_OUTLINE_OR_NAV_PREV_HISTORY_FOLDER), L"Toggle outline — GPU edge detection.", sFx);
        Add(Ctrl(FX::SC_COLOR_THRESHOLD_OR_NAV_NEXT_HISTORY_FOLDER),
            L"Toggle black & white threshold — pixels brighter than " +
            NumF(Constants::BW_THRESHOLD_LEVEL * 100.0f) + L"% become white, the "
            L"rest black.", sFx);
        Add(Ctrl(FX::SC_COLOR_SAT_DOWN) + L" / " + Ctrl(FX::SC_COLOR_SAT_UP),
            L"Saturation − / + in " + NumF(Constants::COLOR_ADJUST_STEP) +
            L" steps  (0 = grayscale, maximum " + NumF(Constants::MIN_MAX_SATURATION) + L").", sFx);
        Add(Ctrl(FX::SC_COLOR_BRIGHTNESS_UP) + L" / " + Ctrl(FX::SC_COLOR_BRIGHTNESS_DOWN),
            L"Brightness + / − in " + NumF(Constants::COLOR_ADJUST_STEP) +
            L" steps  (range ±" + NumF(Constants::MIN_MAX_BRIGHTNESS) + L").", sFx);
        Add(Ctrl(FX::SC_COLOR_CONTRAST_UP) + L" / " + Ctrl(FX::SC_COLOR_CONTRAST_DOWN),
            L"Contrast + / − in " + NumF(Constants::COLOR_ADJUST_STEP) +
            L" steps  (maximum " + NumF(Constants::MIN_MAX_CONTRAST) + L").", sFx);
        Add(Ctrl(FX::SC_COLOR_GAMMA_UP) + L" / " + Ctrl(FX::SC_COLOR_GAMMA_DOWN),
            L"Gamma + / − in " + NumF(Constants::GAMMA_STEP) + L" steps  (range " +
            NumF(Constants::MIN_GAMMA) + L" – " + NumF(Constants::MAX_GAMMA) + L").", sFx);
        Add(K(FX::SC_COLOR_RESET_ALL_EFFECTS),
            L"Reset every color adjustment and effect back to neutral.", sFx);
        Add(K(FX::SC_EFFECT_APPLY_TOGGLE),
            L"Effects bypass — temporarily view the untouched original; press again to "
            L"re-apply all active effects.", sFx);

        // ---------------------------------------------------------------
        const int sFiles = Sec(Constants::ThemeIcons::ICON_SECTION_FLOPPY, L"FILES, CLIPBOARD & SORTING",
                               L"Saving, copying and playlist ordering");

        Add(Ctrl(SC::SC_COPY_TO_CLIPBOARD),
            L"Copy the current image to the clipboard.", sFiles);
        Add(Ctrl(FX::SC_COLOR_SAVE_TO_DISK),
            L"Save As — choose the format in the dialog (PNG, JPEG, BMP, TIFF, GIF). "
            L"Rotation, flips and all color effects are baked into the saved file.", sFiles);
        Add(CtrlAltShift(SC::SC_SORT_BY_NAME),
            L"Sort by name (natural Explorer order). Press again to reverse A→Z / Z→A.", sFiles);
        Add(CtrlAltShift(SC::SC_SORT_BY_DATE),
            L"Sort by date modified. Press again to switch newest ↔ oldest.", sFiles);
        Add(CtrlAltShift(SC::SC_SORT_BY_SIZE),
            L"Sort by file size. Press again to switch largest ↔ smallest.", sFiles);
        Add(CtrlAltShift(SC::SC_SORT_BY_TYPE),
            L"Sort by extension. Press again to reverse A→Z / Z→A.", sFiles);
        Add(CtrlAltShift(SC::SC_SORT_BY_DISK),
            L"Sort by physical disk order — reads files in on-disk sequence, the fastest "
            L"option for mechanical hard drives.", sFiles);

        // ---------------------------------------------------------------
        const int sApp = Sec(Constants::ThemeIcons::ICON_SECTION_GEAR, L"APPLICATION & APPEARANCE",
                             L"Lifecycle, theme and window chrome");

        Add(K(SC::SC_APP_HIDE) + L"  /  " + Ctrl(SC::SC_APP_HIDE_ALT),
            L"Hide to the system tray — the process stays resident so the next open is "
            L"instant. Extra running instances are closed.", sApp);
        Add(Ctrl(SC::SC_APP_HARD_QUIT),
            L"Hard quit — fully removes the process from memory.", sApp);
        Add(Ctrl(SC::SC_APP_NEW_WINDOW), L"Open a new independent QIV window.", sApp);
        Add(K(SC::SC_PANEL_DEDICATED_TOGGLE),
            L"Open the Dedicated panel: configure a named instance — its own images and "
            L"promotions folders, promotion pacing, monitor and slideshow options — then "
            L"generate a shortcut for it. Drop that shortcut in shell:startup and the "
            L"screen starts itself.", sApp);
        Add(K(SC::SC_PANEL_REMOTE_TOGGLE),
            L"Open the Local Server panel — the listener THIS instance runs, and nothing "
            L"else. qIV can listen on a TCP port and be driven over the network — next, "
            L"goto, zoom, open, slideshow — from a script, a phone, a home-automation "
            L"system, or another qIV instance.\r\n"
            L"The listener is OFF unless you switch it on here, in the instance's .ini, or "
            L"with -remote on the command line. It also needs a NAME and a PORT: an empty "
            L"AllowList denies every connection, and a driving instance identifies this one "
            L"by its name, so it will not start without one.\r\n"
            L"THE PROTOCOL DESCRIBES ITSELF. Send `help` and the answer lists every "
            L"command this build accepts, one per line, with whether it takes a value, "
            L"what it does and what the value means — pipe-separated, so a client can "
            L"build its own command list from that one call instead of carrying a copy "
            L"that goes stale. `ping` checks liveness, `version` reports the app and "
            L"protocol version. It is plain UTF-8 text: netcat, curl, telnet and a "
            L"five-line script are all first-class clients.\r\n"
            L"ENCRYPTION IS DECIDED BY THE ADDRESS, never negotiated. A peer on this "
            L"machine (127.x, ::1, localhost) gets plaintext; EVERY other peer, your own "
            L"LAN included, gets TLS 1.2/1.3 from the first byte. One listener on 0.0.0.0 "
            L"serves both. The certificate is self-signed and the client PINS it by the "
            L"SHA-256 fingerprint shown at the bottom of this panel — read it off here and "
            L"type it into the client. There is no plaintext fallback for anything to "
            L"strip.\r\n"
            L"A PASSWORD IS COMPULSORY off loopback and the server refuses to start "
            L"without one. It never crosses the wire: this end stores a PBKDF2-HMAC-SHA256 "
            L"digest and the client answers an HMAC over a fresh nonce, so each guess "
            L"costs the guesser a full derivation. Five failures from one address inside "
            L"ten minutes blacklists it automatically, every failure is answered a second "
            L"late, and a peer that connects and then says nothing is dropped after ten "
            L"seconds.\r\n"
            L"ALLOWLIST AND BLACKLIST take addresses, never domain names — a rule is "
            L"matched against the address a connection actually arrived from. Five forms: "
            L"* for everything, 192.168.1.* as a TEXT prefix, 192.168.0.0/24 as CIDR "
            L"(2001:db8::/32 works too), 192.168.0.10-50 as a range (or the long "
            L"192.168.0.10-192.168.1.5), and a bare address for one host. Mind the star: "
            L"it compares CHARACTERS, so 192.168.1* without the dot also covers "
            L"192.168.10.x — use /24 when the boundary matters. Anything that cannot match "
            L"an address is dropped when you save, and the panel names what it "
            L"dropped.\r\n"
            L"What this instance connects OUT to lives in Remote Servers (F10). "
            L"F9 is what others connect to; F10 is what this connects to.\r\n"
            L"WHO is connected right now is Ctrl+F9, not here.", sApp);
        Add(Ctrl(SC::SC_PANEL_REMOTE_TOGGLE),
            L"Server Clients — every peer currently connected to the listener above, with "
            L"its address, the name it gave itself, whether it is encrypted, and how long "
            L"it has been on. The list refreshes itself while the panel is open.\r\n"
            L"THREE WAYS TO GET RID OF ONE, and the middle one is the useful one:\r\n"
            L"KICK closes the connection. Against a person that is enough; against anything "
            L"automated it is useless, because it reconnects inside a second.\r\n"
            L"KICK FOR N closes it AND refuses that peer for the number of minutes you "
            L"type. This is what answers a bot: it outlasts a retry loop and then forgets "
            L"by itself. The block is held in memory only — restarting qIV clears every "
            L"one of them, which is also how you undo shutting yourself out.\r\n"
            L"BAN writes the address to qivRemoteServerBlacklist.ini and it survives "
            L"restarts. Undo it by editing or deleting that file.\r\n"
            L"For an IPv6 peer all three act on its /64 rather than the single address, "
            L"and the panel says so before it does it — blocking one IPv6 address is "
            L"close to useless, since the peer has billions of others in the same prefix.\r\n"
            L"None of this is reachable over the wire: a connected client cannot kick or "
            L"ban anything, including you.", sApp);
        Add(K(SC::SC_PANEL_REMOTES_CONSOLE),
            L"Open Remote Servers — everything about the instances this copy can drive.\r\n"
            L"Top: address, port, password, name and optional exe. Connect && Save proves the "
            L"target answers before recording it, then writes it to qivRemoteServers.ini beside the "
            L"exe and starts driving it — so an address is typed once.\r\n"
            L"Below: one row per instance. The dot starts a stopped instance or shuts down "
            L"a running one; Connect/Disconnect brings the link up or down (a remote can be "
            L"listed without being dialled).\r\n"
            L"WATCHING an instance moved to Ctrl+F11, along with the rest of the "
            L"who-drives-whom controls. This console is about which instances exist and how "
            L"to reach them.\r\n"
            L"IDENTIFY makes that screen say its own name in the middle of its window. "
            L"Two identical viewers side by side are otherwise anonymous, and this tells "
            L"you which row drives which without walking over to them. Available on "
            L"connected rows only. On the wire it is `msg <text>`, so a script can put any "
            L"message on any instance's screen.\r\n"
            L"The form at the top is READ-ONLY until you say otherwise. Clicking a row "
            L"loads it there so you can read it; pressing UPDATE unlocks the fields, and "
            L"SAVE or CANCEL locks them again. New clears the form and unlocks it for a "
            L"fresh remote. Without the lock a stray click on an address plus one keystroke "
            L"could quietly repoint a working remote at nothing. Esc backs out one step at "
            L"a time: out of a field, then out of edit mode, then closes the panel.\r\n"
            L"Connect all / Disconnect all act on every row, F5 polls them, and Sync all "
            L"pushes this viewer's view and effect state to all of them at once.", sApp);
        Add(L"CTRL+F10",
            L"Remote Commands — type a command and send it to the instances YOU TICK in this "
            L"panel's own SEND TO box, which lists everything currently connected with a "
            L"checkbox, name and address each. Its own selection on purpose: the Ctrl+F11 "
            L"Control ticks decide where keystrokes go, and lining up one screen by hand "
            L"should not change what F11 drives afterwards. Everything connected starts "
            L"ticked; All / None sit on the box's header. The manual counterpart to "
            L"mirroring: F11 "
            L"forwards what you DO, this sends what you TYPE, including the commands that "
            L"have no key at all — msgRemote, OpenFile, JumpToImage, "
            L"SlideshowSetInterval, EnableRemoteLog.\r\n"
            L"Every command has ONE wire name, and it is spelled exactly like its "
            L"Command.h enumerator — no short aliases to remember or confuse.\r\n"
            L"BROWSE FIRST: the full command list is permanently on the left, scrollable "
            L"and filterable — nobody remembers ninety wire names. The list shows NAMES "
            L"only, in two blues: the second one marks a command that TAKES A VALUE "
            L"(the header says so). Highlighting one "
            L"explains it along the TOP: what the command does over the list, what its "
            L"value means over the Value box, on one line — with units, limits and an "
            L"example where there is one to give. The list comes "
            L"from the same table the wire parser accepts, so a name shown here cannot "
            L"come back \"unknown command\".\r\n"
            L"Type to filter (it matches the descriptions too). ARROWS and PgUp/PgDn move "
            L"the highlight wherever the caret is. TAB swaps Filter and Value. ENTER in "
            L"the filter takes the highlighted command — straight to Value if it needs "
            L"one, otherwise it sends. ENTER in Value sends. ESC clears, then closes. "
            L"Clicking a command selects it; it never sends on its own.\r\n"
            L"THE BOX ALONG THE BOTTOM IS A LOG of the whole session: every line you "
            L"sent and every answer, numbered — → for a send, ← for a reply, with the "
            L"instance that answered and its round trip. NEWEST AT THE TOP, so what "
            L"just came back is there without scrolling — the opposite of the Ctrl+F12 "
            L"wire log, which is a transcript you read forwards. It scrolls, and it "
            L"holds your place if you have scrolled into the history.\r\n"
            L"It keeps growing while you work and survives closing the panel; only "
            L"Clear empties it, which also restarts the numbering at 1.\r\n"
            L"Each instance answers separately. Tick ALSO RUN IT HERE to include this "
            L"viewer — off by "
            L"default, since the usual reason to type a command here is to do something to "
            L"the other screens. The LOG button beside Clear jumps to the Ctrl+F12 wire "
            L"log, which carries the matching button back.", sApp);
        Add(K(SC::SC_MIRROR_TOGGLE),
            L"MIRRORING on/off. While on, every command you give here is also sent to the "
            L"instances listed in Remote Servers (F10) — navigation, zoom, effects, "
            L"view mode, sort order, slideshow. Panels, window geometry, quit and hide are "
            L"deliberately never forwarded: one keypress must not close or move every "
            L"screen at once. Always OFF at startup.\r\n"
            L"Both switches are also in the right-click menu under REMOTEACTIVATION, as "
            L"checkboxes — which is the only way to READ them without changing them, since "
            L"the overlay they announce themselves on fades. That submenu also says how "
            L"many instances are connected and how many are actually being driven.\r\n"
            L"F11 asks nothing and puts nothing up — it is only the toggle. Which of the "
            L"connected instances it drives is set in the Ctrl+F11 panel, and shown on "
            L"screen each time mirroring goes on. Everything connected is driven to begin "
            L"with; if you have unticked all of them, F11 says so instead of switching on "
            L"and forwarding to nobody.\r\n"
            L"CTRL+F11 opens Remotes Control: one row per CONNECTED instance, with three "
            L"controls each, in the order you ask them — IDENTIFY makes that screen say "
            L"its own name so you can tell two identical viewers apart; the CONTROL tick "
            L"says whether F11 drives it; and WATCH is the other direction, where that "
            L"instance reports what it does and this viewer follows along, which is how "
            L"you see a screen running its own slideshow (one at a time — it is a radio "
            L"button). Plus All, None, Sync now, and the two mirroring switches "
            L"themselves as toggle buttons — F11 (send anything at all) and F12 (also run "
            L"it here) — which light up while they are on. It is a real "
            L"window — leave it open beside the pictures while you work; it refreshes itself "
            L"as instances connect and drop, and every tick takes effect immediately. Sync "
            L"now pushes this viewer's folder, current image and view state to the ticked "
            L"instances, the same push as Sync all in Remote Servers. Rows that are "
            L"listed but not connected do not appear — connect them in F10 first. The "
            L"selection lasts for this session only and is echoed in the Remote Servers "
            L"header.", sApp);
        Add(Ctrl(SC::SC_REMOTE_SEND_POSITION),
            L"SEND THIS POSITION to the instances under Control (the ticked rows in "
            L"Ctrl+F11). Prepare a picture here, press it, and those screens show that "
            L"picture — the point being that they carry on doing whatever they were "
            L"doing. A target running a fullscreen slideshow keeps running it, in its own "
            L"view mode with its own effects, and simply continues from the image you "
            L"sent. Folder, sort order and position are the ONLY things sent; that is why "
            L"this is not Sync, which stamps this viewer's whole look onto the far end.\r\n"
            L"It works out what to send by ASKING first. Already in the same folder in the "
            L"same order? Then just the position — one line, no rescan, no flicker, so you "
            L"can walk a folder here and push each picture as you reach it. In a different "
            L"folder? Then the folder, then the sort order, then — after waiting for that "
            L"instance's own scan to finish — the image. Sending the position before the "
            L"scan lands is the race this avoids.\r\n"
            L"Same-machine instances only: a folder path and an image number are both "
            L"meaningless against another machine's files, and ticked rows elsewhere are "
            L"reported as skipped rather than silently left out. Independent of F11 — "
            L"mirroring can be off. Plain Enter is still fullscreen.", sApp);
        Add(Alt(SC::SC_REMOTE_STREAM_IMAGE_OUT),
            L"STREAM THIS IMAGE to the instances under Control, shown ONCE — and change "
            L"nothing else about them. No folder, no sort order, no playlist position: the "
            L"picture goes up in place of the slide currently showing, and afterwards that "
            L"instance carries on from exactly where it was, as if nothing had happened. "
            L"An advert dropped into a running slideshow is the case it was built for.\r\n"
            L"THE PICTURE'S OWN BYTES TRAVEL, base64-encoded across several protocol "
            L"lines, and the far end decodes them with its own decoder. So this works to "
            L"an instance on ANOTHER MACHINE, which Ctrl+Enter cannot — a folder path and "
            L"an image number are only meaningful against the far end's own disk. The file "
            L"name rides along for one reason: its extension is what picks the decoder.\r\n"
            L"With a slideshow running it appears at the NEXT slide boundary, so the "
            L"picture already on screen is not cut short, and it occupies one slide. With "
            L"no slideshow it appears at once and stays until something else changes the "
            L"image there. Either way it is gone the moment the picture changes — it never "
            L"enters that instance's playlist, and both the received file and its cache "
            L"entry are thrown away rather than left sitting in memory.\r\n"
            L"CTRL+Enter is the other depth of the same act: that one MOVES the far end to "
            L"your picture and it stays there. Plain Enter is still fullscreen.", sApp);
        Add(CtrlAlt(SC::SC_REMOTE_STREAM_IMAGE_IN),
            L"ASK FOR THE IMAGE another instance is displaying, and show it here — once. "
            L"The same transfer as Alt+Enter, inbound: that instance reads whatever is on "
            L"its screen and sends the bytes, so this works from a machine in another room "
            L"just as well as from one beside you.\r\n"
            L"ONE instance answers, because the answer is a picture and this screen shows "
            L"one at a time: the instance being WATCHED (Ctrl+F11's ◉) if there is one — "
            L"you are already following that screen — otherwise the first connected row "
            L"under Control. The overlay names which one was asked, so a narrowed "
            L"selection is never ambiguous.\r\n"
            L"What comes back is shown the way any one-shot image is: it never enters this "
            L"viewer's playlist, nothing about the folder, sort order or position moves, "
            L"and it is gone the moment the picture changes — but unlike an arriving "
            L"advert it appears IMMEDIATELY, without waiting for a slide boundary, because "
            L"you asked for it at this keyboard.\r\n"
            L"All three of these — and the wire commands behind them — are in the Ctrl+F10 "
            L"Remote Commands list, which is where to test them by hand.", sApp);
        Add(CtrlShift(SC::SC_REMOTE_PUSH_IMAGE_ALL),
            L"The Ctrl+Enter push, WIDENED: send this folder, sort order and position to "
            L"EVERY connected instance rather than only the ones ticked under Control "
            L"(Ctrl+F11).\r\n"
            L"For lining up a whole wall at once without first going to a panel to tick "
            L"rows you are about to untick again. Still same-machine only, for the same "
            L"reason Ctrl+Enter is — a folder path and an image number are read against "
            L"the far end's own disk, and choosing more targets cannot change that. "
            L"Instances on other machines are skipped and counted, and the overlay says "
            L"how many.", sApp);
        Add(L"CTRL+F12",
            L"Server Log — every line that crossed the wire, both directions, in one list: "
            L"number, who sent it, the command, who received it, the reply, the time, and "
            L"how long it took. A mirroring session is a conversation, so both directions "
            L"share one table in the order things actually happened.\r\n"
            L"CONNECTIONS ARE LOGGED TOO, as pairs, and the Sender column names WHO — the "
            L"peer's address with its source port, 192.168.0.7:51423 or "
            L"[2001:db8::1]:51423. The port is what tells two connections apart: one phone "
            L"reconnecting every few minutes, or two instances on one box, are otherwise "
            L"identical in every column, and an arrival could not be paired with its own "
            L"departure. Command rows name the peer the same way, so a line and the "
            L"connection that carried it match up. The departure also fills the Δ column "
            L"with HOW LONG the session lasted.\r\n"
            L"An arrival says whether it is plaintext "
            L"or TLS; the matching departure says WHY it ended — closed by peer, send "
            L"failed, authentication failed, server shutting down, or a TLS handshake that "
            L"never completed. A connection still open is an arrival with nothing under it "
            L"yet, so the list answers who is on this machine right now. Connections that "
            L"never became clients are here as well — refused for being blacklisted, not "
            L"on the AllowList, or over the connection limit — and on a listener anyone can "
            L"reach, those are the lines worth reading first.\r\n"
            L"RECORDING IS OFF by default and costs nothing while it is off. The Recording "
            L"button starts it — and tells every connected instance to start too, because "
            L"a log of one end of a conversation answers half the question. Opening this "
            L"panel does not start recording.\r\n"
            L"Two live counts sit at the right of the button row: CONNECTIONS n/m — how "
            L"many listed targets are actually connected — and CONTROL n/m — how many of "
            L"those F11 actually drives. They are TOGGLE buttons for the panels behind "
            L"those numbers: pressing one opens the F10 console or the Ctrl+F11 panel and "
            L"brings it to the front, pressing it again closes it. They light up while "
            L"that panel is open; dimmed digits mean the count is zero.\r\n"
            L"Clear empties it (entry numbers carry on, so a number never means two "
            L"different exchanges). Save writes a tab-separated UTF-8 file; Load reads one "
            L"back, REPLACING what is in memory. Click #, Time or Δ time to sort, and again to "
            L"reverse — the text columns do not sort, because grouping a conversation by "
            L"sender destroys the ordering that makes it one. The newest 20000 entries are "
            L"kept; older ones are dropped.\r\n"
            L"The list follows the newest entry until you scroll up, and follows again when "
            L"you scroll back to the bottom.\r\n"
            L"DOUBLE-CLICK A ROW (or select it and press Enter) to open it in its own "
            L"window with every field on its own line, wrapped and untrimmed — the table "
            L"has to ellipsise a long command or reply, and that window is where the full "
            L"text lives. Previous / Next step through the list in whatever order it is "
            L"currently sorted, and the list's selection follows along, so closing the "
            L"window leaves you where you finished reading. Copy puts the whole entry on "
            L"the clipboard.", sApp);
        Add(L"While connected",
            L"A connection — driving another instance, or being driven by one — puts the "
            L"viewer into a restricted mode, and leaves it when the last connection drops.\r\n"
            L"Refused while connected: sort by Disk Order (physical order differs per drive, "
            L"so two instances would order their playlists differently), Find, and anything "
            L"that changes the folder — delete, cut, paste, drag-drop and Ctrl+S. Adding or "
            L"removing one file shifts every image number after it, which is exactly what "
            L"the two instances rely on to stay on the same picture. Each refusal says so on "
            L"screen. Copy to clipboard still works: it reads a file and writes nothing.\r\n"
            L"Connecting while already sorted by disk order switches you to sort by Name.\r\n"
            L"Delete, move, paste and save are additionally NEVER reachable over the network, "
            L"whatever the password or allow-list says.", sApp);
        Add(L"Driving another machine",
            L"An instance on the SAME computer is mirrored exactly — it shares the folder, so "
            L"image numbers mean the same picture on both. An instance on ANOTHER computer "
            L"gets the portable commands only: navigation, zoom, effects, view mode and "
            L"slideshow, applied to its own content. Image numbers and folder paths are not "
            L"sent there, because a path from this machine names nothing on that one. The "
            L"Remote Servers marks those rows (remote), and its start button cannot launch "
            L"them — nothing here can start a process on another machine.\r\n"
            L"THE EXCEPTION IS A STREAMED IMAGE. Alt+Enter and Ctrl+Alt+Enter send the "
            L"picture's own bytes rather than a path, so they work to any machine: the far "
            L"end decodes what it receives, and nothing about the transfer depends on it "
            L"being able to read your disk. Both ends must be recent enough to know the "
            L"streaming commands (remote protocol v2) — an older one is refused with a "
            L"message that says so rather than being left to fail.", sApp);
        Add(K(SC::SC_MIRROR_LOCAL_TOGGLE),
            L"While mirroring, also execute the command HERE. Off (the default) makes this "
            L"viewer a pure remote control: it drives the other screens and its own display "
            L"stays where it is. Has no effect while mirroring is off. Always OFF at startup.", sApp);
        Add(K(SC::SC_TOGGLE_ALL_PANELS),
            L"Toggle all panels: closes every floating panel and directory strip if any is "
            L"open, otherwise restores exactly the set the last close hid. Main viewer stays.", sApp);
        Add(Shift(SC::SC_APP_RESET_DEFAULTS),
            L"Reset everything — window layout and all effects return to defaults.", sApp);
        Add(L"Ctrl+Alt+" + K(SC::SC_THEME_FACTOR_UP) + L" / " + K(SC::SC_THEME_FACTOR_DOWN),
            L"Theme brightness: step all panel colors lighter / darker at runtime.", sApp);
        Add(CtrlAlt(SC::SC_THEME_FACTOR_RESET),
            L"Reset theme brightness to the compiled default.", sApp);
        Add(CtrlShift(SC::SC_CORNER_PREFERENCE_TOGGLE),
            L"Toggle window corners: rounded ↔ square.", sApp);
        Add(CtrlShift(SC::SC_BACKDROP_TYPE_CYCLE),
            L"Cycle the window backdrop material: None → Mica → Acrylic → MicaAlt.", sApp);

        // ---------------------------------------------------------------
        const int sTray = Sec(Constants::ThemeIcons::ICON_SECTION_BELL, L"SYSTEM TRAY",
                              L"Right-click the tray icon to access all persistent settings");

        Add(L"Double-click tray icon",
            L"Restore the main window to the foreground.", sTray);
        Add(L"Restore QuickImageViewer",
            L"Show and bring the main window to the front.", sTray);
        Add(L"Help / Shortcuts",
            L"Open this help window directly from the tray.", sTray);
        Add(L"Exit Completely",
            L"Remove the tray icon, destroy the window and fully exit the process.", sTray);
        Add(L"Settings › Keep in Background",
            L"When enabled, closing the window (Esc / Ctrl+W) hides QIV to the tray "
            L"instead of exiting — the process stays resident for an instant re-open. "
            L"When disabled (or if more than one instance is running), Esc / Ctrl+W "
            L"closes the window entirely.", sTray);
        Add(L"Settings › Run on Startup",
            L"Write / remove the Windows startup registry entry (HKCU\\…\\Run) so QIV "
            L"launches automatically with Windows. Dedicated instances write their own "
            L"separate entry and do not conflict with the normal instance.", sTray);
        Add(L"Settings › Thumbnail Effects",
            L"Master switch for thumbnail strip visual effects: glow border on the "
            L"selected thumbnail, rounded-corner overdraw, and hover-scale enlarge.", sTray);
        Add(L"Settings › History: Open Full List",
            L"When enabled, Tab opens the full (uncapped) history view. When disabled, "
            L"the list is capped at the History Max Dirs value.", sTray);
        Add(L"Settings › Info Overlays",
            L"Show / hide all nine overlay text slots at once.", sTray);
        Add(L"Settings › Open Thumbnail Strip on Start",
            L"Automatically open the directory thumbnail strip on every launch.", sTray);
        Add(L"Settings › Overlay Background",
            L"Toggle the semi-transparent background panel behind overlay text.", sTray);
        Add(L"Settings › Swap Mouse Buttons",
            L"Exchange left and right button roles: hold-to-zoom ↔ drag-to-move-window.", sTray);
        Add(L"Settings › Invert Scroll Direction",
            L"Reverse vertical mouse wheel for image navigation.", sTray);
        Add(L"Settings › Invert Horizontal Scroll",
            L"Reverse horizontal wheel direction used for folder history navigation.", sTray);
        Add(L"Settings › Start in Fullscreen",
            L"Open in fullscreen mode on every launch.", sTray);
        Add(L"Settings › VRAM Cache Size",
            L"How many images to keep decoded in GPU memory (0 – 999). "
            L"Click to open a numeric input dialog; current value is shown in the label.", sTray);
        Add(L"Settings › Window Width / Height",
            L"Default window dimensions in pixels (240 – 16000). Used by Ctrl+Space "
            L"restore and window-reset commands.", sTray);
        Add(L"Settings › History Max Dirs / Favs",
            L"Maximum folders / favorites to show in the History panel (0 – 999 each).", sTray);
        Add(L"Settings › Dir Thumb Cache",
            L"Memory budget in MB for the directory thumbnail bitmap store "
            L"(100 – 64000 MB).", sTray);
        Add(L"Settings › Preload Lookaside",
            L"How many images ahead and behind the current one to pre-decode into GPU "
            L"memory in the background (1 – 99).", sTray);
        Add(L"Settings › Overlay Message Duration",
            L"How long center overlay messages remain visible in milliseconds "
            L"(250 – 10000 ms).", sTray);
        Add(L"Settings › History Save Limit",
            L"Maximum number of folders to persist to disk between sessions "
            L"(1 – 99999).", sTray);
        Add(L"Settings › Export / Import Settings",
            L"Save all settings to a UTF-8 INI file, or load a previously exported one. "
            L"Import requires a confirmation dialog and applies all values immediately, "
            L"including theme, overlays and sort order.", sTray);
        Add(L"Settings › Restore Defaults",
            L"Reset every setting to its compiled-in default value after a confirmation "
            L"dialog. History and favorites are not affected.", sTray);
        Add(L"View Mode submenu",
            L"Pick the default fit mode applied to newly loaded images: "
            L"1 Fit to view (keeps aspect)  •  2 Fit to width  •  3 Fit to height  •  "
            L"4 Stretch to window  •  5 Original size, 1:1 pixels. "
            L"The current mode is shown with a radio-button check.", sTray);
        Add(L"Slideshow submenu",
            L"Configure slideshow defaults: interval (100 – 60000 ms), loop, shuffle, "
            L"and transition type (" + NumI(Constants::Slideshow::TRANSITION_COUNT) +
            L" styles, plus Random). "
            L"All changes persist immediately.", sTray);
        Add(L"Sort submenu",
            L"Choose the playlist sort order (Name / Date Modified / Size / Type / "
            L"Disk Order) and toggle Reverse Order. Takes effect immediately on the "
            L"current folder.", sTray);
        Add(L"Backup › Backup History && Favorites",
            L"Export the history and favorites files into a ZIP archive (file-save "
            L"dialog). The default filename includes today's date.", sTray);
        Add(L"Backup › Restore History && Favorites",
            L"Restore from a previously created ZIP backup after a confirmation dialog. "
            L"Overwrites current history and favorites in memory and on disk.", sTray);

        // ---------------------------------------------------------------
        const int sDed = Sec(Constants::ThemeIcons::ICON_SECTION_DESKTOP, L"DEDICATED SCREENS",
                             L"Isolated copies for unattended displays (F8)");

        Add(K(SC::SC_PANEL_DEDICATED_TOGGLE),
            L"Open the Dedicated panel. A dedicated screen is a separate copy of qIV with "
            L"its own settings file beside it — normally parked fullscreen on one monitor "
            L"running a slideshow. Any number can run at once alongside the main app "
            L"without sharing anything.", sDed);
        Add(L"Dedicated › Generate App",
            L"Copy this executable to a chosen folder as qIV_dedicated_<name>.exe. The "
            L"name is what keeps screens apart: its settings, folder lists, single-instance "
            L"identity and shortcut are all derived from it.", sDed);
        Add(L"Dedicated › Generate Config",
            L"Write that copy's .ini next to it, holding every setting shown in the panel. "
            L"The panel edits the CONFIG only — it never changes the app you are using.", sDed);
        Add(L"Dedicated › Add Images / Add Promotions",
            L"Append a folder to the screen's image list (.qim) or promotion list (.qpr). "
            L"Duplicates are reported and skipped. Missing list files are created.", sDed);
        Add(L"Dedicated › Add Startup / Remove Startup",
            L"Create or delete a shortcut in shell:startup pointing at the copy with "
            L"-dedicated -config \"<its .ini>\", so the screen starts itself after a reboot.", sDed);
        Add(L"Dedicated › Test Config / Test Images / Test Promos",
            L"Check one thing at a time: the .ini, the image list, or the promotion list. "
            L"The list tests count the images actually found per folder and flag folders "
            L"that are missing or empty — a folder that exists but holds nothing decodable "
            L"looks fine until the screen goes blank.", sDed);
        Add(L"Promotions",
            L"A SECOND playlist, never merged into the images. Shown occasionally between "
            L"them and invisible to the image counter and arrow keys. Weighted order "
            L"favours files whose name ends in #N — \"sale#500.jpg\" is 500x likelier than "
            L"a file with no suffix (1 to 65535).", sDed);
        Add(L"Promotion pacing",
            L"Two independent triggers, each a (from, to) pair: 0 = off, a single value is "
            L"an exact cadence, a range is re-rolled each time so it never looks mechanical. "
            L"Count them in images, in seconds, or both — whichever comes due first shows a "
            L"promotion. Promotions can hold the screen for their own dwell time.", sDed);
        Add(L"Kiosk lock",
            L"Tick \"Kiosk lock\" in the panel and the screen ignores every key and every "
            L"click, so a passer-by cannot pause the slideshow or reach the panels. The tray "
            L"icon keeps working — its menu runs its own message loop, so it is unaffected — "
            L"and Settings > Kiosk Lock there is the only way back in. Available to the main "
            L"app too, from the same tray item or -lock.", sDed);
        Add(L"Always on top",
            L"Keeps the screen above every other window, so nothing that pops up can cover "
            L"it. Ctrl+T toggles it live; the panel sets it for a generated screen.", sDed);
        Add(L"Keep display awake",
            L"Blocks the screensaver and display sleep while the window is on screen. Leave "
            L"it off and Windows eventually blanks the display with nobody there to wake it "
            L"— and with the kiosk lock on, nobody could. Released automatically when the "
            L"window hides to the tray.", sDed);
        Add(L"Isolation",
            L"A dedicated screen writes NOTHING to the registry: settings live in its .ini, "
            L"and it keeps no history or favorites. Its own mutex and window class mean a "
            L"file opened from Explorer can never land on a slideshow instead of the main "
            L"window.", sDed);
        Add(L"Turning any copy portable",
            L"Put an .ini beside any copy and it reads settings from that file instead of "
            L"the registry. With Dedicated=0 inside, it stays an ordinary viewer — history, "
            L"favorites and all — just portable.", sDed);

        // ---------------------------------------------------------------
        const int sCli = Sec(Constants::ThemeIcons::ICON_SECTION_KEYBOARD, L"COMMAND-LINE ARGUMENTS",
                             L"Options for QuickImageViewer.exe at launch");

        Add(L"\"path\\to\\image.jpg\"",
            L"Positional argument — open this image at startup and browse its folder.", sCli);
        Add(L"-startFolder <path>",
            L"Open this folder at startup — used as the browse source or, combined with "
            L"-slideshow, as the slideshow source.", sCli);
        Add(L"-background",
            L"Start hidden in the system tray (service mode). QIV waits in RAM and opens "
            L"instantly when an image is requested.", sCli);
        Add(L"-fullscreen", L"Start in fullscreen.", sCli);
        Add(L"-windowedView",
            L"Start windowed — the default; useful as an explicit override.", sCli);
        Add(L"-alwaysOnTop",
            L"Keep the window above all others from launch (also accepted: -awaysOnTop).", sCli);
        Add(L"-monitorNum#N",
            L"Open centered on monitor N (1-based). Example: -monitorNum#2.", sCli);
        Add(L"-slideshow",
            L"Auto-start a slideshow after the content is loaded.", sCli);
        Add(L"-slideshowInterval N",
            L"Seconds between slides (whole number). Example: -slideshowInterval 8.", sCli);
        Add(L"-repeat", L"Loop the slideshow when it reaches the end.", sCli);
        Add(L"-shuffle", L"Play the slideshow in random order.", sCli);
        Add(L"-slideshowTransition=<type>",
            L"Slide transition by name: Cut, Fade, Dissolve, Ripple, Flicker, "
            L"SlideLeft, SlideRight, SlideUp, SlideDown, SlideDiagonal, DriftLeft, DriftUp, "
            L"ZoomIn, ZoomOut, SoftZoom, Spin, SpinZoom, Swing, Bounce, Slam or Iris. "
            L"Example: -slideshowTransition=Fade.", sCli);
        Add(L"-slideshowTransitions=<list>",
            L"Use a custom set of transitions, mirroring List mode in the menu. Accepts "
            L"names or the numbers shown beside them — \"Fade,Iris,Spin\" or \"6,8,17\". "
            L"Unknown entries are ignored.", sCli);
        Add(L"-slideshowTransitionSource=<none|all|list>",
            L"Which transitions are in play: the single one picked, all of them, or "
            L"only the custom list.", sCli);
        Add(L"-slideshowTransitionOrder=<sequential|random>",
            L"How the next transition is drawn from that set.", sCli);
        Add(L"-slideshowTransitionShuffle",
            L"Legacy shorthand for source=all order=random.", sCli);
        Add(L"-hideMouse", L"Hide the mouse cursor at startup.", sCli);
        Add(L"-lock",
            L"KIOSK mode — all keyboard and mouse input is ignored. Combine with "
            L"-fullscreen -slideshow for unattended public displays. The tray icon "
            L"stays live and is the only way to unlock. The switch only forces the "
            L"lock on for one launch; the stored setting is left alone.", sCli);
        Add(L"-keepDisplayAwake",
            L"Block the screensaver and display sleep while the window is on screen.", sCli);
        Add(L"-dedicated",
            L"Run as a dedicated screen: no registry writes at all, no history or favorites, "
            L"its own icon, mutex and window class — safe alongside any number of other "
            L"instances. Settings come from the .ini beside the exe.", sCli);
        Add(L"-config <path>",
            L"Use this .ini instead of the one named after the exe. Generated startup "
            L"shortcuts pass it, so one executable can serve several configured screens.", sCli);
        Add(L"-instance=<name>",
            L"Name this instance. Everything that keeps screens apart derives from it: "
            L"settings file, folder lists, mutex and window class.", sCli);
        Add(L"-instanceDesc=<text>",
            L"Free-text description, shown on the generated shortcut.", sCli);
        Add(L"-promoOrder=<sequential|weighted>",
            L"How the next promotion is chosen. Weighted favours files whose name ends "
            L"in #N.", sCli);
        Add(L"-promoEveryImages=<from>-<to>",
            L"Show a promotion every N images. 0 = off; \"5-0\" is exactly every 5th; "
            L"\"5-15\" is re-rolled between 5 and 15.", sCli);
        Add(L"-promoEverySeconds=<from>-<to>",
            L"Same, counted in seconds. Runs independently of the image counter — "
            L"whichever comes due first shows a promotion.", sCli);
        Add(L"-runOnStartup",
            L"Write or refresh the Windows startup registry entry (HKCU\\…\\Run) so QIV "
            L"launches automatically with Windows — equivalent to enabling \"Run on startup\" "
            L"from the tray icon. Useful after moving the exe or for scripted first-run setup. "
            L"Dedicated instances write their own separate entry.", sCli);
        Add(L"Example",
            L"QuickImageViewer.exe -dedicated -lock -fullscreen -slideshow -shuffle "
            L"-slideshowInterval 8 -startFolder \"D:\\Ads\"", sCli);
        Add(L"Example — dedicated screen",
            L"qIV_dedicated_Lobby.exe -dedicated -config \"D:\\Screens\\qIV_dedicated_Lobby.ini\" "
            L"— the form the Dedicated panel writes into a startup shortcut. Folders and "
            L"every setting come from the .ini and its .qim / .qpr lists.", sCli);
    }

    // =========================================================================
    // ExportToText — UTF-8 with BOM so the section icons and arrows survive
    // =========================================================================
    void HelpWnd::ExportToText() const {
        wchar_t path[MAX_PATH];
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_DESKTOP, nullptr, 0, path)))
            return;
        wcscat_s(path, MAX_PATH, L"\\QIV_Help.txt");

        std::wstring out;
        out.reserve(m_sections.size() * 128 + m_entries.size() * 96 + 256);
        out += m_fullTitle + L" — Shortcuts & Command-Line Reference\n";
        out += std::wstring(60, L'=') + L"\n";

        for (size_t s = 0; s < m_sections.size(); ++s) {
            out += L"\n" + m_sections[s].title + L"  —  " + m_sections[s].subtitle + L"\n";
            out += std::wstring(60, L'-') + L"\n";
            for (const auto &e: m_entries) {
                if (e.sectionId != static_cast<int>(s)) continue;
                out += e.shortcut + L"\n    " + e.description + L"\n";
            }
        }

        // Convert to UTF-8 and write with BOM
        int len = WideCharToMultiByte(CP_UTF8, 0, out.c_str(),
                                      static_cast<int>(out.size()), nullptr, 0, nullptr, nullptr);
        std::string utf8(static_cast<size_t>(len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, out.c_str(), static_cast<int>(out.size()),
                            utf8.data(), len, nullptr, nullptr);

        std::ofstream file(path, std::ios::binary);
        if (file.is_open()) {
            file.write("\xEF\xBB\xBF", 3);
            file.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
            file.close();
            TaskDialog(m_hWnd, nullptr, L"Export Complete",
                       L"Help exported successfully", path,
                       TDCBF_OK_BUTTON, TD_INFORMATION_ICON, nullptr);
        }
    }

    // =========================================================================
    // Fonts / back buffer
    // =========================================================================
    void HelpWnd::EnsureFonts(UINT dpi) {
        if (static_cast<int>(dpi) == m_cachedFontDpi) return;
        DestroyFonts();

        auto Make = [dpi](int logSize, int weight, bool italic) {
            return CreateFontW(MulDiv(logSize, static_cast<int>(dpi), 96), 0, 0, 0, weight,
                               italic ? TRUE : FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
        };

        m_hFontTitle = Make(30, FW_BOLD, false);
        m_hFontSubtitle = Make(13, FW_NORMAL, false);
        m_hFontSection = Make(17, FW_BOLD, false);
        m_hFontSectionSub = Make(12, FW_NORMAL, true);
        m_hFontShortcut = Make(static_cast<int>(13 * KEY_FONT_SCALE), FW_BOLD, false);
        m_hFontDesc = Make(13, FW_NORMAL, false);
        m_hFontFooter = Make(11, FW_NORMAL, false);
        m_cachedFontDpi = static_cast<int>(dpi);
        m_measuredWidth = 0; // font change invalidates measurements
    }

    void HelpWnd::DestroyFonts() {
        for (HFONT *f: {
                 &m_hFontTitle, &m_hFontSubtitle, &m_hFontSection, &m_hFontSectionSub,
                 &m_hFontShortcut, &m_hFontDesc, &m_hFontFooter
             }) {
            if (*f) {
                DeleteObject(*f);
                *f = nullptr;
            }
        }
        m_cachedFontDpi = 0;
    }

    void HelpWnd::EnsureBackBuffer(HDC refDC, int w, int h) {
        if (m_memDC && w == m_bufW && h == m_bufH) return;
        DestroyBackBuffer();
        m_memDC = CreateCompatibleDC(refDC);
        m_memBmp = CreateCompatibleBitmap(refDC, std::max(w, 1), std::max(h, 1));
        m_memBmpOld = static_cast<HBITMAP>(SelectObject(m_memDC, m_memBmp));
        m_bufW = w;
        m_bufH = h;
    }

    void HelpWnd::DestroyBackBuffer() {
        if (m_memDC) {
            if (m_memBmpOld) SelectObject(m_memDC, m_memBmpOld);
            DeleteDC(m_memDC);
            m_memDC = nullptr;
        }
        if (m_memBmp) {
            DeleteObject(m_memBmp);
            m_memBmp = nullptr;
        }
        m_memBmpOld = nullptr;
        m_bufW = m_bufH = 0;
    }

    // =========================================================================
    // MeasureContent — per-row heights via DT_CALCRECT, cached per width+DPI
    // =========================================================================
    void HelpWnd::MeasureContent(HDC hdc, int keyColW, int descColW, UINT dpi) {
        const int contentW = keyColW + descColW;
        if (m_measuredWidth == contentW && m_measuredDpi == static_cast<int>(dpi))
            return;

        m_rowHeights.assign(m_entries.size(), 0);
        m_headerHeights.assign(m_sections.size(), 0);

        const int rowPadY = MulDiv(6, static_cast<int>(dpi), 96);
        const int headGapTop = MulDiv(22, static_cast<int>(dpi), 96);
        const int headGapBottom = MulDiv(10, static_cast<int>(dpi), 96);
        const int subGap = MulDiv(3, static_cast<int>(dpi), 96);

        HGDIOBJ oldFont = SelectObject(hdc, m_hFontShortcut);

        for (size_t i = 0; i < m_entries.size(); ++i) {
            RECT rk = {0, 0, keyColW, 0};
            SelectObject(hdc, m_hFontShortcut);
            DrawTextW(hdc, m_entries[i].shortcut.c_str(), -1, &rk,
                      DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);

            RECT rd = {0, 0, descColW, 0};
            SelectObject(hdc, m_hFontDesc);
            DrawTextW(hdc, m_entries[i].description.c_str(), -1, &rd,
                      DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);

            m_rowHeights[i] = std::max(rk.bottom, rd.bottom) + 2 * rowPadY;
        }

        for (size_t s = 0; s < m_sections.size(); ++s) {
            RECT rt = {0, 0, contentW, 0};
            SelectObject(hdc, m_hFontSection);
            std::wstring head = m_sections[s].icon + L"  " + m_sections[s].title;
            DrawTextW(hdc, head.c_str(), -1, &rt, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);

            RECT rs = {0, 0, contentW, 0};
            SelectObject(hdc, m_hFontSectionSub);
            DrawTextW(hdc, m_sections[s].subtitle.c_str(), -1, &rs,
                      DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);

            m_headerHeights[static_cast<int>(s)] =
                    headGapTop + rt.bottom + subGap + rs.bottom + headGapBottom;
        }

        m_totalContentHeight = 0;
        for (size_t s = 0; s < m_sections.size(); ++s) {
            m_totalContentHeight += m_headerHeights[s];
            for (size_t i = 0; i < m_entries.size(); ++i)
                if (m_entries[i].sectionId == static_cast<int>(s))
                    m_totalContentHeight += m_rowHeights[i];
        }

        SelectObject(hdc, oldFont);
        m_measuredWidth = contentW;
        m_measuredDpi = static_cast<int>(dpi);
        m_view.scrollY = std::clamp(m_view.scrollY, 0, std::max(0, m_totalContentHeight));
    }

    // =========================================================================
    // Show
    // =========================================================================
    // One wheel "line" is roughly one text line here. The base applies the
    // user's Mouse setting and the Shift accelerator on top.
    int HelpWnd::ScrollLinePx(const UI::ScrollView &) const {
        return static_cast<int>(20 * app.dpiScale);
    }

    void HelpWnd::Show() {
        m_filter.Reset(); // fresh open starts unfiltered
        m_query.clear();
        m_entryMatch.clear();
        m_view.scrollY = 0;
        ShowCenterOverParent();
    }

    // =========================================================================
    // Keyboard
    // =========================================================================
    // Esc: clear the filter if typed (same as the ✕ button), else hide the panel.
    bool HelpWnd::OnLocalHide() {
        return m_filter.RouteKey(VK_ESCAPE, m_hWnd) == InputResult::RequestClear;
    }

    bool HelpWnd::OnKeyDown(WPARAM vk, bool ctrl, bool /*shift*/, bool /*alt*/) {
        // Host-specific keys first: help toggle, export, and list scrolling.
        if (vk == Shortcuts::SC_PANEL_HELP_TOGGLE) {
            Hide();
            return true;
        }
        if (vk == 'E' && ctrl) {
            ExportToText();
            return true;
        }
        if (vk == VK_PRIOR || vk == VK_NEXT) {
            // ~one viewport per press, floored so a tiny window still moves.
            const int page = std::max(m_view.Height() * 9 / 10, 40);
            m_view.ScrollBy(0, (vk == VK_NEXT) ? page : -page);
            InvalidateRect(m_hWnd, nullptr, FALSE);
            return true;
        }
        if (vk == VK_HOME) {
            m_view.scrollY = 0;
            InvalidateRect(m_hWnd, nullptr, FALSE);
            return true;
        }
        if (vk == VK_END) {
            m_view.scrollY = m_view.MaxScrollY();
            InvalidateRect(m_hWnd, nullptr, FALSE);
            return true;
        }

        // Everything else → the filter box (typing, editing, forward policy).
        switch (m_filter.RouteKey(vk, m_hWnd)) {
            case InputResult::Ignored:         return false; // forward to app pipeline
            case InputResult::RequestClose:    return false; // (Esc arrives via OnLocalHide)
            case InputResult::RequestClear:    InvalidateRect(m_hWnd, nullptr, FALSE); return true;
            case InputResult::ConsumedRepaint: InvalidateRect(m_hWnd, nullptr, FALSE); return true;
            case InputResult::Consumed:        return true;
        }
        return true;
    }

    // =========================================================================
    // HandlePanelMessage
    // =========================================================================
    LRESULT HelpWnd::HandlePanelMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
            // Double buffering — the back buffer is the only thing that paints,
            // so background erasure would just cause flicker.
            case WM_ERASEBKGND:
                return 1;

            case WM_SIZE:
                m_measuredWidth = 0; // re-measure rows for the new width
                InvalidateRect(m_hWnd, nullptr, FALSE);
                return 0;

            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC hdcWin = BeginPaint(m_hWnd, &ps);
                RECT rc;
                GetClientRect(m_hWnd, &rc);

                const UINT dpi = static_cast<UINT>(app.dpiScale * 96.0f);
                const int dpiI = static_cast<int>(dpi);
                EnsureFonts(dpi);
                EnsureBackBuffer(hdcWin, rc.right, rc.bottom);
                HDC hdc = m_memDC;

                // ---- background --------------------------------------------
                FillRect(hdc, &rc, UI::Gdi::Brush(GetBgColor()));
                SetBkMode(hdc, TRANSPARENT);

                // ---- metrics ------------------------------------------------
                const int padding = MulDiv(28, dpiI, 96);
                const int sbWidth = UI::ScrollBarThicknessPx(app.dpiScale);
                const int rowIndent = MulDiv(14, dpiI, 96);
                const int colGap = MulDiv(14, dpiI, 96);
                const int rowPadY = MulDiv(6, dpiI, 96);
                const int barW = MulDiv(4, dpiI, 96);
                const int headGapTop = MulDiv(22, dpiI, 96);
                const int subGap = MulDiv(3, dpiI, 96);

                const int contentLeft = rc.left + padding;
                const int contentRight = rc.right - padding - sbWidth;

                // Key column: 33% of the content width, clamped
                int keyColW = (contentRight - contentLeft - rowIndent - colGap) * 33 / 100;
                keyColW = std::clamp(keyColW, MulDiv(170, dpiI, 96), MulDiv(230, dpiI, 96));
                const int descColW = contentRight - contentLeft - rowIndent - colGap - keyColW;

                // ---- theme colors -------------------------------------------
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
                const COLORREF keyColor = Constants::Theme::ThemedColor(
                        Constants::Theme::HelpWindow::SHORTCUT_KEY_R,
                        Constants::Theme::HelpWindow::SHORTCUT_KEY_G,
                        Constants::Theme::HelpWindow::SHORTCUT_KEY_B, app.themeFactor);
                const COLORREF descColor = Constants::Theme::ThemedGray(
                        Constants::Theme::HelpWindow::DESCRIPTION, app.themeFactor);
                const COLORREF subColor = Constants::Theme::ThemedGray(
                        Constants::Theme::HelpWindow::SUBTITLE, app.themeFactor);
                // Row separator — dim gray (70/255 ≈ 0.27, normalized like all theme bases)
                const COLORREF sepColor = Constants::Theme::ThemedGray(0.27f, app.themeFactor);

                // ---- title + subtitle ---------------------------------------
                SelectObject(hdc, m_hFontTitle);
                SetTextColor(hdc, Constants::Theme::ThemedColor(
                                     Constants::Theme::HelpWindow::TITLE_R,
                                     Constants::Theme::HelpWindow::TITLE_G,
                                     Constants::Theme::HelpWindow::TITLE_B, app.themeFactor));
                RECT titleRect = {contentLeft, rc.top + padding, contentRight, 0};
                DrawTextW(hdc, L"Quick Image Viewer", -1, &titleRect,
                          DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
                titleRect.right = contentRight;
                DrawTextW(hdc, L"Quick Image Viewer", -1, &titleRect,
                          DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);

                SelectObject(hdc, m_hFontSubtitle);
                SetTextColor(hdc, subColor);
                RECT subtitleRect = {
                    contentLeft, titleRect.bottom + MulDiv(5, dpiI, 96),
                    contentRight, titleRect.bottom + MulDiv(25, dpiI, 96)
                };
                std::wstring subtitle = m_fullTitle + L"  —  shortcuts & command-line reference";
                DrawTextW(hdc, subtitle.c_str(), -1, &subtitleRect,
                          DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);

                // ---- filter box (reuses the shared InputBox) ----------------
                // Drawn above the content clip region; typing filters the list.
                const int filterH = MulDiv(26, dpiI, 96);
                RECT filterRect = {
                    contentLeft, subtitleRect.bottom + MulDiv(12, dpiI, 96),
                    contentRight, subtitleRect.bottom + MulDiv(12, dpiI, 96) + filterH
                };
                m_filter.Draw(hdc, m_hFontDesc, filterRect, MulDiv(8, dpiI, 96),
                              GetFocus() == m_hWnd);

                // ---- content geometry ---------------------------------------
                const int contentTop = filterRect.bottom + MulDiv(14, dpiI, 96);
                const int footerH = MulDiv(44, dpiI, 96);
                const int contentBottom = rc.bottom - footerH;

                // No contentHeight local, and no m_contentTop / m_viewHeight
                // members: all three existed only to feed the scroll maths, and
                // m_view.view carries the same numbers as one rectangle once
                // Layout has set it below.

                MeasureContent(hdc, keyColW, descColW, dpi);

                // Filtered content height drives all scroll math (scrollbar, drag,
                // End key). Sum only visible sections + matching rows.
                m_visibleContentHeight = 0;
                for (size_t s = 0; s < m_sections.size(); ++s) {
                    if (!SectionHasVisible(static_cast<int>(s))) continue;
                    m_visibleContentHeight += m_headerHeights[s];
                    for (size_t i = 0; i < m_entries.size(); ++i)
                        if (m_entries[i].sectionId == static_cast<int>(s) && EntryVisible(i))
                            m_visibleContentHeight += m_rowHeights[i];
                }

                // Which bars, where the content goes, and the clamp — one call.
                // The bar's column is inside the region, so Layout takes it back
                // out only when a bar is actually needed.
                m_view.contentH = m_visibleContentHeight;
                m_view.Layout(RECT{contentLeft, contentTop,
                                   contentRight + sbWidth, contentBottom}, sbWidth);

                // ---- clip + draw sections -----------------------------------
                HRGN hrgn = CreateRectRgn(contentLeft, contentTop, contentRight + sbWidth, contentBottom);
                SelectClipRgn(hdc, hrgn);

                int y = contentTop - m_view.scrollY;

                for (size_t s = 0; s < m_sections.size(); ++s) {
                    // Filter: skip sections with no matching entry.
                    if (!SectionHasVisible(static_cast<int>(s))) continue;

                    const COLORREF secColor = sectionColors[s % 4];
                    const int headH = m_headerHeights[s];

                    // Skip whole section if entirely above/below the viewport
                    int sectionH = headH;
                    for (size_t i = 0; i < m_entries.size(); ++i)
                        if (m_entries[i].sectionId == static_cast<int>(s) && EntryVisible(i))
                            sectionH += m_rowHeights[i];

                    if (y + sectionH < contentTop || y > contentBottom) {
                        y += sectionH;
                        continue;
                    }

                    // -- header: accent bar + icon + title, subtitle below ----
                    int hy = y + headGapTop;

                    SelectObject(hdc, m_hFontSection);
                    std::wstring head = m_sections[s].icon + L"  " + m_sections[s].title;
                    RECT rt = {0, 0, contentRight - contentLeft, 0};
                    DrawTextW(hdc, head.c_str(), -1, &rt, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);

                    RECT barRect = {contentLeft, hy, contentLeft + barW, hy + rt.bottom};
                    FillRect(hdc, &barRect, UI::Gdi::Brush(secColor));

                    SetTextColor(hdc, secColor);
                    RECT headRect = {
                        contentLeft + barW + MulDiv(10, dpiI, 96), hy,
                        contentRight, hy + rt.bottom
                    };
                    DrawTextW(hdc, head.c_str(), -1, &headRect,
                              DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
                    hy += rt.bottom + subGap;

                    SelectObject(hdc, m_hFontSectionSub);
                    SetTextColor(hdc, subColor);
                    RECT subRect = {
                        contentLeft + barW + MulDiv(10, dpiI, 96), hy,
                        contentRight, y + headH
                    };
                    DrawTextW(hdc, m_sections[s].subtitle.c_str(), -1, &subRect,
                              DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);

                    y += headH;

                    // -- rows: key column | description column ----------------
                    for (size_t i = 0; i < m_entries.size(); ++i) {
                        if (m_entries[i].sectionId != static_cast<int>(s)) continue;
                        if (!EntryVisible(i)) continue; // filtered out
                        const int rowH = m_rowHeights[i];

                        if (y + rowH >= contentTop && y <= contentBottom) {
                            SelectObject(hdc, m_hFontShortcut);
                            SetTextColor(hdc, keyColor);
                            RECT keyRect = {
                                contentLeft + rowIndent, y + rowPadY,
                                contentLeft + rowIndent + keyColW, y + rowH - rowPadY
                            };
                            // Highlight the matched characters in the shortcut chord
                            // (single-line — Common::DrawMatchText). Falls back to a
                            // plain wrapped draw when not filtering / no match here.
                            const bool hlShortcut = !m_query.empty() &&
                                                    i < m_entryMatch.size() &&
                                                    m_entryMatch[i].scCount > 0;
                            if (hlShortcut) {
                                const EntryMatch& em = m_entryMatch[i];
                                const std::wstring& sc = m_entries[i].shortcut;
                                const int scLen = static_cast<int>(std::min(sc.size(), static_cast<size_t>(255)));
                                bool isHL[256] = {};
                                for (int k = 0; k < em.scCount; ++k) {
                                    const int pos = em.scPos[k];
                                    if (pos >= 0 && pos < scLen) isHL[pos] = true;
                                }
                                const COLORREF hlColor = Constants::Theme::ThemedColor(1.0f, 1.0f, 1.0f, app.themeFactor);
                                Common::DrawMatchText(hdc, sc.c_str(), scLen, isHL,
                                                      keyRect.left, keyRect.top, keyRect, keyColor, hlColor);
                            } else {
                                DrawTextW(hdc, m_entries[i].shortcut.c_str(), -1, &keyRect,
                                          DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
                            }

                            SelectObject(hdc, m_hFontDesc);
                            SetTextColor(hdc, descColor);
                            RECT descRect = {
                                contentLeft + rowIndent + keyColW + colGap, y + rowPadY,
                                contentRight, y + rowH - rowPadY
                            };
                            DrawTextW(hdc, m_entries[i].description.c_str(), -1, &descRect,
                                      DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);

                            // thin separator under the row
                            RECT sep = {contentLeft + rowIndent, y + rowH - 1, contentRight, y + rowH};
                            FillRect(hdc, &sep, UI::Gdi::Brush(sepColor));
                        }
                        y += rowH;
                    }
                }

                SelectClipRgn(hdc, nullptr);
                DeleteObject(hrgn);

                // ---- scrollbar ----------------------------------------------
                UI::DrawBars(hdc, m_view, app.dpiScale,
                             UI::ThemeScrollBarColors(app.themeFactor));

                // ---- footer --------------------------------------------------
                SelectObject(hdc, m_hFontFooter);
                SetTextColor(hdc, subColor);
                RECT copyrightRect = {
                    contentLeft, rc.bottom - MulDiv(38, dpiI, 96),
                    contentRight, rc.bottom - MulDiv(20, dpiI, 96)
                };
                std::wstring copyrightText = std::wstring(Constants::APP_CREATOR) + L" | " +
                                             Constants::APP_HELP_FOOTER;
                DrawTextW(hdc, copyrightText.c_str(), -1, &copyrightRect,
                          DT_CENTER | DT_TOP | DT_NOPREFIX);

                m_footerLinkRect = {
                    contentLeft, rc.bottom - MulDiv(18, dpiI, 96),
                    contentRight, rc.bottom - MulDiv(2, dpiI, 96)
                };
                SetTextColor(hdc, Constants::Theme::ThemedColor(
                                     Constants::Links::COLOR_R_F,
                                     Constants::Links::COLOR_G_F,
                                     Constants::Links::COLOR_B_F, app.themeFactor));
                DrawTextW(hdc, L"Follow on Facebook - Ivan Hristov Yanev", -1,
                          &m_footerLinkRect, DT_CENTER | DT_TOP | DT_NOPREFIX);

                // ---- flip ----------------------------------------------------
                BitBlt(hdcWin, 0, 0, rc.right, rc.bottom, hdc, 0, 0, SRCCOPY);
                EndPaint(m_hWnd, &ps);
                return 0;
            }

            case WM_CHAR: {
                wchar_t ch = static_cast<wchar_t>(wParam);
                if (m_filter.RouteChar(ch, m_hWnd) == InputResult::ConsumedRepaint)
                    InvalidateRect(m_hWnd, nullptr, FALSE);
                return 0;
            }

            // No wheel cases: FloatingPanelWnd drives both against
            // ScrollViewAt() and consumes them before this panel is asked.

            case WM_RBUTTONUP:
                // Right-click inside the filter → Cut/Copy/Paste menu.
                if (m_filter.RouteMouse(WM_RBUTTONUP, wParam, lParam, m_hWnd) == InputResult::ConsumedRepaint)
                    InvalidateRect(m_hWnd, nullptr, FALSE);
                return 0;

            case WM_LBUTTONDOWN: {
                POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

                // Filter box (✕ button or click-to-caret) — before other targets.
                if (m_filter.RouteMouse(WM_LBUTTONDOWN, wParam, lParam, m_hWnd) == InputResult::ConsumedRepaint) {
                    InvalidateRect(m_hWnd, nullptr, FALSE);
                    return 0;
                }

                if (pt.x >= m_footerLinkRect.left && pt.x <= m_footerLinkRect.right &&
                    pt.y >= m_footerLinkRect.top && pt.y <= m_footerLinkRect.bottom) {
                    ShellExecuteW(nullptr, L"open", L"https://www.facebook.com/IvanHristovYanev",
                                  nullptr, nullptr, SW_SHOW);
                    return 0;
                }

                RECT rc;
                GetClientRect(m_hWnd, &rc);
                UINT dpi = static_cast<UINT>(app.dpiScale * 96.0f);
                int sbWidth = MulDiv(10, static_cast<int>(dpi), 96);
                int sbX = rc.right - sbWidth - MulDiv(5, static_cast<int>(dpi), 96);

                // The bar never reaches here — the base consumes thumb and track
                // clicks. What is left is the bar's column with no bar drawn.
                (void) sbX;
                return 0;
            }

            case WM_MOUSEMOVE: {
                POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

                // Filter ✕ hover state / drag-select.
                if (m_filter.RouteMouse(WM_MOUSEMOVE, wParam, lParam, m_hWnd) == InputResult::ConsumedRepaint)
                    InvalidateRect(m_hWnd, nullptr, FALSE);

                if (pt.x >= m_footerLinkRect.left && pt.x <= m_footerLinkRect.right &&
                    pt.y >= m_footerLinkRect.top && pt.y <= m_footerLinkRect.bottom) {
                    SetCursor(Constants::Cursors::CURR_CLICK);
                } else {
                    SetCursor(Constants::Cursors::CURR_DEFAULT);
                }

                // A thumb drag never reaches here — the base holds capture.
                return 0;
            }

            case WM_LBUTTONUP: {
                // A scrollbar release never arrives here — the base owns that
                // button-up — so this cannot be mistaken for the filter's.
                // Ends a filter-box drag-select. Without it the box only drops
                // m_dragging on the next WM_MOUSEMOVE, so moving after release
                // keeps extending the selection.
                if (m_filter.RouteMouse(WM_LBUTTONUP, wParam, lParam, m_hWnd) == InputResult::ConsumedRepaint)
                    InvalidateRect(m_hWnd, nullptr, FALSE);
                return 0;
            }

            case WM_CLOSE: {
                Hide();
                return 0;
            }
        }

        return DefWindowProcW(m_hWnd, message, wParam, lParam);
    }
} // namespace UI
