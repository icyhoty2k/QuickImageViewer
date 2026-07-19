#pragma once
#include <windows.h>
#include <string>
#include <unordered_map>
#include <queue>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <thread>

// Async write queue — one sleeping drain thread, woken on demand.
//
// Registry DWORD and string writes coalesce: pushing the same key twice
// keeps only the latest value, so rapid slider adjustments cost one write.
//
// File/arbitrary tasks are sequential FIFO — order is preserved.
//
// Call Flush() before process exit to guarantee all pending writes complete.
class WriteQueue {
public:
    WriteQueue();
    ~WriteQueue();

    // Coalescing registry writes — last push per key wins.
    void PushDword(std::wstring key, DWORD value);
    void PushString(std::wstring key, std::wstring value);

    // Sequential file / arbitrary I/O tasks.
    void PushTask(std::function<void()> task);

    // Block until all pending work is written (call before process exit).
    void Flush();

private:
    std::mutex                                     m_mutex;
    std::condition_variable                        m_cv;
    std::unordered_map<std::wstring, DWORD>        m_dwords;
    std::unordered_map<std::wstring, std::wstring> m_strings;
    std::queue<std::function<void()>>              m_tasks;
    bool                                           m_stop{false};
    std::thread                                    m_thread;

    void Run();
};

extern WriteQueue g_writeQueue;
