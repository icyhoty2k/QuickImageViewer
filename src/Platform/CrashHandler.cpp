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

        LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS *info) {
            // CONTINUE_SEARCH rather than swallowing it: if we cannot write a
            // dump we have nothing to add, and Windows Error Reporting may still
            // do something useful. Never pretend to have handled it.
            if (!g_writeDump || !info) return EXCEPTION_CONTINUE_SEARCH;
            if (InterlockedCompareExchange(&g_inHandler, 1, 0) != 0)
                return EXCEPTION_CONTINUE_SEARCH;

            wchar_t path[MAX_PATH * 2];
            BuildDumpPath(path, MAX_PATH * 2);

            const HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            bool written = false;

            if (file != INVALID_HANDLE_VALUE) {
                MINIDUMP_EXCEPTION_INFORMATION mei;
                mei.ThreadId          = GetCurrentThreadId();
                mei.ExceptionPointers = info;
                mei.ClientPointers    = FALSE;

                // Stacks, thread info, module list, and the memory the crashing
                // frames point AT — enough to see locals that matter.
                //
                // NOT MiniDumpWithFullMemory. qIV routinely holds several
                // decoded 40-megapixel bitmaps plus a VRAM cache; a full dump
                // would be gigabytes, which no user will ever upload and many
                // disks cannot spare. A file nobody can send is the same as no
                // file at all.
                const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
                    MiniDumpNormal |
                    MiniDumpWithThreadInfo |
                    MiniDumpWithUnloadedModules |
                    MiniDumpWithIndirectlyReferencedMemory);

                written = g_writeDump(GetCurrentProcess(), GetCurrentProcessId(),
                                      file, type, &mei, nullptr, nullptr) != FALSE;
                CloseHandle(file);

                // An empty or partial file is worse than none: it looks like
                // evidence and contains nothing.
                if (!written) DeleteFileW(path);
            }

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

        // WHAT THIS STILL DOES NOT CATCH, so nobody assumes it is total:
        //   * a stack overflow deep enough that the filter has no stack to run on
        //   * CRT invalid-parameter and pure-virtual calls, which terminate
        //     through their own handlers
        //   * anything a __try/__except swallows before it reaches here
        // Each has its own hook. They are worth adding only if a real crash
        // turns out to be escaping this one.
    }
} // namespace Platform::Crash
