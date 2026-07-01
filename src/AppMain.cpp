#include <algorithm>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <dwmapi.h>
#include <intsafe.h>
#include <uxtheme.h>
#include "CacheWindow.h"
#include "../AppState.h"
#include "Platform/Constants.h"
#include "Platform/Shortcuts.h"
#include "../DropTarget.h"
#include "Platform/FileHandler.h"
#include "UI/HelpWindow.h"

#include "Platform/MouseHandler.h"
#include "../WicDecoder.h"
#include "../SvgDecoder.h"

#include <windows.h>
#include <windowsx.h>

#include <shellapi.h> // Parsing command line arguments
#include <string>     // Handling string paths
#include <memory>     // Needed for std::unique_ptr for renderer management

#include "Platform/DpiAwareInit.h"

#include "../resources/resource.h"
#include "Platform/RegistrySetup.h"

#include "Renderer/RendererD2D.h"
#include "Renderer/RendererGDI.h"
#include "WorkerThread.h"
#include <shlobj.h>   // Required for SHOpenFolderAndSelectItems
#include <commdlg.h>  // GetSaveFileName dialog

// Global application state
AppState g_app;
DropTarget *g_pDropTarget = nullptr;

// Define the storage for the globals exactly once in your entry point file
//   g_ioWorker      – IoThreadPool: started lazily in FileHandler once the
//                     target drive is known (1 thread HDD, 2 threads SSD/NVMe)
//   g_decoderWorker – WorkerThread(true): WIC decode + pixel convert
IoThreadPool g_ioWorker;
WorkerThread g_decoderWorker(true);

static void ToggleFullscreen(HWND hWnd) {
    if (!g_app.isFullscreen) {
        GetWindowRect(hWnd, &g_app.savedWindowRect);
        MONITORINFO mi = {sizeof(mi)};
        GetMonitorInfo(MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST), &mi);

        SetWindowPos(hWnd, HWND_TOPMOST,
                     mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOCOPYBITS);

        DWMNCRENDERINGPOLICY policy = DWMNCRP_DISABLED;
        DwmSetWindowAttribute(hWnd, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));
        DWORD corner = 1; // DWMWCP_DONOTROUND
        DwmSetWindowAttribute(hWnd, Constants::DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
        MARGINS margins = {0, 0, 0, 0};
        DwmExtendFrameIntoClientArea(hWnd, &margins);

        g_app.isFullscreen = true;
    } else {
        SetWindowPos(hWnd, HWND_NOTOPMOST,
                     g_app.savedWindowRect.left,
                     g_app.savedWindowRect.top,
                     g_app.savedWindowRect.right - g_app.savedWindowRect.left,
                     g_app.savedWindowRect.bottom - g_app.savedWindowRect.top,
                     SWP_FRAMECHANGED | SWP_NOCOPYBITS);

        DWMNCRENDERINGPOLICY policy = DWMNCRP_ENABLED;
        DwmSetWindowAttribute(hWnd, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));
        DWORD corner = 2; // DWMWCP_ROUND
        DwmSetWindowAttribute(hWnd, Constants::DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
        MARGINS margins = {1, 1, 1, 1};
        DwmExtendFrameIntoClientArea(hWnd, &margins);

        g_app.isFullscreen = false;
    }
}


// Shift+Delete (Shortcuts::SC_APP_RESET_DEFAULTS) — restore default application
// state: window size/position centered on the current monitor, zoom/pan/
// rotation/flip/opacity reset, and every image effect cleared.
static void ResetWindowLayoutAndEffects(HWND hWnd) {
    // --- Viewport / window ---
    g_app.ResetWindowState(hWnd);

    // --- All image effects ---
    g_app.ResetEffects();

    g_app.UpdateRendererColorEffects(hWnd);
}

// Builds the output path for Ctrl+S (Shortcuts::ImageEffects::SC_COLOR_SAVE_TO_DISK):
// same folder, "<name>_edited.png" — never overwrites the source file.
static std::wstring BuildEffectsOutputPath(const std::wstring &srcPath) {
    size_t dot = srcPath.find_last_of(L'.');
    size_t slash = srcPath.find_last_of(L"\\/");
    std::wstring base = (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash))
                            ? srcPath.substr(0, dot)
                            : srcPath;
    return base + L"_edited.png";
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_DPICHANGED: {
            // 1. Update your global scale
            g_app.dpiScale = static_cast<float>(HIWORD(wParam)) / 96.0f;

            // 2. Refresh the Renderer's font format
            g_app.renderer->UpdateTextFormat();
            RECT *const prcNewWindow = (RECT *) lParam;
            SetWindowPos(hWnd,
                         nullptr,
                         prcNewWindow->left,
                         prcNewWindow->top,
                         prcNewWindow->right - prcNewWindow->left,
                         prcNewWindow->bottom - prcNewWindow->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }

        // Handle file paths sent from other instances of the viewer
        case WM_COPYDATA: {
            COPYDATASTRUCT *cds = (COPYDATASTRUCT *) lParam;
            if (cds->dwData == 1) {
                // Create a local copy to ensure safety even if processing takes time
                std::wstring safePath((LPCWSTR) cds->lpData);

                // --- 1. LOAD THE NEW IMAGE ---
                OpenSpecificImage(hWnd, safePath.c_str());

                // --- 2. WAKE UP & CENTER ---
                // (viewport already reset inside LoadImageIndex via OpenSpecificImage)
                ShowWindow(hWnd, SW_RESTORE);
                SetForegroundWindow(hWnd); // Bring to front
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            return TRUE;
        }
        case WM_TIMER: {
            constexpr UINT_PTR TIMER_LOOKASIDE = 1001;

            if (wParam == TIMER_LOOKASIDE) {
                KillTimer(hWnd, TIMER_LOOKASIDE);

                if (g_app.playlist.empty()) return 0;

                int index = g_app.currentIndex;
                const int total = static_cast<int>(g_app.playlist.size());

                for (int i = 1; i <= Constants::PRELOAD_LOOKASIDE_COUNT; ++i) {
                    int fwd = index + i;
                    int bwd = index - i;

                    if (fwd < total) {
                        std::wstring fwdPath = g_app.playlist[fwd];
                        g_decoderWorker.PushTask([fwdPath, index]() {
                            // ABORT if user started scrolling again
                            if (g_app.wantedIndex.load(std::memory_order_acquire) != index) return;
                            if (g_app.renderer) (void) g_app.renderer->PreloadBitmap(fwdPath, index);
                        });
                    }
                    if (bwd >= 0) {
                        std::wstring bwdPath = g_app.playlist[bwd];
                        g_decoderWorker.PushTask([bwdPath, index]() {
                            // ABORT if user started scrolling again
                            if (g_app.wantedIndex.load(std::memory_order_acquire) != index) return;
                            if (g_app.renderer) (void) g_app.renderer->PreloadBitmap(bwdPath, index);
                        });
                    }
                }
            }
            return 0;
        }
        case WM_KEYDOWN: {
            bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            // Keys '1'–'5'  —  Switch view mode
            if (wParam >= Shortcuts::SC_VIEW_MODE_FIRST && wParam <= Shortcuts::SC_VIEW_MODE_LAST) {
                g_app.viewMode = static_cast<Constants::ViewModes::ViewMode>(wParam - '0');
                InvalidateRect(hWnd, nullptr, FALSE);
                return 0;
            }
            // E / Tab  —  Reveal current file in Windows Explorer
            if (wParam == Shortcuts::SC_NAV_SHOW_IN_EXPLORER || wParam == Shortcuts::SC_NAV_SHOW_IN_EXPLORER_TAB) {
                if (!g_app.playlist.empty() && g_app.currentIndex >= 0) {
                    const std::wstring &path = g_app.playlist[g_app.currentIndex];
                    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path.c_str());
                    if (pidl) {
                        SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
                        ILFree(pidl);
                    }
                }
                return 0;
            }
            // H  —  Flip horizontal
            if (wParam == Shortcuts::SC_TRANSFORM_FLIP_H) {
                g_app.viewport.flippedH = !g_app.viewport.flippedH;
                InvalidateRect(hWnd, nullptr, FALSE);
                return 0;
            }
            // V  —  Flip vertical
            if (wParam == Shortcuts::SC_TRANSFORM_FLIP_V) {
                g_app.viewport.flippedV = !g_app.viewport.flippedV;
                InvalidateRect(hWnd, nullptr, FALSE);
                return 0;
            }
            // ===============================
            // COLOR EFFECTS  — see Shortcuts::ImageEffects (Shortcuts.h is the
            // single source of truth; all keys below must come from there).
            // ===============================
            // HELPER: Auto-wakes the preview toggle when a user adjusts any effect

            // Shift+Delete  —  Full app reset: window layout + all image effects
            if (wParam == Shortcuts::SC_APP_RESET_DEFAULTS && shift) {
                ResetWindowLayoutAndEffects(hWnd);
                return 0;
            }
            if (wParam == Shortcuts::ImageEffects::SC_EFFECT_APPLY_TOGGLE) {
                // Toggle the preview state on or off
                g_app.effectPreviewEnabled = !g_app.effectPreviewEnabled;
                g_app.UpdateRendererColorEffects(hWnd);
                return 0;
            }
            // 1.Delete (no shift)  —  Toggle grayscale
            if (wParam == Shortcuts::ImageEffects::SC_COLOR_GRAYSCALE && !shift) {
                g_app.WakeUpAndApplyEffects(hWnd, g_app.effectGrayscale);
                return 0;
            }

            // 2.Insert  —  Toggle invert colors
            if (wParam == Shortcuts::ImageEffects::SC_COLOR_INVERT) {
                g_app.WakeUpAndApplyEffects(hWnd, g_app.effectInvert);
                return 0;
            }

            // 3.Home  —  Toggle sepia
            if (wParam == Shortcuts::ImageEffects::SC_COLOR_SEPIA) {
                g_app.WakeUpAndApplyEffects(hWnd, g_app.effectSepia);
                return 0;
            }

            // 4.End  —  Toggle solarize
            if (wParam == Shortcuts::ImageEffects::SC_COLOR_SOLARIZE) {
                g_app.WakeUpAndApplyEffects(hWnd, g_app.effectSolarize);
                return 0;
            }

            // 5.Page Up  —  Toggle image outline
            if (wParam == Shortcuts::ImageEffects::SC_COLOR_OUTLINE) {
                g_app.WakeUpAndApplyEffects(hWnd, g_app.effectOutline);
                return 0;
            }

            // 6.Page Down  —  Toggle black & white threshold
            if (wParam == Shortcuts::ImageEffects::SC_COLOR_THRESHOLD) {
                g_app.WakeUpAndApplyEffects(hWnd, g_app.effectThreshold);
                g_app.WakeUpAndApplyEffects(hWnd);
                return 0;
            }

            // 7. '+' (OEM_PLUS)  —  Gamma up   /   '-' (OEM_MINUS)  —  Gamma down
            if (wParam == Shortcuts::ImageEffects::SC_COLOR_GAMMA_UP) {
                g_app.gamma = std::min(Constants::MAX_GAMMA, g_app.gamma + Constants::GAMMA_STEP);
                g_app.WakeUpAndApplyEffects(hWnd);
                return 0;
            }
            if (wParam == Shortcuts::ImageEffects::SC_COLOR_GAMMA_DOWN) {
                g_app.gamma = std::max(Constants::MIN_GAMMA, g_app.gamma - Constants::GAMMA_STEP);
                g_app.WakeUpAndApplyEffects(hWnd);
                return 0;
            }

            // 8. '  —  Brightness up   /   \  —  Brightness down
            if (wParam == Shortcuts::ImageEffects::SC_COLOR_BRIGHTNESS_UP) {
                g_app.brightness = std::clamp(
                        g_app.brightness + Constants::COLOR_ADJUST_STEP,
                        -Constants::MIN_MAX_BRIGHTNESS, Constants::MIN_MAX_BRIGHTNESS);
                g_app.WakeUpAndApplyEffects(hWnd);
                return 0;
            }
            if (wParam == Shortcuts::ImageEffects::SC_COLOR_BRIGHTNESS_DOWN) {
                g_app.brightness = std::clamp(
                        g_app.brightness - Constants::COLOR_ADJUST_STEP,
                        -Constants::MIN_MAX_BRIGHTNESS, Constants::MIN_MAX_BRIGHTNESS);
                g_app.WakeUpAndApplyEffects(hWnd);

                return 0;
            }

            // 9. .  —  Contrast up   /   /  —  Contrast down
            if (wParam == Shortcuts::ImageEffects::SC_COLOR_CONTRAST_UP) {
                g_app.contrast = std::clamp(
                        g_app.contrast + Constants::COLOR_ADJUST_STEP,
                        0.0f, Constants::MIN_MAX_CONTRAST);
                g_app.WakeUpAndApplyEffects(hWnd);

                return 0;
            }
            if (wParam == Shortcuts::ImageEffects::SC_COLOR_CONTRAST_DOWN) {
                g_app.contrast = std::clamp(
                        g_app.contrast - Constants::COLOR_ADJUST_STEP,
                        0.0f, Constants::MIN_MAX_CONTRAST);
                g_app.WakeUpAndApplyEffects(hWnd);

                return 0;
            }

            // 10. [  —  Saturation -   /   ]  —  Saturation +
            if (wParam == Shortcuts::ImageEffects::SC_COLOR_SAT_DOWN) {
                g_app.saturation = std::max(0.0f, g_app.saturation - Constants::COLOR_ADJUST_STEP);
                g_app.WakeUpAndApplyEffects(hWnd);
                return 0;
            }
            if (wParam == Shortcuts::ImageEffects::SC_COLOR_SAT_UP) {
                g_app.saturation = std::min(
                        Constants::MIN_MAX_SATURATION, g_app.saturation + Constants::COLOR_ADJUST_STEP);
                g_app.WakeUpAndApplyEffects(hWnd);
                return 0;
            }

            // Numpad0  —  Reset all color effects only (saturation/brightness/
            // contrast/gamma + every toggle), leaves window/zoom/pan untouched.
            if (wParam == Shortcuts::ImageEffects::SC_COLOR_RESET_ALL_EFFECTS) {
                g_app.ResetEffects();
                g_app.UpdateRendererColorEffects(hWnd);
                return 0;
            }

            // Ctrl+S  —  Save current image with effects applied to disk.
            // Opens a standard Save dialog so the user picks the output path.
            // Only PNG output is supported (effects are baked at native pixel size).
            if (wParam == Shortcuts::ImageEffects::SC_COLOR_SAVE_TO_DISK && ctrl) {
                if (g_app.renderer && !g_app.playlist.empty() && g_app.currentIndex >= 0) {
                    const std::wstring &srcPath = g_app.playlist[g_app.currentIndex];

                    // Build a default filename: original name (no extension) + "_edited.png"
                    std::wstring defaultName;
                    {
                        size_t slash = srcPath.find_last_of(L"\\/");
                        std::wstring nameOnly = (slash != std::wstring::npos)
                                                    ? srcPath.substr(slash + 1)
                                                    : srcPath;
                        // Strip extension from display name
                        size_t dotInName = nameOnly.find_last_of(L'.');
                        if (dotInName != std::wstring::npos)
                            nameOnly = nameOnly.substr(0, dotInName);
                        defaultName = nameOnly + L"_edited.png";
                    }

                    // Buffer for the dialog — must be large enough for long paths
                    wchar_t outBuf[MAX_PATH * 2] = {};
                    wcsncpy_s(outBuf, defaultName.c_str(), _TRUNCATE);

                    OPENFILENAMEW ofn{};
                    ofn.lStructSize = sizeof(ofn);
                    ofn.hwndOwner = hWnd;
                    ofn.lpstrFilter = L"PNG Image\0*.png\0All Files\0*.*\0";
                    ofn.nFilterIndex = 1;
                    ofn.lpstrFile = outBuf;
                    ofn.nMaxFile = ARRAYSIZE(outBuf);
                    ofn.lpstrDefExt = L"png";
                    ofn.lpstrTitle = L"Save image with effects";
                    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

                    // Set initial directory to the source image's folder
                    wchar_t initDir[MAX_PATH] = {};
                    {
                        size_t slash = srcPath.find_last_of(L"\\/");
                        if (slash != std::wstring::npos)
                            wcsncpy_s(initDir, srcPath.substr(0, slash).c_str(), _TRUNCATE);
                    }
                    ofn.lpstrInitialDir = initDir;

                    if (GetSaveFileNameW(&ofn)) {
                        HRESULT hr = g_app.renderer->SaveCurrentImageWithEffects(outBuf);
                        if (FAILED(hr)) {
                            wchar_t errBuf[128];
                            swprintf_s(errBuf, L"Failed to save image.\nHRESULT: 0x%08X", static_cast<unsigned>(hr));
                            MessageBoxW(hWnd, errBuf, L"QuickImageViewer", MB_OK | MB_ICONERROR);
                        }
                    }
                }
                return 0;
            }

            // Rotate Image: R (Clockwise) or Shift+R (Counter-Clockwise)
            if (wParam == Shortcuts::SC_TRANSFORM_ROTATE) {
                if (shift) {
                    // Counter-Clockwise
                    g_app.viewport.rotation = (g_app.viewport.rotation - 90 + 360) % 360;
                } else {
                    // Clockwise
                    g_app.viewport.rotation = (g_app.viewport.rotation + 90) % 360;
                }
                InvalidateRect(hWnd, nullptr, FALSE);
                return 0;
            }
            // Ctrl+Q  —  Hard quit: flush from RAM completely
            if (wParam == Shortcuts::SC_APP_HARD_QUIT && ctrl) {
                PostQuitMessage(0);
                return 0;
            }
            // Toggle Help Menu (F1)
            if (wParam == Shortcuts::SC_PANEL_HELP_TOGGLE) {
                UI::ToggleHelpWindow();
                return 0;
            }
            // Open File Dialog (F2)
            if (wParam == Shortcuts::SC_PANEL_OPEN_FILE) {
                OpenInitialImage(hWnd);
                return 0;
            }
            // Toggle Cache panel (F3)
            if (wParam == Shortcuts::SC_PANEL_CACHE_TOGGLE) {
                UI::ToggleCacheWindow();
                return 0;
            }
            // Clear VRAM Cache and UI (F12)
            if (wParam == Shortcuts::SC_PANEL_CACHE_CLEAR) {
                UI::ClearThumbnailCache();
                return 0;
            }
            // // Toggle Dir panel (F5)
            // if (wParam == Shortcuts::SC_PANEL_DIR_TOGGLE) {
            //     UI::ToggleDirWindow();
            //     return 0;
            // }
            // Hide to RAM instead of quitting (Esc or Ctrl + W)
            // Esc / Ctrl+W  —  Hide to RAM
            if (wParam == Shortcuts::SC_APP_HIDE || (wParam == Shortcuts::SC_APP_HIDE_ALT && ctrl)) {
                if (g_app.GetInstanceCount() <= 1) {
                    ShowWindow(hWnd, SW_HIDE);
                } else {
                    // This is a disposable instance: kill it completely
                    PostQuitMessage(0);
                }
                return 0;
            }
            // N  —  Toggle on-screen info text overlay
            if (wParam == Shortcuts::SC_PANEL_OVERLAY_TOGGLE && !ctrl) {
                g_app.showOverlayInfoText = !g_app.showOverlayInfoText;
                InvalidateRect(hWnd, nullptr, FALSE);
                return 0;
            }
            // Ctrl+N  —  Open a new blank QIV window
            if (wParam == Shortcuts::SC_APP_NEW_WINDOW && ctrl) {
                wchar_t exePath[MAX_PATH];
                GetModuleFileNameW(nullptr, exePath, MAX_PATH);

                SetEnvironmentVariableW(L"QIV_NEW_INSTANCE", L"1");
                ShellExecuteW(nullptr, L"open", exePath, nullptr, nullptr, SW_SHOW);
                SetEnvironmentVariableW(L"QIV_NEW_INSTANCE", nullptr);
                return 0;
            }

            // Fullscreen toggle: F, F11, Enter, Ctrl+Shift+T
            if (wParam == Shortcuts::SC_PANEL_FULLSCREEN ||
                wParam == Shortcuts::SC_PANEL_FULLSCREEN_F ||
                wParam == Shortcuts::SC_PANEL_FULLSCREEN_ENTER ||
                (wParam == Shortcuts::SC_PANEL_FULLSCREEN_T && ctrl && shift)) {
                ToggleFullscreen(hWnd);
                InvalidateRect(hWnd, nullptr, FALSE);
                return 0;
            }

            // Navigation + zoom
            if (!g_app.playlist.empty()) {
                // Cache size as int once to avoid repeated size_t->int casts below
                const int playlistSize = static_cast<int>(g_app.playlist.size());
                switch (wParam) {
                    case Shortcuts::SC_NAV_PREV:
                    case Shortcuts::SC_NAV_PREV_A:
                        LoadImageIndex(hWnd, (g_app.currentIndex - 1 + playlistSize) % playlistSize);
                        InvalidateRect(hWnd, nullptr, FALSE);
                        break;
                    case Shortcuts::SC_NAV_NEXT:
                    case Shortcuts::SC_NAV_NEXT_A:
                        LoadImageIndex(hWnd, (g_app.currentIndex + 1) % playlistSize);
                        InvalidateRect(hWnd, nullptr, FALSE);
                        break;
                    case Shortcuts::SC_NAV_NEXT_SPACE:
                        if (shift)
                            LoadImageIndex(hWnd, (g_app.currentIndex - 1 + playlistSize) % playlistSize);
                        else
                            LoadImageIndex(hWnd, (g_app.currentIndex + 1) % playlistSize);
                        InvalidateRect(hWnd, nullptr, FALSE);
                        break;

                    case Shortcuts::SC_ZOOM_IN_NUMPAD:
                        g_app.viewport.zoom *= Constants::ZOOM_STEP;
                        InvalidateRect(hWnd, nullptr, FALSE);
                        break;

                    case Shortcuts::SC_ZOOM_OUT_NUMPAD:
                        g_app.viewport.zoom /= Constants::ZOOM_STEP;
                        InvalidateRect(hWnd, nullptr, FALSE);
                        break;
                    case Shortcuts::SC_ZOOM_RESET:
                        g_app.viewport.zoom = 1.0f;
                        g_app.viewport.offsetX = 0.0f;
                        g_app.viewport.offsetY = 0.0f;
                        InvalidateRect(hWnd, nullptr, FALSE);
                        break;
                }
            }
            return 0;
        }

        case WM_NCACTIVATE:
            return TRUE;

        case WM_NCHITTEST: {
            if (g_app.isFullscreen) return HTCLIENT;

            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            RECT rc;
            GetWindowRect(hWnd, &rc);

            // Scale the border by the DPI factor
            // Using int to maintain pixel alignment
            const int border = static_cast<int>(2 * g_app.dpiScale);

            bool top = pt.y < rc.top + border;
            bool bottom = pt.y >= rc.bottom - border;
            bool left = pt.x < rc.left + border;
            bool right = pt.x >= rc.right - border;

            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;

            return HTCLIENT;
        }

        // Window size changed: Update renderer
        case WM_SIZE:
            if (g_app.renderer) {
                g_app.renderer->Resize(LOWORD(lParam), HIWORD(lParam));
            }
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        case WM_SIZING:
            if (g_app.renderer) {
                RECT *r = (RECT *) lParam;
                g_app.renderer->Resize(r->right - r->left, r->bottom - r->top);
            }
            InvalidateRect(hWnd, nullptr, FALSE);
            return TRUE;

        // --- CLEAN MOUSE HANDLERS ---
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
            MouseHandler::HandleButtonDown(hWnd, message, lParam);
            return 0;

        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
            MouseHandler::HandleButtonUp(hWnd, message, lParam);
            return 0;

        case WM_MOUSEMOVE:
            MouseHandler::HandleMouseMove(hWnd, lParam);
            return 0;

        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
            if (message == WM_MBUTTONDOWN) MouseHandler::HandleButtonDown(hWnd, message, lParam);
            else MouseHandler::HandleButtonUp(hWnd, message, lParam);
            return 0;

        case WM_MOUSEWHEEL: {
            bool isShiftDown = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

            if (isShiftDown) {
                int delta = GET_WHEEL_DELTA_WPARAM(wParam);

                if (delta > 0) {
                    g_app.opacity = static_cast<BYTE>(
                        (g_app.opacity + Constants::OPACITY_STEP > 255)
                            ? 255
                            : (g_app.opacity + Constants::OPACITY_STEP));
                } else {
                    g_app.opacity = static_cast<BYTE>(
                        (g_app.opacity - Constants::OPACITY_STEP < 10)
                            ? 10
                            : (g_app.opacity - Constants::OPACITY_STEP));
                }

                SetLayeredWindowAttributes(hWnd, 0, g_app.opacity, LWA_ALPHA);
                return 0;
            }

            MouseHandler::HandleMouseWheel(hWnd, wParam, lParam);
            return 0;
        }

        case WM_MOUSEHWHEEL: {
            bool isRmbDown = (GetKeyState(VK_RBUTTON) & 0x8000) != 0;
            int hDelta = GET_WHEEL_DELTA_WPARAM(wParam);

            if (isRmbDown) {
                RECT rc;
                GetWindowRect(hWnd, &rc);
                int currentW = rc.right - rc.left;
                int currentH = rc.bottom - rc.top;

                int resizeStep = (hDelta > 0) ? 20 : -20;
                int newW = currentW + resizeStep;
                int newH = static_cast<int>(std::round(
                        currentH + resizeStep * (static_cast<float>(currentH) / currentW)));

                int newX = rc.left - (resizeStep / 2);
                int newY = rc.top - (resizeStep / 2);

                SetWindowPos(hWnd, nullptr, newX, newY, newW, newH,
                             SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
                InvalidateRect(hWnd, nullptr, FALSE);
                return 0;
            }

            if (hDelta > 0) {
                g_app.opacity = static_cast<BYTE>(std::min(255, g_app.opacity + Constants::OPACITY_STEP));
            } else {
                g_app.opacity = static_cast<BYTE>(std::max(10, g_app.opacity - Constants::OPACITY_STEP));
            }
            SetLayeredWindowAttributes(hWnd, 0, g_app.opacity, LWA_ALPHA);
            return 0;
        }

        case WM_LBUTTONDBLCLK: {
            SendMessageW(hWnd, WM_SETREDRAW, FALSE, 0);

            ToggleFullscreen(hWnd);

            SendMessageW(hWnd, WM_SETREDRAW, TRUE, 0);
            RedrawWindow(hWnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_FRAME);
            return 0;
        }

        case WM_CAPTURECHANGED: {
            g_app.viewport.isDragging = false;
            g_app.isWindowDragging = false;
            ReleaseCapture();
            return 0;
        }
        case Constants::WM_QIV_REPAINT: {
            // The background thread has finished decoding and caching the bitmap.
            // Now, on the UI thread, we probe the cache to make it the active bitmap.
            if (g_app.renderer && !g_app.playlist.empty()) {
                const std::wstring &currentPath = g_app.playlist[g_app.currentIndex];
                // This call will find the bitmap in the cache and set it as active.
                if (SUCCEEDED(g_app.renderer->LoadBitmap(nullptr, 0, 0, currentPath))) {
                    // --- CALL THE EFFECT UPDATER HERE ---
                    // Now the bitmap is ready, we can safely wire the effect graph.
                    g_app.UpdateRendererColorEffects(hWnd);
                    InvalidateRect(hWnd, nullptr, FALSE); // Now, repaint with the correct image.
                    UI::UpdateCacheView();
                }
            }
            return 0;
        }

        case Constants::WM_QIV_SVG_READY: {
            // wParam = playlist index at the time of the IO request
            // lParam = heap-allocated SvgPayload* (we own it, must delete)
            struct SvgPayload {
                std::wstring path;
                std::vector<BYTE> bytes;
            };
            auto *payload = reinterpret_cast<SvgPayload *>(lParam);
            if (!payload) return 0;

            int arrivedIndex = static_cast<int>(wParam);

            // Discard if user navigated away while bytes were in flight
            if (arrivedIndex == g_app.wantedIndex.load(std::memory_order_acquire) &&
                g_app.renderer) {
                if (SUCCEEDED(g_app.renderer->LoadSvgFromBytes(payload->bytes, payload->path))) {
                    InvalidateRect(hWnd, nullptr, FALSE);
                    UI::UpdateCacheView();
                }
            }

            delete payload;
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hWnd, &ps);
            if (g_app.renderer) {
                (void) g_app.renderer->Render();
            }
            EndPaint(hWnd, &ps);
            return 0;
        }

        case WM_NCCALCSIZE:
            return 0;

        case WM_NCPAINT:
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_CLOSE: {
            if (IsDebuggerPresent()) {
                PostQuitMessage(0);
            } else {
#ifdef DEBUG_BUILD
                PostQuitMessage(0);
#else
                ShowWindow(hWnd, SW_HIDE);
#endif
            }
            return 0;
        }

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hWnd, message, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    // 1. Initialize OLE
    if (FAILED(OleInitialize(nullptr))) return 0;

    // Set DPI awareness
    typedef BOOL (WINAPI
                *SETDPI
            )
            (DPI_AWARENESS_CONTEXT);
    if (HMODULE hU32 = GetModuleHandleW(L"user32.dll")) {
        if (auto setDpi = reinterpret_cast<SETDPI>(GetProcAddress(hU32, "SetProcessDpiAwarenessContext"))) {
            setDpi(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_app.wicFactory)))) return 0;

    System::RegisterAppForOpenWith();
    System::EnableRunOnStartup();

    // --- SINGLE INSTANCE & RAM RESIDENT LOGIC ---
    bool bypassMutex = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (GetEnvironmentVariableW(L"QIV_NEW_INSTANCE", nullptr, 0) > 0) bypassMutex = true;
    std::wstring mutexName = L"QuickImageViewer_SingleInstanceMutex" + (bypassMutex
                                                                            ? std::to_wstring(GetTickCount())
                                                                            : L"");
    HANDLE hMutex = CreateMutexW(NULL, TRUE, mutexName.c_str());
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND hExistingWnd = FindWindowW(Constants::WINDOW_CLASS_NAME, nullptr);
        if (hExistingWnd) {
            int argc;
            LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
            if (argc > 1) {
                COPYDATASTRUCT cds;
                cds.dwData = 1;
                cds.cbData = (DWORD) ((wcslen(argv[1]) + 1) * sizeof(wchar_t));
                cds.lpData = (void *) argv[1];
                SendMessageW(hExistingWnd, WM_COPYDATA, 0, (LPARAM) &cds);
            }
            LocalFree(argv);
        }
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
        return 0;
    }

    WNDCLASSW wc{0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = Constants::WINDOW_CLASS_NAME;
    wc.style = CS_DBLCLKS;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APP_ICON));
    RegisterClassW(&wc);

    HWND hWnd = CreateViewerWindow(hInstance, wc.lpszClassName);
    if (!hWnd) return 1;

    SetWindowLongW(hWnd, GWL_EXSTYLE, GetWindowLongW(hWnd, GWL_EXSTYLE) | WS_EX_LAYERED);
    SetLayeredWindowAttributes(hWnd, 0, g_app.opacity, LWA_ALPHA);
    UINT dpi = GetDpiForWindow(hWnd);
    g_app.dpiScale = static_cast<float>(dpi) / 96.0f;
    g_app.screenW = GetSystemMetrics(SM_CXSCREEN);
    g_app.screenH = GetSystemMetrics(SM_CYSCREEN);

    // Initialize Renderer (D2D with GDI fallback)
    g_app.renderer = std::make_unique<RendererD2D>();
    if (FAILED(g_app.renderer->Initialize(hWnd))) {
        g_app.renderer = std::make_unique<RendererGDI>();
        (void) g_app.renderer->Initialize(hWnd); // GDI init: S_OK always, failure is non-fatal
    }
    // ---  CALLBACK REGISTRATION From IRenderer---
    g_app.renderer->onImageChangedCallback = [](int) {
        // This ensures the rectangle snaps to the actual displayed image index
        UI::SyncSelectionRectangle();
    };
#ifdef _DEBUG
    if (g_app.renderer) {
        if (dynamic_cast<RendererD2D *>(g_app.renderer.get())) {
            OutputDebugStringW(L"RENDERER: Using Direct2D (GPU Acceleration Active)\n");
        } else {
            OutputDebugStringW(L"RENDERER: Using GDI (Fallback Mode)\n");
        }
    }
#endif

    RegisterDragDrop(hWnd, (g_pDropTarget = new DropTarget(hWnd)));
    UI::InitHelpWindow(hInstance, hWnd);
    UI::InitCacheWindow(hInstance, hWnd, Constants::CACHE_WINDOW_POSITION);

    DWORD corner = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(hWnd, Constants::DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

    // Handle startup arguments
    int argc;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc > 1 && std::wstring(argv[1]) == L"-background") ShowWindow(hWnd, SW_HIDE);
    else {
        ShowWindow(hWnd, nCmdShow);
        UpdateWindow(hWnd);
        if (argc > 1) OpenSpecificImage(hWnd, argv[1]);
        else OpenInitialImage(hWnd);
    }
    LocalFree(argv);

    MSG msg{};
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // --- CRITICAL CLEANUP ---
    g_app.renderer.reset();

    if (g_app.wicFactory) {
        g_app.wicFactory.Reset();
    }

    if (g_pDropTarget) {
        RevokeDragDrop(hWnd);
        g_pDropTarget->Release();
        g_pDropTarget = nullptr;
    }

    UnregisterClassW(Constants::WINDOW_CLASS_NAME, hInstance);

    CoUninitialize();
    OleUninitialize();

    if (hMutex) {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
    }

    return static_cast<int>(msg.wParam);
}
