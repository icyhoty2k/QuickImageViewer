// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#include "CrashHandler.h"

#include <windows.h>
#include <dbghelp.h>

#include <cstdlib>   // _set_purecall_handler, _set_invalid_parameter_handler
#include <exception> // std::set_terminate

// One .ini write from the failing process, read back and logged next launch —
// see the note at the call site.
#include "Persistence/SessionFile.h"

//
// EVERYTHING BELOW THE FILTER RUNS IN A BROKEN PROCESS.
//
// The rules that follow from that, and they are not stylistic:
//
//   * NO HEAP. The heap may be the thing that is corrupt, and a std::wstring in
//     here would turn a crash that could have been diagnosed into a second crash
//     inside the diagnostic. Every buffer is on the stack, every string
//     operation is a Win32 one.
//   * NO LOADLIBRARY. Resolved in Install(), while the process is healthy — see
//     the header.
//   * NO ThemedDialog. It touches static window handles, fonts and a message
//     loop, any of which may be in the state that caused the crash. This is the
//     one place in qIV where a raw MessageBoxW is correct, and it is why the
//     rule "always use ThemedDialog" has this single exception.
//   * NO ASSUMPTION THAT ANYTHING SUCCEEDS. Each step is checked and the failure
//     path is "tell the user less", never "crash again".
//

namespace Platform::Crash {

    // ------------------------------------------------------------------------
    // BREADCRUMBS — see the header.
    //
    // NOT in the anonymous namespace and NOT static: the linker must keep them
    // and give them a name, because their entire purpose is to be found in a
    // dump. /OPT:REF would be free to discard a file-local buffer that only ever
    // gets written to, and a discarded breadcrumb is worse than none.
    //
    // The names are prefixed and distinctive so they are trivially greppable in
    // a debugger's symbol list.
    // ------------------------------------------------------------------------
    wchar_t     g_qivCrumbImage[MAX_PATH] = L"(none)";
    volatile int g_qivCrumbCommand        = -1;
    const char  *g_qivCrumbPhase          = "startup";

    void NoteImage(const wchar_t *path) {
        if (!path) return;
        size_t i = 0;
        while (path[i] && i + 1 < MAX_PATH) { g_qivCrumbImage[i] = path[i]; ++i; }
        g_qivCrumbImage[i] = L'\0';
    }

    void NoteCommand(int commandId) { g_qivCrumbCommand = commandId; }
    void NotePhase(const char *phase) { if (phase) g_qivCrumbPhase = phase; }

    namespace {
        using MiniDumpWriteDumpFn = BOOL(WINAPI *)(HANDLE, DWORD, HANDLE,
                                                   MINIDUMP_TYPE,
                                                   PMINIDUMP_EXCEPTION_INFORMATION,
                                                   PMINIDUMP_USER_STREAM_INFORMATION,
                                                   PMINIDUMP_CALLBACK_INFORMATION);

        MiniDumpWriteDumpFn g_writeDump = nullptr;

        // One dump per process life. A crash inside the handler — or a second
        // thread faulting while the first is mid-write — must not recurse, and
        // must not truncate the dump already being written by overwriting it.
        LONG g_inHandler = 0;

        // What the dump is written next to. Captured in Install() because
        // GetModuleFileNameW is cheap then and one less thing to trust later.
        wchar_t g_dumpDir[MAX_PATH] = {};

        // Appends to `dst` without the CRT. Returns the new length. Truncates
        // rather than overflowing; a shortened file name is a survivable outcome
        // and a smashed stack is not.
        size_t Append(wchar_t *dst, size_t cap, size_t len, const wchar_t *src) {
            while (*src && len + 1 < cap) dst[len++] = *src++;
            dst[len] = L'\0';
            return len;
        }

        // Fixed-width unsigned, zero padded. wsprintfW would do, but it is a
        // user32 call that can allocate internally; this cannot fail.
        size_t AppendNum(wchar_t *dst, size_t cap, size_t len, unsigned value, int digits) {
            wchar_t tmp[16];
            for (int i = digits - 1; i >= 0; --i) {
                tmp[i] = static_cast<wchar_t>(L'0' + (value % 10));
                value /= 10;
            }
            tmp[digits] = L'\0';
            return Append(dst, cap, len, tmp);
        }

        // "<dir>\QuickImageViewer_crash_YYYYMMDD_HHMMSS_<pid>.dmp"
        //
        // NO VERSION IN THE NAME, deliberately. AppMain cannot use VER_STR — see
        // Version.h, where reaching the build number from a widely included
        // header would make every build a full rebuild. It is not needed anyway:
        // a minidump carries the module list, so the debugger reads the exact
        // version and timestamp of the exe out of the dump itself.
        //
        // The PID is there so two instances crashing in the same second cannot
        // write over one another.
        void BuildDumpPath(wchar_t *out, size_t cap) {
            SYSTEMTIME st;
            GetLocalTime(&st);

            size_t n = 0;
            n = Append(out, cap, n, g_dumpDir);
            n = Append(out, cap, n, L"QuickImageViewer_crash_");
            n = AppendNum(out, cap, n, st.wYear, 4);
            n = AppendNum(out, cap, n, st.wMonth, 2);
            n = AppendNum(out, cap, n, st.wDay, 2);
            n = Append(out, cap, n, L"_");
            n = AppendNum(out, cap, n, st.wHour, 2);
            n = AppendNum(out, cap, n, st.wMinute, 2);
            n = AppendNum(out, cap, n, st.wSecond, 2);
            n = Append(out, cap, n, L"_");
            n = AppendNum(out, cap, n, GetCurrentProcessId() % 1000000u, 6);
            (void) Append(out, cap, n, L".dmp");
        }

        // Writes a dump and returns whether it landed.
        bool WriteDump(MINIDUMP_EXCEPTION_INFORMATION *mei,
                       wchar_t *pathOut, size_t pathCap) {
            if (!g_writeDump) return false;

            BuildDumpPath(pathOut, pathCap);

            const HANDLE file = CreateFileW(pathOut, GENERIC_WRITE, 0, nullptr,
                                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) return false;

            const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
                MiniDumpNormal |
                MiniDumpWithThreadInfo |
                MiniDumpWithUnloadedModules |
                MiniDumpWithIndirectlyReferencedMemory |
                MiniDumpWithDataSegs |
                MiniDumpWithHandleData |
                MiniDumpWithFullMemoryInfo);

            const bool ok = g_writeDump(GetCurrentProcess(), GetCurrentProcessId(),
                                        file, type, mei, nullptr, nullptr) != FALSE;
            CloseHandle(file);

            // An empty or partial file is worse than none: it looks like evidence
            // and contains nothing.
            if (!ok) DeleteFileW(pathOut);
            return ok;
        }

        LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS *info) {
            // CONTINUE_SEARCH rather than swallowing it: if we cannot write a
            // dump we have nothing to add, and Windows Error Reporting may still
            // do something useful. Never pretend to have handled it.
            if (!g_writeDump || !info) return EXCEPTION_CONTINUE_SEARCH;
            if (InterlockedCompareExchange(&g_inHandler, 1, 0) != 0)
                return EXCEPTION_CONTINUE_SEARCH;

            wchar_t path[MAX_PATH * 2];
            MINIDUMP_EXCEPTION_INFORMATION mei;
            mei.ThreadId          = GetCurrentThreadId();
            mei.ExceptionPointers = info;
            mei.ClientPointers    = FALSE;

            const bool written = WriteDump(&mei, path, MAX_PATH * 2);

            // HANDED TO THE NEXT LAUNCH, not logged here.
            //
            // The General log would be the obvious place, and it is the wrong
            // one: this runs in a process that has already failed, and reaching
            // into a logger means a mutex, string allocation and a writer thread
            // that may already be dead. One .ini write is far less than the
            // minidump just written beside it, and startup — where the heap is
            // sound — turns it into a proper ERROR line naming this file.
            if (written) Persistence::Session::RecordCrashDump(path);

            wchar_t msg[MAX_PATH * 2 + 512];
            size_t m = 0;
            if (written) {
                m = Append(msg, MAX_PATH * 2 + 512, m,
                           L"QuickImageViewer has crashed and must close.\n\n"
                           L"A crash report was saved to:\n\n");
                m = Append(msg, MAX_PATH * 2 + 512, m, path);
                (void) Append(msg, MAX_PATH * 2 + 512, m,
                              L"\n\nNothing has been sent anywhere. If you would like this "
                              L"fixed, attach that file to an issue at:\n"
                              L"github.com/icyhoty2k/QuickImageViewer/issues\n\n"
                              L"It contains a snapshot of the program's state at the moment "
                              L"it failed. It does not contain your images.");
            } else {
                (void) Append(msg, MAX_PATH * 2 + 512, m,
                              L"QuickImageViewer has crashed and must close.\n\n"
                              L"A crash report could not be written — the folder the program "
                              L"runs from may be read-only.");
            }

            // Raw MessageBoxW on purpose — see the note at the top of this file.
            MessageBoxW(nullptr, msg, L"QuickImageViewer",
                        MB_OK | MB_ICONERROR | MB_TASKMODAL | MB_SETFOREGROUND);

            // We have told the user and written what we can, so there is nothing
            // for WER to add and its second dialog would only be noise.
            return EXCEPTION_EXECUTE_HANDLER;
        }

        // --- The failures that never reach the filter above ------------------
        //
        // SetUnhandledExceptionFilter only sees STRUCTURED exceptions that nobody
        // handled. Several common ways to die are not that, and each terminates
        // the process through its own path with no dump written. Every hook below
        // exists to convert one of those into an ordinary access violation, so
        // the filter runs and a dump lands with a usable stack.
        //
        // RaiseException rather than calling the filter directly: it produces a
        // real EXCEPTION_POINTERS with a genuine context record, so the dump
        // shows the frames that led here rather than the frames of the reporting
        // code itself.
        void CrashWith(DWORD code) {
            RaiseException(code, EXCEPTION_NONCONTINUABLE, 0, nullptr);
        }

        // An uncaught C++ exception, or a throw during unwinding. Otherwise
        // std::terminate calls abort(), which exits without a dump.
        void OnTerminate() {
            CrashWith(0xE0000001);
        }

        // A virtual call on a partially destroyed object. Silent by default.
        void __cdecl OnPureCall() {
            CrashWith(0xE0000002);
        }

        // The CRT's answer to a bad argument — a null format string, an invalid
        // iterator, a bad file handle. In a release build the default handler
        // calls abort() with no message and no dump, which is the single most
        // opaque way this program can die.
        void __cdecl OnInvalidParameter(const wchar_t *, const wchar_t *,
                                        const wchar_t *, unsigned int, uintptr_t) {
            CrashWith(0xE0000003);
        }

        // NO std::set_new_handler HERE, and the reason is worth keeping.
        //
        // One was installed, to turn an allocation failure into a dump. It is a
        // REGRESSION: a new_handler that does not return makes operator new stop
        // throwing, so std::bad_alloc is never raised — and this codebase has
        // catch(...) blocks that recover from it today. FileHandler's open path
        // and IniFile's parser both bail out cleanly on a throw; with the handler
        // installed they would have crashed instead.
        //
        // Trading graceful degradation for a crash report is the wrong way round.
        //
        // It was also redundant. An allocation failure that nobody catches ends
        // in std::terminate, and OnTerminate above is already hooked — so an
        // UNHANDLED bad_alloc still produces a dump, while a handled one still
        // recovers. That is exactly the behaviour wanted.

    } // namespace

    void Install() {
        // Loaded HERE, not in the filter. See the header.
        const HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
        if (!dbghelp) return;

        g_writeDump = reinterpret_cast<MiniDumpWriteDumpFn>(
            reinterpret_cast<void *>(GetProcAddress(dbghelp, "MiniDumpWriteDump")));
        if (!g_writeDump) {
            FreeLibrary(dbghelp);
            return;
        }
        // dbghelp is deliberately NOT freed on success — the filter needs it for
        // the life of the process, and unloading it later would leave a function
        // pointer into unmapped memory.

        // BESIDE THE EXE, matching where qIV keeps its INI. That is the folder a
        // portable app's user already looks in, so the file is found rather than
        // hunted for. If the program lives somewhere read-only the CreateFileW
        // in the filter fails and the user is told so, which is a better outcome
        // than silently writing to a temp folder they will never open.
        const DWORD n = GetModuleFileNameW(nullptr, g_dumpDir, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) {
            g_dumpDir[0] = L'\0'; // current directory — still better than nothing
        } else {
            for (DWORD i = n; i > 0; --i) {
                if (g_dumpDir[i - 1] == L'\\' || g_dumpDir[i - 1] == L'/') {
                    g_dumpDir[i] = L'\0'; // keep the separator
                    break;
                }
            }
        }

        SetUnhandledExceptionFilter(OnUnhandledException);

        // Route the non-SEH death paths into the same filter. Without these, each
        // one calls abort() and the process disappears with no dump at all — see
        // the handlers above for what each one covers.
        std::set_terminate(OnTerminate);
        _set_purecall_handler(OnPureCall);
        _set_invalid_parameter_handler(OnInvalidParameter);

        // ENOUGH STACK LEFT TO REPORT A STACK OVERFLOW.
        //
        // On overflow the guard page is hit and the filter runs on whatever
        // remains — which by definition is almost nothing, so writing a dump
        // faults again and the process dies silently. Reserving 64 KB up front
        // means the handler has room to work in the one case it is least able to
        // ask for any.
        ULONG stackBytes = 64 * 1024;
        SetThreadStackGuarantee(&stackBytes);

        // Suppress Windows Error Reporting's own dialog. Ours has already told
        // the user where the dump is; a second, less useful box racing it just
        // makes the crash look worse than it is.
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

        // STILL NOT CAUGHT, so nobody assumes this is total:
        //   * anything a __try/__except swallows before it reaches here
        //   * a heap corruption that Windows turns into an immediate
        //     RtlFailFast — those bypass every user-mode handler by design
        //   * a hang, which is not a crash and produces nothing at all
        //
        // SetThreadStackGuarantee applies per THREAD. The worker pools set their
        // own in WorkerThread.h, since every crash seen so far has been on one.
    }

} // namespace Platform::Crash
