// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <windows.h>
#include <string>
#include <vector>

// =============================================================================
//  DuplicateScan — the reading half of finding duplicate pictures
//
//  Common::DuplicateFinder decides WHAT counts as a duplicate and touches no
//  disk. This is the part that cannot be pure: it walks the index, reads the
//  files the rule asks for, and writes the report.
//
//  HOW LITTLE IT READS IS THE WHOLE DESIGN. The index already knows every
//  picture and its size, and two files of different sizes cannot be identical -
//  so only files sharing a size with another are ever opened. In an ordinary
//  collection that is a few dozen out of many thousands.
//
//  ⚠ IT REPORTS. IT NEVER DELETES. The result is a text file listing the groups
//  and what could be reclaimed; what to do about them stays the user's action,
//  taken in the shell where it can be undone. A viewer that deleted photographs
//  on its own judgement would only have to be wrong once.
// =============================================================================

namespace Platform::DuplicateScan {

    // Starts a scan on the IO pool and returns immediately. When it finishes it
    // posts WM_QIV_DUPLICATES_READY to hWnd with a heap-allocated result, which
    // the window procedure owns and deletes.
    //
    // Does nothing if a scan is already running: the button cannot start five of
    // them, and the second press of an impatient user is not a reason to read
    // every file twice.
    void StartAsync(HWND hWnd);

    // True while a scan is in flight.
    bool IsRunning();

    // What a finished scan found. Owned by the WM_QIV_DUPLICATES_READY message.
    struct Result {
        int                groups        = 0; // sets of identical pictures
        int                files         = 0; // pictures in those sets
        unsigned long long reclaimable   = 0; // bytes held by all but one of each
        int                read          = 0; // files actually opened
        int                indexed       = 0; // files considered
        std::wstring       reportPath;        // where the listing was written
        bool               reportWritten = false;

        // Every duplicate path, groups in order and copies ADJACENT, ready to
        // hand to the Find panel. Adjacency is the whole point: the two files
        // you are comparing are next to each other in the list, so arrowing
        // through it shows copy, copy, next group.
        std::vector<std::wstring> paths;

        // Which group each path belongs to, aligned with `paths`. Carried rather
        // than re-derived: the panel needs to show the OTHER copies of whatever
        // row is selected, and size-and-digest equality is the scan's answer to
        // give, not something a UI should work out again from file names.
        std::vector<int> groupIds;
    };

} // namespace Platform::DuplicateScan
