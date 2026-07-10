#pragma once
#include <windows.h>
#include <string>

namespace DriveInfo {
    // Returns the number of IO threads appropriate for the drive that hosts
    // the given path.
    //
    //   Spinning HDD  (IncursSeekPenalty == TRUE)  → 1
    //     A single thread preserves the disk-order sort benefit.
    //     Multiple concurrent requests cause head thrashing and break the
    //     physical-order guarantee from FSCTL_GET_RETRIEVAL_POINTERS.
    //
    //   SSD / NVMe    (IncursSeekPenalty == FALSE)  → 2
    //     The controller can service multiple queued commands in parallel
    //     across NAND dies. Two threads lets us overlap file-open latency
    //     while the previous decode is still running.
    //     More than 2 gives negligible extra gain for image-sized payloads.
    //
    //   Unknown / error                             → 1  (safe default)
    //
    size_t GetOptimalIoThreadCount(const std::wstring &path);
} // namespace DriveInfo.cpp
