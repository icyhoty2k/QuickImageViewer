// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#pragma once
#include <string>

// =============================================================================
// SessionFile — what this copy was doing, as opposed to how it is configured.
//
// Where this copy was: the image on screen at the last exit and the folder it
// was in, reopened on the next launch so qIV resumes where it was left instead
// of prompting. Both are recorded — the folder is not merely the image's parent,
// because a folder with no openable image in it has no image path to store.
//
// WHY IT IS NOT A SETTING. It changes at every close, it means nothing on
// another machine, and it is not something anyone would export, import or
// restore to defaults. Held in the settings store it was also expensive out of
// all proportion to its worth: an .ini write rewrites the WHOLE file, so one
// line of session state made every close rewrite every setting the application
// has. Its own tiny file costs one small write instead.
//
// Nothing here is worth preserving. Deleting qivSession.ini simply means the
// next launch opens from history instead.
//
// Always a file, in registry mode as well — resume position is per-copy state
// like qivHistory.txt and qivFavorites.txt beside it, not a user preference,
// and routing it through the settings store is what tied it to the settings
// file in the first place.
// =============================================================================

namespace Persistence::Session {

    // Full path of the image to reopen, or empty when there is none. The caller
    // still has to check the file exists — a remembered path may name a file on
    // a drive that is no longer attached.
    std::wstring LoadLastImage();

    // Records the image on screen. An empty value clears the key rather than
    // writing a blank, so a cleared session leaves nothing behind to puzzle over.
    void SaveLastImage(const std::wstring &fullPath);

    // The folder that was open at the last exit, or empty when there is none.
    // Same caveat as LoadLastImage: the caller checks it still exists.
    //
    // Written even when no image was loaded, which is the case LoadLastImage
    // cannot cover — an empty folder or one whose images all failed to decode
    // leaves no image path to remember, and without this the next launch has
    // nothing to resume from but the folder history.
    std::wstring LoadLastFolder();
    void SaveLastFolder(const std::wstring &folderPath);

    // --- Main window placement ----------------------------------------------
    //
    // Written only while app.rememberWindowPosition is on, and the caller
    // validates the result before using it: a stored rect can name a monitor
    // that is no longer attached.
    //
    // Returns false when nothing is stored or the stored value does not parse
    // as four integers with a positive size — in which case the out parameters
    // are left untouched and the caller keeps its own default placement.
    bool LoadWindowRect(int &x, int &y, int &width, int &height);
    void SaveWindowRect(int x, int y, int width, int height);

    // The display device the window was on, e.g. "\\.\DISPLAY2", or empty.
    // Recorded beside the rect so a rearranged desktop can still put the window
    // back on the SCREEN it was on rather than the coordinates it occupied.
    std::wstring LoadWindowMonitor();
    void SaveWindowMonitor(const std::wstring &deviceName);

    // Drops the stored rect. Called when the setting is switched off, so that
    // switching it back on starts remembering from that point rather than
    // restoring a placement from some earlier session.
    void ClearWindowRect();

    // Deletes values left behind by older builds — currently the pre-session
    // "qivLastImage", which nothing has read or written since the resume
    // position moved out of the settings store. Cheap, idempotent, and safe to
    // call on every launch: it succeeds once per machine and then does nothing.
    void RemoveObsoleteValues();

    // --- Did the last run end properly? --------------------------------------
    //
    // A LEFTOVER, not a report. A process that is killed, loses power or dies in
    // a way no handler catches gets no opportunity to say so — so the only thing
    // that can testify is a mark it failed to clear. MarkRunning(true) at
    // startup, MarkRunning(false) on the clean exit path, and a mark still
    // present next launch means the run in between never reached that path.
    //
    // Deliberately NOT a settings value: it changes twice per run, means nothing
    // on another machine, and has no business in an exported settings file.

    // What the PREVIOUS run left behind. Read once at startup, before
    // MarkRunning(true) overwrites it.
    struct PreviousRun {
        // The mark was still set — the last run did not reach its exit path.
        bool crashed = false;
        // Full path of the minidump the crash handler wrote, when there is one.
        // Empty for a kill or a power loss, which produce no dump at all — and
        // that difference is itself the diagnosis.
        std::wstring dumpPath;
    };

    // Reads the two keys and CLEARS them, so one abnormal exit is reported once
    // rather than on every launch until something overwrites it.
    PreviousRun TakePreviousRun();

    void MarkRunning(bool running);

    // Called from the crash handler, which is why it takes a raw pointer and
    // does nothing but one .ini write: that code runs in a process that has
    // already failed and must allocate as little as it can get away with.
    void RecordCrashDump(const wchar_t *dumpPath);

} // namespace Persistence::Session
