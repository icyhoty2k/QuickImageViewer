#include "WriteQueue.h"
#include "Constants.h"
#include <future>

WriteQueue g_writeQueue;

WriteQueue::WriteQueue() : m_thread(&WriteQueue::Run, this) {}

WriteQueue::~WriteQueue() {
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_stop = true;
    }
    m_cv.notify_one();
    if (m_thread.joinable())
        m_thread.join();
}

void WriteQueue::PushDword(std::wstring key, DWORD value) {
    { std::lock_guard<std::mutex> lk(m_mutex); m_dwords[std::move(key)] = value; }
    m_cv.notify_one();
}

void WriteQueue::PushString(std::wstring key, std::wstring value) {
    { std::lock_guard<std::mutex> lk(m_mutex); m_strings[std::move(key)] = std::move(value); }
    m_cv.notify_one();
}

void WriteQueue::PushTask(std::function<void()> task) {
    { std::lock_guard<std::mutex> lk(m_mutex); m_tasks.push(std::move(task)); }
    m_cv.notify_one();
}

void WriteQueue::Flush() {
    auto done = std::make_shared<std::promise<void>>();
    auto fut = done->get_future();
    PushTask([done]() { done->set_value(); });
    fut.wait();
}

void WriteQueue::Run() {
    for (;;) {
        std::unique_lock<std::mutex> lk(m_mutex);
        m_cv.wait(lk, [&] {
            return m_stop || !m_dwords.empty() || !m_strings.empty() || !m_tasks.empty();
        });

        // Snapshot everything under the lock, then release before doing any I/O.
        decltype(m_dwords)  dwords  = std::move(m_dwords);
        decltype(m_strings) strings = std::move(m_strings);
        std::queue<std::function<void()>> tasks;
        std::swap(tasks, m_tasks);
        const bool stop = m_stop;
        lk.unlock();

        for (auto& [name, val] : dwords)
            RegSetKeyValueW(Constants::Registry::ROOT_HIVE, Constants::Registry::ROOT_KEY,
                            name.c_str(), REG_DWORD, &val, sizeof(DWORD));

        for (auto& [name, val] : strings)
            RegSetKeyValueW(Constants::Registry::ROOT_HIVE, Constants::Registry::ROOT_KEY,
                            name.c_str(), REG_SZ,
                            val.c_str(), static_cast<DWORD>((val.size() + 1) * sizeof(wchar_t)));

        while (!tasks.empty()) { tasks.front()(); tasks.pop(); }

        // Check stop AFTER draining so the destructor always flushes pending work.
        if (stop) break;
    }
}
