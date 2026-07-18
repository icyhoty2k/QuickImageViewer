#include "HelpWnd.h"
#include "../../Platform/Constants.h"
#include "../../AppState.h"
#include "Shortcuts.h"
#include <string>
#include <vector>
#include <dwmapi.h>
#include <shellapi.h>
#include <algorithm>
#include <shlobj.h>
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
        const int sNav = Sec(L"🧭", L"IMAGE NAVIGATION",
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
        const int sZoom = Sec(L"🔍", L"ZOOM, PAN & VIEW MODES",
                              L"How the image fits and moves inside the window");

        Add(L"Up / Down",
            L"Zoom in / out by ×" + NumF(Constants::ZOOM_STEP) + L" per step.", sZoom);
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

        // ---------------------------------------------------------------
        const int sMouse = Sec(L"🖱️", L"MOUSE CONTROLS",
                               L"All pointer actions on the main window");

        Add(L"ℹ Button roles",
            L"The roles below assume SWAP_MOUSE_BUTTONS = true in Constants.h (the shipped "
            L"default). Set it to false to exchange the left and right button functions.", sMouse);
        Add(L"LMB hold / drag",
            L"Quick " + NumF(Constants::ZOOM_CLICK) + L"× zoom centered on the cursor; "
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
        const int sWin = Sec(L"🪟", L"WINDOW MANAGEMENT",
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
        Add(L"Drag near screen edge",
            L"Releasing a window drag within " + NumI(Constants::WINDOW_SNAP_DISTANCE) +
            L" px of a screen edge snaps the window to that edge.", sWin);
        Add(K(SC::SC_PANEL_FULLSCREEN_F) + L" / " + K(SC::SC_PANEL_FULLSCREEN) + L" / " +
            K(SC::SC_PANEL_FULLSCREEN_ENTER) + L" / " + CtrlShift(SC::SC_PANEL_FULLSCREEN_T),
            L"Toggle borderless fullscreen on the current monitor.", sWin);
        Add(Ctrl(SC::SC_ALWAYS_ON_TOP) + L"  /  " + Ctrl(SC::SC_ALWAYS_ON_TOP_A),
            L"Toggle always-on-top — the main window and every visible panel stay above "
            L"all other windows.", sWin);

        // ---------------------------------------------------------------
        const int sPanels = Sec(L"🧰", L"PANELS & TOOLS",
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
        Add(K(SC::SC_PANEL_DIR_TOGGLE) + L" / " + K(SC::SC_PANEL_DIR_MOVE),
            L"Toggle the current-directory strip / move it to the next free screen edge.", sPanels);
        Add(K(SC::SC_PANEL_CACHE_CLEAR),
            L"Clear the VRAM cache. Images are re-decoded on demand afterwards.", sPanels);
        Add(K(SC::IPANNEL_WINDOW_LOCAL_HIDE) + L"  /  " + Ctrl(SC::SC_APP_HIDE_ALT),
            L"Close the focused panel — works in every panel window.", sPanels);

        // ---------------------------------------------------------------
        const int sThumbs = Sec(L"🖼️", L"THUMBNAIL STRIPS",
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
        Add(L"Right Click",
            L"Context menu on any thumbnail: Copy, Cut, Delete and Paste. Copy / Cut use "
            L"the Windows clipboard, so files can also be pasted in Explorer. A cut file "
            L"is shown dimmed until it is pasted. Delete sends the file to the Recycle "
            L"Bin. Paste drops clipboard files into the strip's folder. Also: Select All, "
            L"Select Inverse (flips the current selection) and Select None.", sThumbs);

        // ---------------------------------------------------------------
        const int sHist = Sec(L"📜", L"HISTORY PANEL",
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
            L" strips (top, left, right, bottom). Great for comparing folders.", sHist);
        Add(K(SC::HISTORY_FAVORITES_TOGGLE_KEY),
            L"Toggle favorite on the hovered entry. Favorites survive history clears.", sHist);
        Add(K(VK_DELETE),
            L"Delete the hovered entry. " + Ctrl('Z') + L" restores the last deleted one.", sHist);
        Add(CtrlShift(SC::HISTORY_CLEAR_ALL_HISTORY_BUT_NOT_FAVORITES),
            L"Clear the entire history but keep all favorites.", sHist);
        Add(CtrlAltShift(SC::HISTORY_CLEAR_ALL_FAVORITES_BUT_NOT_HISTORY),
            L"Clear all favorites but keep the history.", sHist);

        // ---------------------------------------------------------------
        const int sSlide = Sec(L"▶️", L"SLIDESHOW",
                               L"Automatic playback with transitions");

        Add(Ctrl(SC::SC_SLIDESHOW_TOGGLE), L"Start / stop the slideshow.", sSlide);
        Add(K(SC::SC_SLIDESHOW_PAUSE_RESUME) + L"  (while running)",
            L"Pause / resume the slideshow.", sSlide);
        Add(K(SC::SC_SLIDESHOW_LOOP_TOGGLE) + L"  (while running)",
            L"Toggle loop — restart from the first image after the last.", sSlide);
        Add(K(SC::SC_SLIDESHOW_SHUFFLE_TOGGLE) + L"  (while running)",
            L"Toggle shuffle — show the playlist in random order.", sSlide);
        Add(K(SC::SC_SLIDESHOW_TRANSITION_CYCLE) + L"  (while running)",
            L"Cycle the slide transition: Cut → Fade → Dissolve → Ripple → Push → Zoom.", sSlide);

        // ---------------------------------------------------------------
        const int sOverlay = Sec(L"ℹ️", L"INFO OVERLAYS",
                                 L"The 3×3 on-screen information grid");

        Add(K(SC::SC_PANEL_OVERLAY_TOGGLE) + L" / " + K(SC::SC_PANEL_OVERLAY_MASTER) +
            L" / " + Ctrl(SC::SC_PANEL_OVERLAY_MASTER_CTRL0),
            L"Master toggle — show / hide all overlay slots at once.", sOverlay);
        Add(Ctrl(SC::SC_OVERLAY_SLOT_1) + L" – " + Ctrl(SC::SC_OVERLAY_SLOT_9),
            L"Toggle an individual slot of the 3×3 grid. Assignments:  1 index + filename  •  "
            L"2 zoom %  •  5 center message area  •  7 active effects list  •  "
            L"9 dimensions + file size. Remaining slots are reserved.", sOverlay);
        Add(CtrlShift(SC::SC_OVERLAY_COMPACT_1) + L" – " + CtrlShift(SC::SC_OVERLAY_COMPACT_9),
            L"Toggle compact mode per slot — one line instead of two. Slot 5 (center "
            L"message) is always single-line.", sOverlay);
        Add(K(SC::SC_OVERLAY_LAYOUT_CYCLE),
            L"Cycle the overlay layout: Grid → Stacked → Summary.", sOverlay);
        Add(K(SC::SC_OVERLAY_BG_TOGGLE),
            L"Toggle the semi-transparent background behind overlay text — the text itself "
            L"always stays visible.", sOverlay);

        // ---------------------------------------------------------------
        const int sFx = Sec(L"🎨", L"EFFECTS & COLOR",
                            L"Non-destructive adjustments — the file on disk is never touched");

        Add(K(SC::SC_TRANSFORM_ROTATE) + L" / " + Shift(SC::SC_TRANSFORM_ROTATE),
            L"Rotate 90° clockwise / counter-clockwise.", sFx);
        Add(K(SC::SC_TRANSFORM_FLIP_H) + L" / " + K(SC::SC_TRANSFORM_FLIP_V),
            L"Flip horizontally / vertically.", sFx);
        Add(K(FX::SC_COLOR_GRAYSCALE), L"Toggle grayscale.", sFx);
        Add(K(FX::SC_COLOR_INVERT), L"Toggle color inversion (negative).", sFx);
        Add(K(FX::SC_COLOR_SEPIA), L"Toggle sepia tone.", sFx);
        Add(K(FX::SC_COLOR_SOLARIZE),
            L"Toggle solarize — inverts only tones above " +
            NumF(Constants::SOLARIZE_THRESHOLD * 100.0f) + L"% brightness.", sFx);
        Add(K(FX::SC_COLOR_OUTLINE), L"Toggle outline — GPU edge detection.", sFx);
        Add(K(FX::SC_COLOR_THRESHOLD),
            L"Toggle black & white threshold — pixels above " +
            NumF(Constants::BW_THRESHOLD_LEVEL * 100.0f) + L"% brightness become white, the "
            L"rest black.", sFx);
        Add(K(FX::SC_COLOR_SAT_DOWN) + L" / " + K(FX::SC_COLOR_SAT_UP),
            L"Saturation − / + in " + NumF(Constants::COLOR_ADJUST_STEP) +
            L" steps  (0 = grayscale, maximum " + NumF(Constants::MIN_MAX_SATURATION) + L").", sFx);
        Add(K(FX::SC_COLOR_BRIGHTNESS_UP) + L" / " + K(FX::SC_COLOR_BRIGHTNESS_DOWN),
            L"Brightness + / − in " + NumF(Constants::COLOR_ADJUST_STEP) +
            L" steps  (range ±" + NumF(Constants::MIN_MAX_BRIGHTNESS) + L").", sFx);
        Add(K(FX::SC_COLOR_CONTRAST_UP) + L" / " + K(FX::SC_COLOR_CONTRAST_DOWN),
            L"Contrast + / − in " + NumF(Constants::COLOR_ADJUST_STEP) +
            L" steps  (maximum " + NumF(Constants::MIN_MAX_CONTRAST) + L").", sFx);
        Add(K(FX::SC_COLOR_GAMMA_UP) + L" / " + K(FX::SC_COLOR_GAMMA_DOWN),
            L"Gamma + / − in " + NumF(Constants::GAMMA_STEP) + L" steps  (range " +
            NumF(Constants::MIN_GAMMA) + L" – " + NumF(Constants::MAX_GAMMA) + L").", sFx);
        Add(K(FX::SC_COLOR_RESET_ALL_EFFECTS),
            L"Reset every color adjustment and effect back to neutral.", sFx);
        Add(K(FX::SC_EFFECT_APPLY_TOGGLE),
            L"Effects bypass — temporarily view the untouched original; press again to "
            L"re-apply all active effects.", sFx);

        // ---------------------------------------------------------------
        const int sFiles = Sec(L"💾", L"FILES, CLIPBOARD & SORTING",
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
        const int sApp = Sec(L"⚙️", L"APPLICATION & APPEARANCE",
                             L"Lifecycle, theme and window chrome");

        Add(K(SC::SC_APP_HIDE) + L"  /  " + Ctrl(SC::SC_APP_HIDE_ALT),
            L"Hide to the system tray — the process stays resident so the next open is "
            L"instant. Extra running instances are closed.", sApp);
        Add(Ctrl(SC::SC_APP_HARD_QUIT),
            L"Hard quit — fully removes the process from memory.", sApp);
        Add(Ctrl(SC::SC_APP_NEW_WINDOW), L"Open a new independent QIV window.", sApp);
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
        const int sCli = Sec(L"⌨️", L"COMMAND-LINE ARGUMENTS",
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
            L"Slide transition: Cut, Fade, Dissolve, Ripple, Push or Zoom. "
            L"Example: -slideshowTransition=Fade.", sCli);
        Add(L"-slideshowTransitionShuffle",
            L"Pick a random transition for every slide.", sCli);
        Add(L"-hideMouse", L"Hide the mouse cursor at startup.", sCli);
        Add(L"-lock",
            L"KIOSK mode — all keyboard and mouse input is ignored. Combine with "
            L"-fullscreen -slideshow for unattended public displays.", sCli);
        Add(L"-dedicated",
            L"Isolated instance: no registry writes, a separate history file, its own tray "
            L"icon and mutex — safe to run alongside a normal QIV instance.", sCli);
        Add(L"Example",
            L"QuickImageViewer.exe -dedicated -lock -fullscreen -slideshow -shuffle "
            L"-slideshowInterval 8 -startFolder \"D:\\Ads\"", sCli);
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
            MessageBoxW(m_hWnd, (std::wstring(L"Help exported to:\n") + path).c_str(),
                        L"Export Complete", MB_OK | MB_ICONINFORMATION);
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
        m_scrollOffsetY = std::clamp(m_scrollOffsetY, 0, std::max(0, m_totalContentHeight));
    }

    // =========================================================================
    // Show
    // =========================================================================
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

    // =========================================================================
    // Keyboard
    // =========================================================================
    bool HelpWnd::OnKeyDown(WPARAM vk, bool ctrl, bool /*shift*/, bool /*alt*/) {
        if (vk == Shortcuts::SC_PANEL_HELP_TOGGLE) {
            Hide();
            return true;
        }
        if (vk == 'E' && ctrl) {
            ExportToText();
            return true;
        }
        if (vk == VK_PRIOR || vk == VK_NEXT) {
            const int page = std::max(m_viewHeight * 9 / 10, 40); // ~one viewport per press
            m_scrollOffsetY += (vk == VK_NEXT) ? page : -page;
            InvalidateRect(m_hWnd, nullptr, FALSE);
            return true;
        }
        if (vk == VK_HOME) {
            m_scrollOffsetY = 0;
            InvalidateRect(m_hWnd, nullptr, FALSE);
            return true;
        }
        if (vk == VK_END) {
            m_scrollOffsetY = std::max(0, m_totalContentHeight - m_viewHeight);
            InvalidateRect(m_hWnd, nullptr, FALSE);
            return true;
        }
        return false;
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
                HBRUSH hBrush = CreateSolidBrush(GetBgColor());
                FillRect(hdc, &rc, hBrush);
                DeleteObject(hBrush);
                SetBkMode(hdc, TRANSPARENT);

                // ---- metrics ------------------------------------------------
                const int padding = MulDiv(28, dpiI, 96);
                const int sbWidth = MulDiv(10, dpiI, 96);
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

                // ---- content geometry ---------------------------------------
                const int contentTop = subtitleRect.bottom + MulDiv(20, dpiI, 96);
                const int footerH = MulDiv(44, dpiI, 96);
                const int contentBottom = rc.bottom - footerH;
                const int contentHeight = std::max(contentBottom - contentTop, 1);

                m_contentTop = contentTop;
                m_viewHeight = contentHeight;

                MeasureContent(hdc, keyColW, descColW, dpi);

                const int maxScroll = std::max(0, m_totalContentHeight - contentHeight);
                m_scrollOffsetY = std::clamp(m_scrollOffsetY, 0, maxScroll);

                // ---- clip + draw sections -----------------------------------
                HRGN hrgn = CreateRectRgn(contentLeft, contentTop, contentRight + sbWidth, contentBottom);
                SelectClipRgn(hdc, hrgn);

                int y = contentTop - m_scrollOffsetY;

                for (size_t s = 0; s < m_sections.size(); ++s) {
                    const COLORREF secColor = sectionColors[s % 4];
                    const int headH = m_headerHeights[s];

                    // Skip whole section if entirely above/below the viewport
                    int sectionH = headH;
                    for (size_t i = 0; i < m_entries.size(); ++i)
                        if (m_entries[i].sectionId == static_cast<int>(s))
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
                    HBRUSH hBar = CreateSolidBrush(secColor);
                    FillRect(hdc, &barRect, hBar);
                    DeleteObject(hBar);

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
                        const int rowH = m_rowHeights[i];

                        if (y + rowH >= contentTop && y <= contentBottom) {
                            SelectObject(hdc, m_hFontShortcut);
                            SetTextColor(hdc, keyColor);
                            RECT keyRect = {
                                contentLeft + rowIndent, y + rowPadY,
                                contentLeft + rowIndent + keyColW, y + rowH - rowPadY
                            };
                            DrawTextW(hdc, m_entries[i].shortcut.c_str(), -1, &keyRect,
                                      DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);

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
                            HBRUSH hSep = CreateSolidBrush(sepColor);
                            FillRect(hdc, &sep, hSep);
                            DeleteObject(hSep);
                        }
                        y += rowH;
                    }
                }

                SelectClipRgn(hdc, nullptr);
                DeleteObject(hrgn);

                // ---- scrollbar ----------------------------------------------
                if (maxScroll > 0) {
                    const int sbX = rc.right - sbWidth - MulDiv(5, dpiI, 96);
                    const int trackTop = contentTop;
                    const int trackBottom = contentBottom;
                    const int trackH = trackBottom - trackTop;

                    const int thumbH = std::max(MulDiv(30, dpiI, 96),
                                                trackH * contentHeight / m_totalContentHeight);
                    const int thumbY = trackTop + (trackH - thumbH) * m_scrollOffsetY / maxScroll;

                    HBRUSH hTrack = CreateSolidBrush(Constants::Theme::ThemedColor(
                            Constants::Theme::HelpWindow::SCROLLBAR_TRACK_R,
                            Constants::Theme::HelpWindow::SCROLLBAR_TRACK_G,
                            Constants::Theme::HelpWindow::SCROLLBAR_TRACK_B, app.themeFactor));
                    RECT trackRect = {sbX, trackTop, sbX + sbWidth, trackBottom};
                    FillRect(hdc, &trackRect, hTrack);
                    DeleteObject(hTrack);

                    HBRUSH hThumb = CreateSolidBrush(Constants::Theme::ThemedColor(
                            Constants::Theme::HelpWindow::SCROLLBAR_THUMB_R,
                            Constants::Theme::HelpWindow::SCROLLBAR_THUMB_G,
                            Constants::Theme::HelpWindow::SCROLLBAR_THUMB_B, app.themeFactor));
                    RECT thumbRect = {sbX, thumbY, sbX + sbWidth, thumbY + thumbH};
                    FillRect(hdc, &thumbRect, hThumb);
                    DeleteObject(hThumb);
                }

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

            case WM_MOUSEWHEEL: {
                int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                m_scrollOffsetY -= (delta / WHEEL_DELTA) * static_cast<int>(60 * app.dpiScale);
                InvalidateRect(m_hWnd, nullptr, FALSE);
                return 0;
            }

            case WM_LBUTTONDOWN: {
                POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

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

                if (pt.x >= sbX && pt.x < sbX + sbWidth) {
                    m_sbDragging = true;
                    m_sbDragStartY = pt.y;
                    m_sbDragStartOffset = m_scrollOffsetY;
                    SetCapture(m_hWnd);
                }
                return 0;
            }

            case WM_MOUSEMOVE: {
                POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

                if (pt.x >= m_footerLinkRect.left && pt.x <= m_footerLinkRect.right &&
                    pt.y >= m_footerLinkRect.top && pt.y <= m_footerLinkRect.bottom) {
                    SetCursor(LoadCursor(nullptr, IDC_HAND));
                } else {
                    SetCursor(LoadCursor(nullptr, IDC_ARROW));
                }

                if (m_sbDragging) {
                    const int maxScroll = std::max(0, m_totalContentHeight - m_viewHeight);
                    const int delta = pt.y - m_sbDragStartY;
                    if (m_viewHeight > 0)
                        m_scrollOffsetY = m_sbDragStartOffset + delta * maxScroll / m_viewHeight;
                    InvalidateRect(m_hWnd, nullptr, FALSE);
                }
                return 0;
            }

            case WM_LBUTTONUP: {
                if (m_sbDragging) {
                    m_sbDragging = false;
                    ReleaseCapture();
                }
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
