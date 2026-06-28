#include "../WicDecoder.h"
#include "AppState.h"
#include "../WorkerThread.h"
#include "Constants.h"

using Microsoft::WRL::ComPtr;

constexpr UINT_PTR TIMER_LOOKASIDE = 1001;

void LoadImageIndex(HWND hWnd, int index) {
    g_decoderWorker.ClearQueue();

    if (index < 0 || index >= static_cast<int>(g_app.playlist.size())) return;

    if (g_app.currentIndex != index) {
        g_app.viewport = ViewportState{};
    }

    g_app.currentIndex = index;

    // --- ATOMIC SYNC ---
    // Broadcast the new target to the background thread instantly
    g_app.wantedIndex.store(index, std::memory_order_release);

    const std::wstring &currentPath = g_app.playlist[index];

    std::wstring fileName = currentPath.substr(currentPath.find_last_of(L"\\/") + 1);
    std::wstring windowTitle = fileName + L" - QuickImageViewer";
    SetWindowTextW(hWnd, windowTitle.c_str());

    if (g_app.renderer && SUCCEEDED(g_app.renderer->LoadBitmap(nullptr, 0, 0, currentPath))) {
        InvalidateRect(hWnd, nullptr, FALSE);
        SetTimer(hWnd, TIMER_LOOKASIDE, Constants::PRELOAD_TIMER_COUNTDOWN, nullptr);
        return;
    }

    // --- CACHE MISS: The Cancelable Target ---
    g_decoderWorker.PushTask([currentPath, index, hWnd]() {
        // CANCELLATION CHECK: Did the user scroll before this task woke up?
        if (g_app.wantedIndex.load(std::memory_order_acquire) != index) {
            return; // ABORT INSTANTLY. Do not waste CPU cycles.
        }

        if (g_app.renderer) {
            (void) g_app.renderer->PreloadBitmap(currentPath, index);
            PostMessageW(hWnd, Constants::WM_QIV_PENDING_UPLOADS, 0, 0);
        }
    });

    SetTimer(hWnd, TIMER_LOOKASIDE, Constants::PRELOAD_TIMER_COUNTDOWN, nullptr);
}
