#include "../WicDecoder.h"
#include "AppState.h"
#include "../WorkerThread.h"
#include "Platform/Constants.h"

using Microsoft::WRL::ComPtr;

constexpr UINT_PTR TIMER_LOOKASIDE = 1001;

void LoadImageIndex(HWND hWnd, int index) {
    // 1. Flush any pending decoding work from the previous state
    g_decoderWorker.ClearQueue();
    g_ioWorker.ClearQueue();

    if (index < 0 || index >= static_cast<int>(g_app.playlist.size())) return;

    // Reset viewport on index change
    if (g_app.currentIndex != index) {
        g_app.viewport = ViewportState{};
    }

    g_app.currentIndex = index;

    // 2. Synchronize target for background workers
    g_app.wantedIndex.store(index, std::memory_order_release);

    const std::wstring &currentPath = g_app.playlist[index];

    // Update window title
    std::wstring fileName = currentPath.substr(currentPath.find_last_of(L"\\/") + 1);
    std::wstring windowTitle = fileName + L" - QuickImageViewer";
    SetWindowTextW(hWnd, windowTitle.c_str());

    // 3. Cache Probe: If already in VRAM, do not hit the disk
    if (g_app.renderer && SUCCEEDED(g_app.renderer->LoadBitmap(nullptr, 0, 0, currentPath))) {
        InvalidateRect(hWnd, nullptr, FALSE);
        SetTimer(hWnd, TIMER_LOOKASIDE, Constants::PRELOAD_TIMER_COUNTDOWN, nullptr);
        return;
    }

    // 4. Split I/O and Decode Pipeline (Cache Miss)
    g_ioWorker.PushTask([currentPath, index, hWnd]() {
        // Cooperative Cancellation: Abort if user scrolled past this
        if (g_app.wantedIndex.load(std::memory_order_acquire) != index) return;

        // Perform I/O priming and hand off to decoder
        if (g_app.renderer) {
            (void) g_app.renderer->PreloadBitmap(currentPath, index);
        }
    });

    SetTimer(hWnd, TIMER_LOOKASIDE, Constants::PRELOAD_TIMER_COUNTDOWN, nullptr);
}
