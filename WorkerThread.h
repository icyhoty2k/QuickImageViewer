#pragma once
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <functional>
#include <windows.h>

class WorkerThread {
public:
    WorkerThread() : m_running(true) {
        m_thread = std::thread([this] {
            // Initialize COM for background WIC/Direct2D operations
            CoInitializeEx(NULL, COINIT_MULTITHREADED);
            
            while (m_running) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(m_queueMutex);
                    // Thread sleeps here until notified; consumes 0 CPU
                    m_cv.wait(lock, [this] { return !m_queue.empty() || !m_running; });
                    
                    if (!m_running) break;
                    
                    task = std::move(m_queue.front());
                    m_queue.pop();
                }
                if (task) task();
            }
            CoUninitialize();
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
            m_queue.push(std::move(task));
        }
        m_cv.notify_one();
    }
    void ClearQueue() {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        std::queue<std::function<void()>> empty;
        std::swap(m_queue, empty);
    }

private:
    std::thread m_thread;
    std::mutex m_queueMutex;
    std::condition_variable m_cv;
    std::queue<std::function<void()>> m_queue;
    bool m_running;
};
// Define the global workers here so they are initialized once at startup
extern WorkerThread g_ioWorker;
extern WorkerThread g_decoderWorker;