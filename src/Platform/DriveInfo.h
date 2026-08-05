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
