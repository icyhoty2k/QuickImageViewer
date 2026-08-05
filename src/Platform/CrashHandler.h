// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#pragma once

//
// CrashHandler — turns "it crashed sometimes" into a file with a stack in it.
//
// WHY THIS EXISTS. qIV sends nothing anywhere, which is a feature and stays one.
// The cost of that is total blindness: when the viewer dies on someone else's
// machine it simply vanishes, and the only report possible is "it closed". A bug
// that survived in OverlayManager for months — a COM object released twice,
// which crashes only when the reference count happens to reach zero — is exactly
// the shape of defect that is unfindable without a dump and obvious with one.
//
// THIS IS NOT TELEMETRY. Nothing is transmitted, no network call is made, no
// identifier is collected. A file is written next to the executable and the user
// is told it is there. Sending it is their decision and requires them to attach
// it to an issue by hand. That distinction is the whole design.
//
// WHAT A DUMP CONTAINS. Stacks for every thread, the loaded module list with
// versions, and the memory referenced by the crashing frame. It does NOT contain
// the whole heap — a viewer holding several decoded 40-megapixel bitmaps would
// otherwise produce a multi-gigabyte file that nobody can upload. It is enough
// to open in Visual Studio or WinDbg and land on the failing line, given the PDB
// from the matching build.
//
namespace Platform::Crash {

    // Installs the unhandled-exception filter. Call ONCE, as early in wWinMain
    // as possible — a crash before this runs is still invisible.
    //
    // Resolves MiniDumpWriteDump immediately rather than at crash time. Loading
    // a DLL from inside an exception filter can deadlock on the loader lock,
    // which turns a crash that would have produced a dump into a hang that
    // produces nothing.
    //
    // Safe to call when dbghelp.dll is missing: the filter is simply not
    // installed and the process behaves exactly as it did before.
    void Install();

    // --- Breadcrumbs ---------------------------------------------------------
    //
    // A dump says WHERE the process died. It does not say what it was working on,
    // and for an image viewer that is usually the more useful half: a fault in
    // the decoder means little until you know it was a 40-megapixel JPEG XL that
    // nobody else has.
    //
    // These record it. The values live in fixed buffers in the module's data
    // section, which MiniDumpWithDataSegs captures, so they arrive in the dump
    // with no extra work at crash time — the point at which nothing complicated
    // can be trusted to run.
    //
    // COST, since that is the reason nothing else was added: one bounded string
    // copy per image change, and one integer store per command. No thread, no
    // timer, nothing periodic. They are written on events that already happen and
    // are read only by a debugger, after the fact.
    //
    // Deliberately NOT a log. A rolling log means a file handle, buffering and
    // I/O on paths that must stay fast. This is the last value of each thing,
    // which is what a post-mortem actually asks for.

    // The image currently being opened or decoded. Truncated if longer than the
    // buffer; a shortened path still identifies the file.
    void NoteImage(const wchar_t *path);

    // The command last dispatched, as its Command enum value. An int rather than
    // a name so the write is a single store on the keystroke path.
    void NoteCommand(int commandId);

    // Coarse phase — "startup", "decode", "render", "shutdown". Static strings
    // only: the pointer is stored, not the characters, so the caller must pass a
    // literal that outlives the process.
    void NotePhase(const char *phase);

} // namespace Platform::Crash
