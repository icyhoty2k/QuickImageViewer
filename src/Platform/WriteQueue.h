// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

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

    // Returns 1 while the drain thread is live, 0 after destruction.
    int ThreadCount() const noexcept { return m_thread.joinable() ? 1 : 0; }

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
