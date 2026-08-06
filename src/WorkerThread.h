// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#pragma once

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <functional>
#include <atomic>
#include <vector>


// A CEILING ON QUEUED WORK, not a throttle. Shared by both pools below.
//
// Neither queue was bounded, so rapid navigation, a folder change or a large
// thumbnail sweep could accumulate std::function objects without limit.
// Cancellation happens at a higher level and stops the WORK, but the queued
// closures stay allocated until something pops them.
//
// Set far above any legitimate burst — a visible thumbnail range is tens of
// items, not thousands — so it never fires in normal use. It exists to bound the
// pathological case, and 16k closures is a couple of megabytes.
//
// WHY REJECTION IS REPORTED RATHER THAN THE TASK SILENTLY DROPPED. Callers mark
// work as in-flight BEFORE pushing (RendererD2D's m_bitmapInFlight, and the
// per-panel inFlight sets) and clear that mark INSIDE the task. A dropped task
// would leave the mark set forever, so the image or thumbnail it guards could
// never be requested again — a permanently blank tile, which is worse than the
// unbounded queue this replaces. PushTask therefore returns bool, and any caller
// holding such bookkeeping must undo it when the answer is false.
inline constexpr size_t QIV_WORKER_MAX_QUEUED = 16384;


class DecoderThreadPool {
    public:
        DecoderThreadPool() : m_running(true) {}

        ~DecoderThreadPool() {
            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                m_running = false;
            }
            m_cv.notify_all();
            for (auto &t: m_threads) {
                if (t.joinable()) t.join();
            }
        }

        // Returns FALSE when the task was not queued — pool stopped, or the queue
        // is at capacity. See MAX_QUEUED in DecoderThreadPool for why the ceiling
        // exists and why rejection is reported instead of dropping silently.
        bool PushTask(std::function<void(IWICImagingFactory2 *)> task) {
            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                if (!m_running) return false;
                if (m_queue.size() >= QIV_WORKER_MAX_QUEUED) return false;
                m_queue.push(std::move(task));
            }
            m_cv.notify_one();
            return true;
        }

        void ClearQueue() {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            std::queue<std::function<void(IWICImagingFactory2 *)> > empty;
            std::swap(m_queue, empty);
        }

        void setThreadCount(int threads) {
            if (!m_threads.empty()) {
                clearOldThreads();
            }
            //reset running flag
            m_running = true;
            // Safety: force at least 1 thread
            int threadCount = (threads > 0) ? threads : 1;

            m_threads.reserve(threadCount);
            for (int i = 0; i < threadCount; ++i) {
                m_threads.emplace_back([this] {
                    // 1. Thread priority optimization
                    // Setting this here ensures the worker threads don't starve the UI thread
                    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                    // Reserve stack for the crash filter, per thread.
                    //
                    // SetThreadStackGuarantee applies to the CALLING thread only, so
                    // setting it in wWinMain covers the UI thread and nothing else. A
                    // worker that overflows its stack then faults again inside the
                    // handler and dies with no dump — and decode recursion on a
                    // malformed image is exactly how that happens. Every crash seen
                    // during development so far has been on a worker, not the UI
                    // thread, which is the argument for spending 64 KB here.
                    { ULONG guard = 64 * 1024; SetThreadStackGuarantee(&guard); }
                    // 2. COM Init
                    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

                    Microsoft::WRL::ComPtr<IWICImagingFactory2> localFactory;
                    if (SUCCEEDED(hr)) {
                        CoCreateInstance(CLSID_WICImagingFactory2, nullptr,
                                         CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&localFactory));
                    }

                    while (true) {
                        std::function<void(IWICImagingFactory2 *)> task;
                        {
                            std::unique_lock<std::mutex> lock(m_queueMutex);
                            m_cv.wait(lock, [this] {
                                return !m_queue.empty() || !m_running;
                            });
                            if (!m_running && m_queue.empty()) break;
                            task = std::move(m_queue.front());
                            m_queue.pop();
                        }

                        if (task && localFactory) {
                            // A BAD PICTURE MUST NOT TAKE THE PROCESS DOWN.
                            // Every buffer in SimpleFormats is sized from header
                            // fields of a file this app did not write — a QOI
                            // header alone can legitimately ask for 1.6 GB — and
                            // std::vector answers an impossible request with
                            // std::bad_alloc. An exception leaving a
                            // std::thread's function calls std::terminate: no
                            // dump, no message, the whole viewer gone because
                            // one file was malformed.
                            //
                            // Swallowed rather than reported, because the task
                            // already reports for itself: RendererD2D's
                            // FailureReporter is a destructor, so it runs during
                            // unwinding and the user is told that this file
                            // failed to decode before we ever get here.
                            try {
                                task(localFactory.Get());
                            } catch (...) {
                            }
                        }
                    }

                    localFactory.Reset();
                    if (SUCCEEDED(hr)) CoUninitialize();
                });
            }
        }

        int getThreadCount() {
            return static_cast<int>(m_threads.size());
        }

        // NO PUBLIC Shutdown() — deliberately.
        //
        // One existed briefly, to let a duplicate instance stop this pool before
        // returning from wWinMain: the GeoNames warm-up queued at startup was
        // still parsing into a file-scope unordered_map when the CRT destroyed
        // it, and joining first was the obvious patch.
        //
        // The real fix was to stop starting the pool at all in a process that is
        // about to exit — the single-instance check now runs before anything here
        // is touched (see AppMain.cpp). With no threads started there is nothing
        // to shut down, so the method became dead code and was removed rather
        // than left as an invitation to reintroduce the ordering it papered over.
        //
        // The destructor still joins, which is correct for the surviving instance.

        size_t PendingTaskCount() {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            return m_queue.size();
        }

    private:
        std::vector<std::thread> m_threads;
        std::mutex m_queueMutex;
        std::condition_variable m_cv;
        std::queue<std::function<void(IWICImagingFactory2 *)> > m_queue;
        std::atomic<bool> m_running;

        void clearOldThreads() {
            // 1. Signal threads to stop
            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                m_running = false;
            }
            m_cv.notify_all();

            // 2. Wait for current threads to exit (crucial)
            for (auto &t: m_threads) {
                if (t.joinable()) t.join();
            }
            m_threads.clear();
        }
};


// ---------------------------------------------------------------------------
// IoThreadPool  —  N threads sharing one queue, no WIC factory
// ---------------------------------------------------------------------------
// Constructed dormant (0 threads). Call Start(n) once the target drive path
// is known so thread count can be chosen based on drive type:
//
//   HDD  (seek penalty)  → Start(1)
//     Single thread preserves the physical disk-order sort.
//     Multiple concurrent requests would cause head thrashing.
//
//   SSD / NVMe           → Start(2)
//     Two threads let file-open latency overlap with the previous decode.
//     The NVMe controller can service both from its deep command queue.
//     More than 2 gives negligible gain for image-sized payloads.
//
// Tasks pushed before Start() are queued and drain immediately once
// Start() is called — no tasks are lost.
// ---------------------------------------------------------------------------
class IoThreadPool {
    public:
        // Construct dormant — no threads yet, queue accepts tasks safely
        IoThreadPool()
            : m_running(false) {}

        ~IoThreadPool() {
            Stop();
        }

        // Call once, after the first folder path is known.
        // Safe to call multiple times — subsequent calls are no-ops.
        void Start(size_t threadCount) {
            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                if (m_running) return; // already started
                m_running = true;
            }

            m_threads.reserve(threadCount);
            for (size_t i = 0; i < threadCount; ++i) {
                m_threads.emplace_back([this] {
                    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                    // Reserve stack for the crash filter, per thread.
                    //
                    // SetThreadStackGuarantee applies to the CALLING thread only, so
                    // setting it in wWinMain covers the UI thread and nothing else. A
                    // worker that overflows its stack then faults again inside the
                    // handler and dies with no dump — and decode recursion on a
                    // malformed image is exactly how that happens. Every crash seen
                    // during development so far has been on a worker, not the UI
                    // thread, which is the argument for spending 64 KB here.
                    { ULONG guard = 64 * 1024; SetThreadStackGuarantee(&guard); }
                    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                    if (FAILED(hr))
                        OutputDebugStringW(L"IoThreadPool: COM init failed\n");

                    while (true) {
                        std::function<void()> task;
                        {
                            std::unique_lock<std::mutex> lock(m_queueMutex);
                            m_cv.wait(lock, [this] {
                                return !m_queue.empty() || !m_running;
                            });
                            if (!m_running && m_queue.empty()) break;
                            task = std::move(m_queue.front());
                            m_queue.pop();
                        }
                        // Same reason as DecoderThreadPool above: an exception
                        // escaping a std::thread's function is std::terminate,
                        // and these tasks read files whose size is decided by
                        // whatever is on disk.
                        if (task) {
                            try {
                                task();
                            } catch (...) {
                            }
                        }
                    }

                    if (SUCCEEDED(hr)) CoUninitialize();
                });
            }

            // Wake all new threads — there may already be queued tasks
            m_cv.notify_all();
        }

        void Stop() {
            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                m_running = false;
            }
            m_cv.notify_all();
            for (auto &t: m_threads)
                if (t.joinable()) t.join();
            m_threads.clear();
        }

        // Returns FALSE when the task was not queued. Callers holding in-flight
        // bookkeeping MUST undo it on false — see QIV_WORKER_MAX_QUEUED.
        bool PushTask(std::function<void()> task) {
            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                // Accept tasks even before Start() — they drain once threads are up
                if (m_queue.size() >= QIV_WORKER_MAX_QUEUED) return false;
                m_queue.push(std::move(task));
            }
            m_cv.notify_one();
            return true;
        }

        void ClearQueue() {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            std::queue<std::function<void()> > empty;
            std::swap(m_queue, empty);
        }

        bool IsStarted() const { return m_running.load(); }

        int getThreadCount() const { return static_cast<int>(m_threads.size()); }

        size_t PendingTaskCount() {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            return m_queue.size();
        }

    private:
        std::vector<std::thread> m_threads;
        std::mutex m_queueMutex;
        std::condition_variable m_cv;
        std::queue<std::function<void()> > m_queue;
        std::atomic<bool> m_running;
};


// ---------------------------------------------------------------------------
// Role assignment
// ---------------------------------------------------------------------------
// g_ioWorker      – IoThreadPool: file open + CreateDecoderFromFilename
//                   Started with 1 thread (HDD) or 2 threads (SSD/NVMe)
//                   after the first folder path is detected at runtime.
//                   No WIC factory — tasks receive it via lambda capture.
//
// g_decoderWorker – WorkerThread(true): WIC frame decode + pixel conversion
//                   Single thread: WIC conversion is CPU-bound, one thread
//                   per logical pipeline is optimal.
//
// Render thread   – UI thread only: Direct2D GPU upload + draw
//                   ProcessPendingUploads() on WM_QIV_PENDING_UPLOADS.
// ---------------------------------------------------------------------------
extern IoThreadPool g_ioWorker;
extern DecoderThreadPool g_decoderWorker;
extern DecoderThreadPool g_dirThumbWorker;
