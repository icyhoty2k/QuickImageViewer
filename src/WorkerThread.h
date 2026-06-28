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


class WorkerThread {
    public:
        Microsoft::WRL::ComPtr<IWICImagingFactory2> wicFactory;


        WorkerThread()
            : m_running(true) {
            m_thread = std::thread([this] {
                HRESULT hr = CoInitializeEx(
                        nullptr,
                        COINIT_MULTITHREADED
                        );


                if (FAILED(hr)) {
                    OutputDebugStringW(
                            L"WorkerThread COM init failed\n"
                            );
                }


                //
                // WIC2 factory for this worker thread
                //
                CoCreateInstance(
                        CLSID_WICImagingFactory2,
                        nullptr,
                        CLSCTX_INPROC_SERVER,
                        IID_PPV_ARGS(&wicFactory)
                        );


                while (true) {
                    std::function<void()> task;


                    {
                        std::unique_lock<std::mutex> lock(
                                m_queueMutex
                                );


                        m_cv.wait(
                                lock,
                                [this] {
                                    return !m_queue.empty()
                                           ||
                                           !m_running;
                                }
                                );


                        if (!m_running && m_queue.empty())
                            break;


                        task = std::move(
                                m_queue.front()
                                );

                        m_queue.pop();
                    }


                    if (task)
                        task();
                }


                wicFactory.Reset();


                if (SUCCEEDED(hr))
                    CoUninitialize();
            });
        }


        ~WorkerThread() {
            {
                std::lock_guard<std::mutex> lock(
                        m_queueMutex
                        );

                m_running = false;
            }


            m_cv.notify_one();


            if (m_thread.joinable())
                m_thread.join();
        }


        void PushTask(std::function<void()> task) {
            {
                std::lock_guard<std::mutex> lock(
                        m_queueMutex
                        );


                if (!m_running)
                    return;


                m_queue.push(
                        std::move(task)
                        );
            }


            m_cv.notify_one();
        }


        void ClearQueue() {
            std::lock_guard<std::mutex> lock(
                    m_queueMutex
                    );


            std::queue<std::function<void()> > empty;

            std::swap(
                    m_queue,
                    empty
                    );
        }

    private:
        std::thread m_thread;

        std::mutex m_queueMutex;

        std::condition_variable m_cv;

        std::queue<std::function<void()> > m_queue;

        std::atomic<bool> m_running;
};


extern WorkerThread g_ioWorker;
extern WorkerThread g_decoderWorker;
