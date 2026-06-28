#include "../WicDecoder.h"
#include "AppState.h"
#include "../WorkerThread.h"
#include "Constants.h"

using Microsoft::WRL::ComPtr;

constexpr UINT_PTR TIMER_LOOKASIDE = 1001;

void LoadImageIndex(HWND hWnd, int index) {
    // --- 1. NUKE THE BACKLOG ---
    // Instantly abort any background decodes for images we scrolled past
    g_decoderWorker.ClearQueue();

    if (index < 0 || index >= static_cast<int>(g_app.playlist.size())) return;

    if (g_app.currentIndex != index) {
        g_app.viewport = ViewportState{};
    }

    g_app.currentIndex = index;
    const std::wstring &currentPath = g_app.playlist[g_app.currentIndex];

    // 1. Update the Window Title instantly
    std::wstring fileName = currentPath.substr(currentPath.find_last_of(L"\\/") + 1);
    std::wstring windowTitle = fileName + L" - QuickImageViewer";
    SetWindowTextW(hWnd, windowTitle.c_str());

    // 2. Probe the Cache (Zero-Disk I/O)
    if (g_app.renderer && SUCCEEDED(g_app.renderer->LoadBitmap(nullptr, 0, 0, currentPath))) {
        InvalidateRect(hWnd, nullptr, FALSE);

        // Even on a cache hit, refresh the lookaside timer to keep predictive memory warm
        SetTimer(hWnd, TIMER_LOOKASIDE, Constants::PRELOAD_TIMER_COUNTDOWN, nullptr);
        return;
    }

    // --- 3. CACHE MISS: The Flipbook Target ---
    // Push ONLY the explicitly requested image to guarantee 1-to-1 sync with the mouse wheel
    g_decoderWorker.PushTask([currentPath, hWnd]() {
        if (g_app.renderer) {
            (void) g_app.renderer->PreloadBitmap(currentPath);
            PostMessageW(hWnd, Constants::WM_QIV_PENDING_UPLOADS, 0, 0);
        }
    });

    // --- 4. DEBOUNCE LOOKASIDES ---
    // Wait for the scroll wheel to pause for 150ms before pushing predictive tasks.
    // If you keep scrolling, this resets, preventing the 50-task queue explosion.
    SetTimer(hWnd, TIMER_LOOKASIDE, Constants::PRELOAD_TIMER_COUNTDOWN, nullptr);
}
