// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#include "HistoryFoldersManager.h"

#include <windows.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <cwctype>
#include <cstring>
#include "../AppState.h"
#include "RegistryManager.h"
#include "IniFile.h"                       // TimeStampNow — one clock format
#include "../Platform/WriteQueue.h"
#include "../Dedicated/DedicatedSettings.h" // SettingsUseFile — history is off for dedicated

namespace fs = std::filesystem;

extern AppState app;

// A dedicated instance keeps NO history and NO favorites.
//
// History exists so a person can get back to somewhere they browsed; an
// appliance bolted to a wall never browses. Keeping it would also mean every
// dedicated copy writing one shared file and clobbering the others — the exact
// collision this mode is meant to eliminate. A dedicated instance runs from its
// two folder lists instead (see src/Dedicated/DedicatedLists.h).
//
// Every disk-touching method below returns early on this. The in-memory lists
// simply stay empty, so the History panel shows nothing and every caller keeps
// working without a special case.
// Keyed on the DEDICATED flag, not merely on being file-backed: a portable copy
// that keeps its settings in an .ini but is not an appliance should still have
// its history.
static bool HistoryDisabled() {
    return Dedicated::IsDedicatedFlag();
}

// Returns the filename to use on disk. Dedicated instances never reach here.
static std::wstring PrefixedFileName(const std::wstring &baseName) {
    return baseName;
}

// ---------------------------------------------------------------------------
// PATH HYGIENE  —  see the header for why this exists
// ---------------------------------------------------------------------------
namespace HistoryPath {

    // Longest path Win32 accepts with the \\?\ prefix. Anything past this is a
    // corrupt line, not a path — two concatenated entries, say.
    static constexpr size_t MAX_PATH_CHARS = 32767;

    std::wstring Clean(const std::wstring &raw) {
        std::wstring s = raw;

        // --- trim whitespace (covers \r, \n, tabs, the trailing spaces a hand
        //     edit leaves behind) ---
        auto trim = [](std::wstring &t) {
            size_t b = 0, e = t.size();
            while (b < e && iswspace(t[b])) ++b;
            while (e > b && iswspace(t[e - 1])) --e;
            t = t.substr(b, e - b);
        };
        trim(s);

        // --- unwrap one pair of quotes: Explorer's "Copy as path" produces them
        if (s.size() >= 2 && s.front() == L'"' && s.back() == L'"') {
            s = s.substr(1, s.size() - 2);
            trim(s);
        }
        return s;
    }

    bool Normalize(const std::wstring &raw, std::wstring &out) {
        std::wstring s = Clean(raw);
        if (s.empty() || s.size() > MAX_PATH_CHARS) return false;

        // --- reject characters that cannot appear in a Win32 path ---
        // ':' is allowed only as the drive separator at index 1, checked below.
        for (size_t i = 0; i < s.size(); ++i) {
            const wchar_t c = s[i];
            if (c < 0x20) return false; // control character
            if (c == L'<' || c == L'>' || c == L'"' || c == L'|' ||
                c == L'?' || c == L'*')
                return false;
            if (c == L':' && i != 1) return false;
        }

        // --- separators: '/' -> '\', then collapse runs ---
        for (auto &c: s)
            if (c == L'/') c = L'\\';

        const bool isUnc = (s.size() >= 2 && s[0] == L'\\' && s[1] == L'\\');
        std::wstring collapsed;
        collapsed.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == L'\\' && !collapsed.empty() && collapsed.back() == L'\\')
                continue; // skip the repeat
            collapsed += s[i];
        }
        if (isUnc) collapsed.insert(collapsed.begin(), L'\\'); // restore the UNC pair
        s.swap(collapsed);

        // --- must be absolute: "X:\..." or "\\server\share" ---
        const bool isDrive = (s.size() >= 3 && iswalpha(s[0]) && s[1] == L':' && s[2] == L'\\');
        if (!isDrive && !isUnc) return false;
        if (isUnc && s.size() <= 2) return false; // bare "\\"

        // --- drop a trailing separator, but keep the drive root "D:\" ---
        while (s.size() > 3 && s.back() == L'\\')
            s.pop_back();
        if (isUnc && s.size() > 2 && s.back() == L'\\')
            s.pop_back();

        // A drive root alone ("D:\") is a legitimate folder; a bare "D:" is not.
        if (s.size() < 3) return false;

        out.swap(s);
        return true;
    }

    bool IsBroken(const std::wstring &entry) {
        std::wstring ignored;
        return !Normalize(entry, ignored);
    }

    bool Equal(const std::wstring &a, const std::wstring &b) {
        return a.size() == b.size() && _wcsicmp(a.c_str(), b.c_str()) == 0;
    }

    size_t HashCI::operator()(const std::wstring &s) const {
        // FNV-1a over the lowercased characters — must agree with Equal().
        size_t h = 1469598103934665603ULL;
        for (wchar_t c: s) {
            h ^= static_cast<size_t>(towlower(c));
            h *= 1099511628211ULL;
        }
        return h;
    }

} // namespace HistoryPath

// ---------------------------------------------------------------------------
// Backup helpers  (file-scope, not exposed in the header)
// ---------------------------------------------------------------------------

// Returns (creating if needed) the QivBackup folder next to the executable.
static fs::path GetBackupDir() {
    // fs::current_path() THROWS, and this is reached from the history thread as
    // well as the UI one — an exception leaving a std::thread's function is
    // std::terminate, with no dump and no message. The empty fallback lands the
    // backup next to the working directory, which is where the throwing version
    // was aiming anyway.
    std::error_code cwdEc;
    std::wstring exePath = Persistence::Registry::GetExePathW();
    fs::path exeDir = exePath.empty()
                          ? fs::current_path(cwdEc)
                          : fs::path(exePath).parent_path();
    if (cwdEc) exeDir.clear();

    // Strip leading slash from the constant (it is stored as L"/QivBackup")
    std::wstring folderName = Constants::History::HISTORY_FAVORITES_BACKUP_FOLDER;
    if (!folderName.empty() && (folderName.front() == L'/' || folderName.front() == L'\\'))
        folderName = folderName.substr(1);

    fs::path backupDir = exeDir / folderName;
    std::error_code ec;
    if (!fs::exists(backupDir, ec))
        fs::create_directories(backupDir, ec);

    return backupDir;
}

// Builds a dated backup file path:  QivBackup/qivHistory_dd.MM.YYYY_HH.MM.SS.bak
static fs::path MakeBackupPath(const fs::path &backupDir,
                               const std::wstring &origFileName,
                               const SYSTEMTIME &st) {
    wchar_t suffix[64];
    swprintf_s(suffix, L"_%02d.%02d.%04d_%02d.%02d.%02d",
               st.wDay, st.wMonth, st.wYear,
               st.wHour, st.wMinute, st.wSecond);

    std::wstring stem = fs::path(origFileName).stem().wstring();
    std::wstring ext = Constants::History::HISTORY_FAVORITES_BACKUP_EXTENSION;
    return backupDir / (stem + suffix + ext);
}

// A line whose first non-space character is ';' or '#'.
//
// Checked BEFORE the keep-unusable-lines rule in the loader: that rule exists so
// a mistyped path is never silently swallowed, but a comment is not a mistake
// and must not be painted as a dead folder row.
static bool IsCommentLine(const std::wstring &line) {
    for (wchar_t c : line) {
        if (iswspace(c)) continue;
        return c == Constants::History::COMMENT_MARK_SEMI ||
               c == Constants::History::COMMENT_MARK_HASH;
    }
    return false; // blank line — not a comment; the loader drops it separately
}

// Writes the explanatory header, but only into a file that does not exist yet or
// is empty. Never touched again — see HISTORY_FILE_TITLE for why there is no
// "Updated" stamp here.
static void EnsureTextHeader(const std::wstring &path, const wchar_t *title) {
    {
        std::ifstream probe(path, std::ios::in | std::ios::binary | std::ios::ate);
        if (probe.is_open() && probe.tellg() > 0) return;
    }

    std::wofstream f(path, std::ios::out | std::ios::app);
    if (!f.is_open()) return;

    f << L"; QuickImageViewer - " << title << L"\n"
      << L"; Generated: " << Persistence::Ini::TimeStampNow() << L"\n"
      << L"; Lines starting with ; or # are comments and are ignored.\n"
      << L"; This header is not rewritten later - the file's modified date is the\n"
      << L"; accurate last-changed time.\n";
}

// Writes the mandatory first line:
//   BACKUP COMPUTER_NAME, dd.MM.YYYY, HH:MM:SS.ms, Backup Version Schema : 1.0
static void WriteBackupHeader(std::wofstream &f, const SYSTEMTIME &st) {
    wchar_t compName[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD sz = MAX_COMPUTERNAME_LENGTH + 1;
    GetComputerNameW(compName, &sz);

    wchar_t header[512];
    swprintf_s(header,
               L"BACKUP %s, %02d.%02d.%04d, %02d:%02d:%02d.%03d, %s",
               compName,
               st.wDay, st.wMonth, st.wYear,
               st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
               Constants::History::HISTORY_FAVORITES_BACKUP_VERSION);
    f << header << L"\n";
}

// ---------------------------------------------------------------------------
// GetFilePath  —  full path to qivHistory.txt next to the executable
// ---------------------------------------------------------------------------
std::wstring HistoryFoldersManager::GetFilePath() const {
    const std::wstring name = PrefixedFileName(historyFileName);
    std::wstring exePath = Persistence::Registry::GetExePathW();
    if (exePath.empty())
        return name;

    fs::path appDir = fs::path(exePath).parent_path();

    if (!historyFolderName.empty()) {
        appDir /= historyFolderName;
        // Non-throwing: this is called from the history thread at startup, and
        // an exception out of a std::thread's function is std::terminate. If the
        // directory cannot be made, the path is still returned and the open that
        // follows fails the ordinary way, with a message.
        std::error_code ec;
        if (!fs::exists(appDir, ec))
            fs::create_directories(appDir, ec);
    }

    return (appDir / name).wstring();
}

// ---------------------------------------------------------------------------
// GetFavoritesFilePath  —  full path to qivFavorites.txt next to the executable
// ---------------------------------------------------------------------------
std::wstring HistoryFoldersManager::GetFavoritesFilePath() const {
    const std::wstring name = PrefixedFileName(favoritesFileName);
    std::wstring exePath = Persistence::Registry::GetExePathW();
    if (exePath.empty())
        return name;

    fs::path appDir = fs::path(exePath).parent_path();

    if (!historyFolderName.empty()) {
        appDir /= historyFolderName;
        // Same reason as GetFilePath above.
        std::error_code ec;
        if (!fs::exists(appDir, ec))
            fs::create_directories(appDir, ec);
    }

    return (appDir / name).wstring();
}

// ---------------------------------------------------------------------------
// LoadHistoryFromDisk
//   Reads qivHistory.txt (paths only, no prefix) and qivFavorites.txt
//   (paths only, no prefix) separately. folderHistory is built oldest-first
//   then reversed so index 0 = most recently visited.
//
//   Legacy support: lines starting with '*' in qivHistory.txt are treated as
//   old-format favorites and migrated to the favorites set automatically.
// ---------------------------------------------------------------------------
void HistoryFoldersManager::LoadHistoryFromDisk() {
    if (HistoryDisabled()) return; // dedicated instance: no history, no favorites
    folderHistory.clear();
    favorites.clear();

    // Both files are hand-editable, so every line is untrusted: it is normalized
    // and validated before it is allowed into RAM, and duplicates are collapsed
    // case-insensitively. A line that cannot be a folder path is dropped.

    // --- Load favorites first (small file, O(1) lookup during history load) ---
    {
        std::wifstream fav(GetFavoritesFilePath());
        if (fav.is_open()) {
            std::wstring line, path;
            while (std::getline(fav, line)) {
                if (IsCommentLine(line)) continue;
                if (!HistoryPath::Normalize(line, path)) continue;
                // FolderPathSet dedupes case-insensitively on insert.
                favorites.insert(path); // no cap — all favorites must always be loadable
            }
        }
    }

    // --- Load history ---
    {
        std::wifstream file(GetFilePath());
        if (!file.is_open())
            return;

        FolderPathSet seen; // case-insensitive duplicate guard
        std::wstring line;
        while (std::getline(file, line)) {
            if (IsCommentLine(line)) continue;
            // Legacy: old-format '*' prefix — migrate to favorites set.
            // Checked before normalization, which would reject the '*'.
            bool legacyFavorite = false;
            {
                size_t b = 0;
                while (b < line.size() && iswspace(line[b])) ++b;
                if (b < line.size() &&
                    line[b] == static_cast<wchar_t>(Constants::History::HISTORY_FAVORITES_MARK)) {
                    line = line.substr(b + 1);
                    legacyFavorite = true;
                }
            }

            std::wstring path;
            const bool usable = HistoryPath::Normalize(line, path);
            if (!usable) {
                // KEEP the line. A hand-edited file must always be shown in full —
                // a row that silently disappears reads as data loss and hides the
                // very mistake the user needs to see. It is stored trimmed-only,
                // and GetFolderStatus() reports it as dead, so the panel paints it
                // in the missing-folder colour and navigation steps over it.
                path = HistoryPath::Clean(line);
                if (path.empty()) continue; // genuinely blank line — nothing to show
            }

            if (usable && legacyFavorite &&
                static_cast<int>(favorites.size()) < app.historyMaxFavs)
                favorites.insert(path);

            if (!seen.insert(path).second) continue; // already have this folder

            folderHistory.push_back(path);
        }
    }

    // Apply the save cap AFTER reading everything. The file is oldest-first, so
    // the entries to keep are the ones at the END — truncating during the read
    // (as this used to) kept the oldest N and threw away the most recent, which
    // is precisely backwards for an MRU list.
    if (app.historyMaxDirsSave > 0 &&
        folderHistory.size() > static_cast<size_t>(app.historyMaxDirsSave)) {
        folderHistory.erase(folderHistory.begin(),
                            folderHistory.end() - app.historyMaxDirsSave);
    }

    // File is oldest-first; reverse so index 0 = most recently visited
    std::reverse(folderHistory.begin(), folderHistory.end());
}

// ---------------------------------------------------------------------------
// MergeHistoryFromDisk
//   Merges disk state into the current in-memory MRU list without destroying
//   the in-memory order.  Both history and favorites are merged two-way,
//   duplicate-free:
//     Memory → disk : appends entries present in RAM but absent from the file.
//     Disk → memory : adds entries present in the file but absent from RAM.
//
//   Missing files are treated as empty (no crash, no data loss).
//   History uses append-only writes.  Favorites use a full rewrite only when
//   RAM has entries the file does not (the file is small, ≤ historyMaxFavs).
// ---------------------------------------------------------------------------
void HistoryFoldersManager::MergeHistoryFromDisk() {
    if (HistoryDisabled()) return; // dedicated instance: no history, no favorites
    // ---- HISTORY --------------------------------------------------------
    std::vector<std::wstring> diskList;
    FolderPathSet diskSet;
    {
        std::wifstream file(GetFilePath());
        if (file.is_open()) {
            std::wstring line;
            while (std::getline(file, line)) {
                if (IsCommentLine(line)) continue;
                // Legacy '*' prefix — treat path as history entry. Stripped
                // before normalization, which would otherwise reject the mark.
                size_t b = 0;
                while (b < line.size() && iswspace(line[b])) ++b;
                if (b < line.size() &&
                    line[b] == static_cast<wchar_t>(Constants::History::HISTORY_FAVORITES_MARK))
                    line = line.substr(b + 1);

                std::wstring path;
                if (!HistoryPath::Normalize(line, path)) {
                    path = HistoryPath::Clean(line); // keep it visible — see LoadHistoryFromDisk
                    if (path.empty()) continue;
                }
                if (!diskSet.insert(path).second) continue; // case-insensitive dupe
                diskList.push_back(path);
            }
        }
        // Missing file → diskList empty; memory entries will recreate it via appends below.
    }
    // Disk file is oldest-first; reverse so newest entries are appended to memory last
    // (they belong at lower priority than anything already in the MRU list).
    std::reverse(diskList.begin(), diskList.end());

    // Memory → disk: entries in RAM that are not on disk
    for (const auto &path : folderHistory) {
        if (!diskSet.count(path))
            AppendNewFolderToDisk(path);
    }

    // Disk → memory: entries on disk that are not in RAM
    {
        FolderPathSet memSet;
        memSet.reserve(folderHistory.size() + diskList.size());
        memSet.insert(folderHistory.begin(), folderHistory.end());
        for (const auto &path : diskList) {
            if (!memSet.count(path) &&
                static_cast<int>(folderHistory.size()) < app.historyMaxDirsSave) {
                folderHistory.push_back(path);
                memSet.insert(path);
            }
        }
    }

    // ---- FAVORITES ------------------------------------------------------
    FolderPathSet diskFavSet;
    {
        std::wifstream fav(GetFavoritesFilePath());
        if (fav.is_open()) {
            std::wstring line, path;
            while (std::getline(fav, line)) {
                if (IsCommentLine(line)) continue;
                if (HistoryPath::Normalize(line, path))
                    diskFavSet.insert(path);
            }
        }
        // Missing file → diskFavSet empty; memory favorites will be rewritten below if any.
    }

    // Disk → memory: disk favorites not in RAM (no cap — all favorites must be visible)
    for (const auto &path : diskFavSet) {
        if (!favorites.count(path))
            favorites.insert(path);
    }

    // Memory → disk: if RAM has favorites the file does not, rewrite (file is small)
    bool memHasNewFavs = false;
    for (const auto &path : favorites) {
        if (!diskFavSet.count(path)) { memHasNewFavs = true; break; }
    }
    if (memHasNewFavs)
        RewriteFavoritesToDisk();
}

// ---------------------------------------------------------------------------
// AppendNewFolderToDisk
//   Appends a single path to qivHistory.txt. No prefix — plain path only.
//   The caller guarantees this path is genuinely new (not already in folderHistory).
// ---------------------------------------------------------------------------
void HistoryFoldersManager::AppendNewFolderToDisk(const std::wstring &folderPath) const {
    if (HistoryDisabled()) return; // dedicated instance: no history, no favorites
    std::wstring path = GetFilePath();
    std::wstring entry;
    if (!HistoryPath::Normalize(folderPath, entry)) return; // never write junk
    g_writeQueue.PushTask([path = std::move(path), entry = std::move(entry)]() {
        EnsureTextHeader(path, Constants::History::HISTORY_FILE_TITLE);
        // The file may not end with a newline: a previous write can be cut short
        // by the process exiting, and a person editing the file in Notepad often
        // leaves the last line unterminated. Appending blindly then glues the new
        // path onto the old one and destroys BOTH entries — e.g.
        //   D:\Wallpapers\[Set 9]E:\Wallpapers\[Set 9]
        // So: look at the last byte first and terminate the line if needed.
        bool needsNewline = false;
        {
            std::ifstream probe(path, std::ios::in | std::ios::binary | std::ios::ate);
            if (probe.is_open()) {
                const std::streamoff size = probe.tellg();
                if (size > 0) {
                    probe.seekg(-1, std::ios::end);
                    char last = 0;
                    probe.read(&last, 1);
                    needsNewline = (last != '\n');
                }
            }
        }

        std::wofstream f(path, std::ios::out | std::ios::app);
        if (!f.is_open()) return;
        if (needsNewline) f << L"\n";
        f << entry << L"\n";
    });
}

// ---------------------------------------------------------------------------
// RewriteHistoryToDisk
//   Rewrites qivHistory.txt with all current paths (oldest-first, no prefix).
//   Does NOT touch qivFavorites.txt.
//   Used by ClearHistoryKeepFavorites.
// ---------------------------------------------------------------------------
void HistoryFoldersManager::RewriteHistoryToDisk() const {
    if (HistoryDisabled()) return; // dedicated instance: no history, no favorites
    std::wstring path = GetFilePath();
    // Snapshot oldest-first (reverse of MRU) at push time so the drain thread
    // writes a consistent view even if folderHistory changes before it wakes.
    std::vector<std::wstring> snap(folderHistory.rbegin(), folderHistory.rend());
    g_writeQueue.PushTask([path = std::move(path), snap = std::move(snap)]() {
        // trunc wipes the header along with the rows, so it is laid down again
        // before them. The Generated time therefore tracks the last full
        // rewrite here, which is the only honest thing it can say.
        { std::wofstream wipe(path, std::ios::out | std::ios::trunc); }
        EnsureTextHeader(path, Constants::History::HISTORY_FILE_TITLE);

        std::wofstream f(path, std::ios::out | std::ios::app);
        if (!f.is_open()) return;
        for (const auto &e : snap) f << e << L"\n";
    });
}

// ---------------------------------------------------------------------------
// RewriteFavoritesToDisk
//   Rewrites qivFavorites.txt with up to HISTORY_MAX_FAVORITES_TO_SHOW paths.
//   Does NOT touch qivHistory.txt.
//   Used by ToggleFavorite and ClearFavoritesKeepHistory.
// ---------------------------------------------------------------------------
void HistoryFoldersManager::RewriteFavoritesToDisk() const {
    if (HistoryDisabled()) return; // dedicated instance: no history, no favorites
    std::wstring path = GetFavoritesFilePath();
    std::vector<std::wstring> snap(favorites.begin(), favorites.end());
    g_writeQueue.PushTask([path = std::move(path), snap = std::move(snap)]() {
        { std::wofstream wipe(path, std::ios::out | std::ios::trunc); }
        EnsureTextHeader(path, Constants::History::FAVORITES_FILE_TITLE);

        std::wofstream f(path, std::ios::out | std::ios::app);
        if (!f.is_open()) return;
        // Everything in RAM is written — the cap belongs at the point a favorite
        // is ADDED, not here. Truncating on save silently destroys favorites the
        // user already has, and that is now reachable: the add-time limit counts
        // unique folders, so a junction and its target are one favorite for the
        // cap but two lines in this file.
        for (const auto &e : snap) f << e << L"\n";
    });
}

// ---------------------------------------------------------------------------
// BackupHistoryToDisk
//   Writes a dated snapshot of the current in-RAM history to QivBackup/.
//   Must be called BEFORE clearing RAM and before RewriteHistoryToDisk.
//   Format:  first line = header, then paths oldest-first (same as the live file).
// ---------------------------------------------------------------------------
void HistoryFoldersManager::BackupHistoryToDisk() const {
    if (HistoryDisabled()) return; // dedicated instance: no history, no favorites
    // Capture time and data snapshot now so the backup reflects the exact moment
    // the user triggered it, not whenever the drain thread eventually runs.
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::wstring fileName = PrefixedFileName(historyFileName);
    std::vector<std::wstring> snap(folderHistory.rbegin(), folderHistory.rend());
    g_writeQueue.PushTask([st, fileName = std::move(fileName), snap = std::move(snap)]() {
        fs::path backupPath = MakeBackupPath(GetBackupDir(), fileName, st);
        std::wofstream f(backupPath, std::ios::out | std::ios::trunc);
        if (!f.is_open()) return;
        WriteBackupHeader(f, st);
        for (const auto &e : snap) f << e << L"\n";
    });
}

// ---------------------------------------------------------------------------
// BackupFavoritesToDisk
//   Writes a dated snapshot of the current in-RAM favorites to QivBackup/.
//   Must be called BEFORE clearing RAM and before RewriteFavoritesToDisk.
//   Format:  first line = header, then favorite paths.
// ---------------------------------------------------------------------------
void HistoryFoldersManager::BackupFavoritesToDisk() const {
    if (HistoryDisabled()) return; // dedicated instance: no history, no favorites
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::wstring fileName = PrefixedFileName(favoritesFileName);
    std::vector<std::wstring> snap(favorites.begin(), favorites.end());
    const int maxFavs = app.historyMaxFavs;
    g_writeQueue.PushTask([st, fileName = std::move(fileName), snap = std::move(snap), maxFavs]() {
        fs::path backupPath = MakeBackupPath(GetBackupDir(), fileName, st);
        std::wofstream f(backupPath, std::ios::out | std::ios::trunc);
        if (!f.is_open()) return;
        WriteBackupHeader(f, st);
        int written = 0;
        for (const auto &e : snap) {
            if (written >= maxFavs) break;
            f << e << L"\n";
            ++written;
        }
    });
}
