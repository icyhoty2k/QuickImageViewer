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


// ---------------------------------------------------------------------------
// WorkerThread  —  single background thread with a task queue
// ---------------------------------------------------------------------------
// initWic = true  → decode worker: owns IWICImagingFactory2, CPU pixel work
// initWic = false → plain task thread, no WIC
// ---------------------------------------------------------------------------
class WorkerThread {
    public:
        explicit WorkerThread(bool initWic = false)
            : m_running(true) {
            m_thread = std::thread([this, initWic] {
                HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                if (FAILED(hr))
                    OutputDebugStringW(L"WorkerThread: COM init failed\n");

                if (initWic) {
                    HRESULT wicHr = CoCreateInstance(
                            CLSID_WICImagingFactory2, nullptr,
                            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wicFactory));
                    if (FAILED(wicHr))
                        OutputDebugStringW(L"WorkerThread: WIC factory creation failed\n");
                }

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
                    if (task) task();
                }

                wicFactory.Reset();
                if (SUCCEEDED(hr)) CoUninitialize();
            });
        }

        ~WorkerThread() {
            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                m_running = false;
            }
            m_cv.notify_one();
            if (m_thread.joinable()) m_thread.join();
        }

        void PushTask(std::function<void()> task) {
            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                if (!m_running) return;
                m_queue.push(std::move(task));
            }
            m_cv.notify_one();
        }

        void ClearQueue() {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            std::queue<std::function<void()> > empty;
            std::swap(m_queue, empty);
        }

        // Non-null only on the decode worker (initWic == true)
        Microsoft::WRL::ComPtr<IWICImagingFactory2> wicFactory;

    private:
        std::thread m_thread;
        std::mutex m_queueMutex;
        std::condition_variable m_cv;
        std::queue<std::function<void()> > m_queue;
        std::atomic<bool> m_running;
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
                        if (task) task();
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

        void PushTask(std::function<void()> task) {
            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                // Accept tasks even before Start() — they drain once threads are up
                m_queue.push(std::move(task));
            }
            m_cv.notify_one();
        }

        void ClearQueue() {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            std::queue<std::function<void()> > empty;
            std::swap(m_queue, empty);
        }

        bool IsStarted() const {
            return m_running.load();
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
extern WorkerThread g_decoderWorker;
