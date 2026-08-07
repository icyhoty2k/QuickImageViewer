// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

// CmdArgsIO.cpp — cmdArgs file export/import, shortcut generation and validation.
//
// Reached from the context menu's CmdArgs submenu. Kept separate from
// CMDArgs.cpp, which owns launch-time parsing only.
//
// DISTINCT FROM SETTINGS EXPORT: Settings Export/Import round-trips the REGISTRY
// (how the app persists itself). These emit a COMMAND LINE — the switches that
// reproduce the current session's toggles when passed to the exe.
//
// SCOPE: only switches that map to a real, persisted setting are generated.
// One-shot launch switches (-RestoreDefaults, -windowedView, a positional file)
// describe an action or a moment, not a configuration, so they are never emitted.
#include "CMDArgs.h"
#include "AppState.h"
#include "SlideshowTransitions.h"
#include "Persistence/RegistryManager.h"
#include "Platform/Constants.h"
#include "Platform/ConstantsStrings.h"
#include "Platform/ConstantsIcons.h"
#include "UI/ThemedDialog.h"
#include <algorithm>
#include <string>
#include <vector>
#include <shlobj.h>
#include <shobjidl.h>

extern AppState app;

namespace {

// Append "-flag" when `on`.
void AddFlag(std::wstring &s, bool on, const wchar_t *flag) {
    if (!on) return;
    if (!s.empty()) s += L' ';
    s += flag;
}

// Append a "-switch<joiner>value" pair ("=" inline, " " for separate-value switches).
void AddValue(std::wstring &s, const wchar_t *sw, const std::wstring &value,
              const wchar_t *joiner = L"=") {
    if (!s.empty()) s += L' ';
    s += sw;
    s += joiner;
    s += value;
}

// Quote a path containing blanks so CommandLineToArgvW rebuilds it as one token.
std::wstring QuoteIfNeeded(const std::wstring &p) {
    if (p.find(L' ') == std::wstring::npos) return p;
    return L"\"" + p + L"\"";
}

bool ReadTextFile(const std::wstring &path, std::wstring &out) {
    FILE *f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"r, ccs=UTF-8") != 0 || !f) return false;
    out.clear();
    wchar_t line[1024];
    while (fgetws(line, 1024, f)) out += line;
    fclose(f);
    return true;
}

// Shared file-dialog helper. save=true → IFileSaveDialog, else IFileOpenDialog.
std::wstring PickFile(HWND hWnd, bool save, const wchar_t *title,
                      const wchar_t *defaultName) {
    std::wstring result;
    IFileDialog *pfd = nullptr;
    const CLSID clsid = save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog;
    if (FAILED(CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd))))
        return result;

    COMDLG_FILTERSPEC filters[] = {
        {L"Text Files", L"*.txt"},
        {L"All Files",  L"*.*" }
    };
    pfd->SetFileTypes(ARRAYSIZE(filters), filters);
    pfd->SetDefaultExtension(L"txt");
    pfd->SetTitle(title);
    if (defaultName) pfd->SetFileName(defaultName);
    if (!save) {
        DWORD opts = 0;
        pfd->GetOptions(&opts);
        pfd->SetOptions(opts | FOS_FILEMUSTEXIST);
    }
    if (SUCCEEDED(pfd->Show(hWnd))) {
        IShellItem *psi = nullptr;
        if (SUCCEEDED(pfd->GetResult(&psi))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                result = p;
                CoTaskMemFree(p);
            }
            psi->Release();
        }
    }
    pfd->Release();
    return result;
}

bool IsAllDigits(const std::wstring &s) {
    return !s.empty() && std::all_of(s.begin(), s.end(),
                                     [](wchar_t c) { return c >= L'0' && c <= L'9'; });
}

// File text → argv tokens. Comment lines (# or ;) are dropped and the remainder
// joined into one line, then handed to the OS tokenizer so quoting behaves
// exactly as it will on a real command line.
bool TokenizeCmdArgs(const std::wstring &text, std::vector<std::wstring> &out) {
    out.clear();
    std::wstring line;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find_first_of(L"\r\n", pos);
        if (end == std::wstring::npos) end = text.size();
        const std::wstring row = text.substr(pos, end - pos);
        pos = end + 1;
        const size_t b = row.find_first_not_of(L" \t");
        if (b == std::wstring::npos) continue;
        if (row[b] == L'#' || row[b] == L';') continue; // comment
        if (!line.empty()) line += L' ';
        line += row;
    }
    if (line.empty()) return true; // empty file is parseable, just has nothing in it

    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW((L"qiv.exe " + line).c_str(), &argc);
    if (!argv) return false;
    for (int i = 1; i < argc; ++i) out.emplace_back(argv[i]);
    LocalFree(argv);
    return true;
}

// Validates one token. Empty return = valid, otherwise the reason it is not.
// Sets consumedNext when the switch takes the following token as its value.
std::wstring ValidateToken(const std::wstring &tok, const std::wstring &next,
                           bool &consumedNext) {
    consumedNext = false;
    if (tok.empty()) return L"empty argument";
    if (tok[0] != L'-') return {}; // positional path — nothing to verify

    static const wchar_t *kFlags[] = {
        L"-background", L"-fullscreen", L"-windowedView", L"-awaysOnTop",
        L"-slideshow",  L"-repeat",     L"-shuffle",      L"-hideMouse",
        L"-lock",       L"-dedicated",  L"-runOnStartup", L"-RestoreDefaults",
        L"-slideshowTransitionShuffle",
    };
    for (const wchar_t *f : kFlags)
        if (_wcsicmp(tok.c_str(), f) == 0) return {};

    if (_wcsnicmp(tok.c_str(), L"-monitorNum#", 12) == 0) {
        if (!IsAllDigits(tok.substr(12))) return L"expects a number, e.g. -monitorNum#1";
        return {};
    }

    // Switches whose value is the NEXT token.
    if (_wcsicmp(tok.c_str(), L"-startFolder") == 0) {
        if (next.empty()) return L"expects a folder path after it";
        consumedNext = true;
        return {};
    }
    if (_wcsicmp(tok.c_str(), L"-slideshowInterval") == 0) {
        if (!IsAllDigits(next)) return L"expects a number of seconds after it";
        consumedNext = true;
        return {};
    }

    // Switches with an inline =value. Longest prefix first.
    if (_wcsnicmp(tok.c_str(), L"-slideshowTransitionSource=", 27) == 0)
        return ParseTransitionSource(tok.substr(27)) < 0 ? L"expects none, all or list"
                                                         : std::wstring();
    if (_wcsnicmp(tok.c_str(), L"-slideshowTransitionOrder=", 26) == 0)
        return ParseTransitionOrder(tok.substr(26)) < 0 ? L"expects sequential or random"
                                                        : std::wstring();
    if (_wcsnicmp(tok.c_str(), L"-slideshowTransitions=", 22) == 0)
        return ParseTransitionList(tok.substr(22)) == 0
                   ? L"no recognised transition names or numbers"
                   : std::wstring();
    if (_wcsnicmp(tok.c_str(), L"-slideshowTransition=", 21) == 0) {
        const std::wstring v = tok.substr(21);
        // ParseTransitionType falls back to Cut, so a typo is only detectable by
        // confirming the token really did say "Cut".
        if (ParseTransitionType(v) == TransitionType::Cut && _wcsicmp(v.c_str(), L"Cut") != 0)
            return L"unknown transition name";
        return {};
    }

    return L"unknown switch";
}

// Runs every token through ValidateToken. Returns the problem report (empty when
// all valid) and the number of arguments that checked out.
std::wstring CollectProblems(const std::vector<std::wstring> &tokens, int &okCount) {
    okCount = 0;
    std::wstring problems;
    for (size_t i = 0; i < tokens.size(); ++i) {
        bool consumed = false;
        const std::wstring next = (i + 1 < tokens.size()) ? tokens[i + 1] : L"";
        const std::wstring err = ValidateToken(tokens[i], next, consumed);
        if (err.empty()) ++okCount;
        else problems += L"\n  " + tokens[i] + L"  —  " + err;
        if (consumed) ++i;
    }
    return problems;
}

} // namespace

// =============================================================================
// BuildCmdArgsFromState
// =============================================================================
std::wstring BuildCmdArgsFromState() {
    namespace TS = Constants::Slideshow::TransitionSource;
    namespace TO = Constants::Slideshow::TransitionOrder;
    const auto &tr = app.slideshow.transition;
    std::wstring s;

    AddFlag(s, app.isDedicated,          L"-dedicated");
    AddFlag(s, app.isEnableRunOnStartup, L"-runOnStartup");
    AddFlag(s, app.startInFullscreen,    L"-fullscreen");
    AddFlag(s, app.isAlwaysOnTop,        L"-awaysOnTop");
    AddFlag(s, app.isKeepInBackground,   L"-background");
    AddFlag(s, app.isLocked,             L"-lock");
    AddFlag(s, app.keepDisplayAwake,     L"-keepDisplayAwake");

    // Current folder, so a generated shortcut reopens where you are now.
    if (!app.playlist.empty() && app.currentIndex >= 0 &&
        app.currentIndex < static_cast<int>(app.playlist.size())) {
        const std::wstring &p = app.playlist[app.currentIndex];
        const size_t slash = p.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
            AddValue(s, L"-startFolder", QuoteIfNeeded(p.substr(0, slash)), L" ");
    }

    AddFlag(s, app.slideshow.loop,    L"-repeat");
    AddFlag(s, app.slideshow.shuffle, L"-shuffle");
    AddValue(s, L"-slideshowInterval",
             std::to_wstring(app.slideshow.intervalMs / 1000), L" ");

    // Transition block — the switches that reproduce the current selection.
    AddValue(s, L"-slideshowTransition",
             Constants::Messages::TRANSITION_NAMES[static_cast<int>(tr.type)]);
    if (tr.source == TS::LIST) {
        // Emitted as menu numbers: short, stable, and exactly what the menu shows.
        const int *order = TransitionDisplayOrder();
        std::wstring list;
        for (int n = 0; n < Constants::Slideshow::TRANSITION_COUNT; ++n) {
            const int i = order[n];
            if ((tr.listMask & (1u << i)) == 0u) continue;
            if (!list.empty()) list += L',';
            list += std::to_wstring(n + 1);
        }
        if (!list.empty()) AddValue(s, L"-slideshowTransitions", list);
    }
    AddValue(s, L"-slideshowTransitionSource",
             tr.source == TS::ALL ? L"all" : (tr.source == TS::LIST ? L"list" : L"none"));
    AddValue(s, L"-slideshowTransitionOrder",
             tr.order == TO::RANDOM ? L"random" : L"sequential");
    return s;
}

// =============================================================================
// ExportCmdArgsFile
// =============================================================================
void ExportCmdArgsFile(HWND hWnd) {
    const std::wstring path = PickFile(hWnd, true, L"Export Command-Line Arguments",
                                       Constants::CmdArgsFile::EXPORT_NAME);
    if (path.empty()) return;

    FILE *f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"w, ccs=UTF-8") != 0 || !f) {
        UI::ThemedDialog::Message(hWnd, L"Failed to write the file.", L"Export CmdArgs");
        return;
    }
    fwprintf(f, L"%s\n", Constants::CmdArgsFile::FILE_HEADER);
    fwprintf(f, L"# Lines starting with # or ; are ignored.\n");
    fwprintf(f, L"%s\n", BuildCmdArgsFromState().c_str());
    fclose(f);
    UI::ThemedDialog::Message(hWnd, L"Command-line arguments exported.", L"Export CmdArgs");
}

// =============================================================================
// ImportCmdArgsFile — validate first, then apply
// =============================================================================
void ImportCmdArgsFile(HWND hWnd) {
    const std::wstring path = PickFile(hWnd, false, L"Import Command-Line Arguments",
                                       Constants::CmdArgsFile::EXPORT_NAME);
    if (path.empty()) return;

    std::wstring text;
    if (!ReadTextFile(path, text)) {
        UI::ThemedDialog::Message(hWnd, L"Failed to read the file.", L"Import CmdArgs");
        return;
    }
    std::vector<std::wstring> tokens;
    if (!TokenizeCmdArgs(text, tokens) || tokens.empty()) {
        UI::ThemedDialog::Message(hWnd, L"No arguments found in the file.", L"Import CmdArgs");
        return;
    }

    // Refuse to apply anything that would not survive a real launch — a partial
    // apply is worse than none, since the user cannot tell what took effect.
    int okCount = 0;
    const std::wstring problems = CollectProblems(tokens, okCount);
    if (!problems.empty()) {
        UI::ThemedDialog::Message(hWnd,
            (L"The file contains problems and was not applied:" + problems).c_str(),
            L"Import CmdArgs");
        return;
    }

    // Rebuild an argv the launch-time parser understands (argv[0] = exe slot).
    std::wstring exeSlot = L"qiv.exe";
    std::vector<LPWSTR> argv;
    argv.push_back(exeSlot.data());
    for (auto &t : tokens) argv.push_back(t.data());

    const CmdArgs parsed = ParseCmdArgs(static_cast<int>(argv.size()), argv.data());
    ApplyCmdArgs(hWnd, parsed, SW_SHOW);
    UI::ThemedDialog::Message(hWnd, L"Command-line arguments applied.", L"Import CmdArgs");
}

// =============================================================================
// CreateCmdArgsShortcut — desktop .lnk carrying the generated switches
// =============================================================================
void CreateCmdArgsShortcut(HWND hWnd) {
    const std::wstring exe = Persistence::Registry::GetExePathW();
    if (exe.empty()) {
        UI::ThemedDialog::Message(hWnd, L"Could not determine the executable path.",
                                  L"Generate Shortcut");
        return;
    }

    PWSTR desktop = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop)) || !desktop) {
        UI::ThemedDialog::Message(hWnd, L"Could not locate the Desktop folder.",
                                  L"Generate Shortcut");
        return;
    }
    const std::wstring lnk =
        std::wstring(desktop) + L"\\" + Constants::CmdArgsFile::SHORTCUT_NAME;
    CoTaskMemFree(desktop);

    IShellLinkW *link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&link))) || !link) {
        UI::ThemedDialog::Message(hWnd, L"Could not create the shortcut object.",
                                  L"Generate Shortcut");
        return;
    }

    const std::wstring argsStr = BuildCmdArgsFromState();
    std::wstring workDir = exe;
    const size_t slash = workDir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) workDir.resize(slash);

    link->SetPath(exe.c_str());
    link->SetArguments(argsStr.c_str());
    link->SetWorkingDirectory(workDir.c_str());
    link->SetIconLocation(exe.c_str(), 0);
    link->SetDescription(L"QuickImageViewer");

    IPersistFile *pf = nullptr;
    HRESULT hr = link->QueryInterface(IID_PPV_ARGS(&pf));
    if (SUCCEEDED(hr)) {
        hr = pf->Save(lnk.c_str(), TRUE);
        pf->Release();
    }
    link->Release();

    if (SUCCEEDED(hr))
        UI::ThemedDialog::Message(hWnd,
            (L"Shortcut created on the Desktop:\n\n" + argsStr).c_str(),
            L"Generate Shortcut");
    else
        UI::ThemedDialog::Message(hWnd, L"Failed to save the shortcut.", L"Generate Shortcut");
}

// =============================================================================
// TestCmdArgsFile — validate without applying anything
// =============================================================================
void TestCmdArgsFile(HWND hWnd) {
    const std::wstring path = PickFile(hWnd, false, L"Test Command-Line Arguments File",
                                       Constants::CmdArgsFile::EXPORT_NAME);
    if (path.empty()) return;

    std::wstring text;
    if (!ReadTextFile(path, text)) {
        UI::ThemedDialog::Message(hWnd, L"Failed to read the file.", L"Test CmdArgs");
        return;
    }
    std::vector<std::wstring> tokens;
    if (!TokenizeCmdArgs(text, tokens)) {
        UI::ThemedDialog::Message(hWnd, L"The file could not be parsed as a command line.",
                                  L"Test CmdArgs");
        return;
    }
    if (tokens.empty()) {
        UI::ThemedDialog::Message(hWnd, L"No arguments found in the file.", L"Test CmdArgs");
        return;
    }

    int okCount = 0;
    const std::wstring problems = CollectProblems(tokens, okCount);
    const std::wstring tally = L"\n\nArguments checked: " + std::to_wstring(okCount) +
                               L" of " + std::to_wstring(tokens.size());
    if (problems.empty())
        UI::ThemedDialog::Message(hWnd, (std::wstring(Constants::Icon::CHECK) + L" All arguments are valid." + tally).c_str(),
                                  L"Test CmdArgs");
    else
        UI::ThemedDialog::Message(hWnd, (std::wstring(Constants::Icon::WARNING) + L" Problems found:" + problems + tally).c_str(),
                                  L"Test CmdArgs");
}
