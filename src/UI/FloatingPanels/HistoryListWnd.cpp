// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#include "HistoryListWnd.h"
#include "UI/GdiPool.h" // pooled brushes and pens — never DeleteObject them
#include "UI/CustomControls/ScrollView.h" // WheelDeltaToPixels — the one wheel rule
#include "../../Platform/Constants.h"
#include "../../Platform/ConstantsTheme.h"
#include "../../Platform/ConstantsStrings.h"
#include "../../Platform/ConstantsIcons.h"
#include "../../Platform/FileHandler.h"
#include "../../Persistence/RegistryManager.h"
#include "../../Overlays/OverlayManager.h"
#include "../../AppState.h"
#include "../../Input/Shortcuts.h"
#include "../../Input/Command.h"
#include "../../Persistence/HistoryFoldersManager.h"
#include "../../Platform/WriteQueue.h" // g_writeQueue.Flush() — F5 reloads from disk
#include "../UIManager.h"
#include "Common/FuzzyMatch.h"
#include "CustomControls/InputBox.h"
#include <algorithm>
#include <atomic>
#include <cwctype>
#include <thread>
#include <unordered_map>
#include "../ThemedTooltip.h"
#include <windowsx.h>
#include <filesystem>
#include <Constants.h>

// Forward declarations from ThumbnailPanelWnd.cpp
namespace UI {
    extern void SetActivePanelWindow(HWND hWnd);

    extern HWND g_activePanelHwnd;
}

// ---------------------------------------------------------------------------
// HistoryListWnd.cpp  —  Last-visited folder history panel.
//
// DATA MODEL
//   historyFoldersManager.folderHistory — MRU vector, index 0 = most recent,
//                                         up to HISTORY_MAX_DIRS_TO_SAVE entries.
//   historyFoldersManager.favorites     — FolderPathSet for O(1) favorite lookup.
//
//   Both hold NORMALIZED paths (HistoryPath::Normalize) and compare
//   case-insensitively, because the two backing .txt files are hand-editable and
//   Windows folder names are case-insensitive. Never compare two folder paths
//   here with operator== — use HistoryPath::Equal or a FolderPathSet.
//
// DISPLAY MODEL
//   BuildDisplayList() assembles g_displayList from the MRU vector each time
//   the panel is shown or invalidated.  The display list respects
//   HISTORY_FAVORITES_POSITION (0=top, 1=bottom, 2=in-place) and is capped
//   at HISTORY_MAX_DIRS_TO_SHOW regular rows + HISTORY_MAX_FAVORITES_TO_SHOW
//   favorite rows.
//
// FILE STRATEGY
//   Append-only for new unique paths.  Full rewrite only on ToggleFavorite
//   or ClearHistoryKeepFavorites.
// ---------------------------------------------------------------------------

namespace UI {
    // ---------------------------------------------------------------------------
    // File-scope state
    // ---------------------------------------------------------------------------
    static HWND g_hHistOwner = nullptr;
    static int g_hoverRow = -1;
    // Row remembered across focus loss, restored on refocus. Focus bounces
    // whenever a spawned panel opens/closes (Shift+Enter) — without this the
    // keyboard selection resets every time. -1 = nothing to restore.
    static int g_savedHoverRow = -1;
    // Scroll state. A file static like everything else in this panel; the base
    // drives every interaction against it through ScrollViewAt.
    static UI::ScrollView g_view;
    // The scrollbar drag statics are gone with the code that used them: the base
    // owns the drag, keyed off the ScrollView above. Its mapping is cursor
    // travel onto THUMB travel, where this panel's was travel × maxScroll ÷
    // thumb-free length — close, but it drifted from the pointer on long lists.

    // Full vs short list lives in app.historyFullModeEnabled — AppState is the
    // single source of truth for every persistent toggle. There is deliberately
    // NO local copy: a panel-scoped bool would be reset from AppState on every
    // Show(), silently discarding a Ctrl+Tab the user made while the panel was
    // open. Toggling writes to AppState (and the registry) so the panel reopens
    // in the mode the user last chose, and so the folder-walk keys — which read
    // the same flag — always agree with what the panel is showing.
    static void SetFullHistoryMode(bool full) {
        if (app.historyFullModeEnabled == full) return;
        app.historyFullModeEnabled = full;
        Persistence::Registry::SaveSetting(Constants::Registry::HISTORY_FULL_MODE,
                                           static_cast<DWORD>(full));
    }

    // One-shot "show the full list for THIS opening" flag.
    //
    // Ctrl+Tab from the main app means "open the history, uncapped" — it is a way
    // of looking at everything, not a statement about which mode the panel should
    // default to. So it must NOT touch app.historyFullModeEnabled: a later plain
    // Tab has to reopen in whatever mode the user actually chose.
    //
    // Ctrl+Tab INSIDE an already-open panel is the opposite: that is a deliberate
    // mode switch, so it writes the preference (and clears this override, since
    // the preference then describes what is on screen).
    //
    // Cleared by Show(), so every ordinary open starts from the preference.
    static bool g_fullModeOverride = false;

    // What the panel is showing right now: the saved preference, unless this
    // particular opening was forced full by Ctrl+Tab.
    static bool EffectiveFullMode() {
        return g_fullModeOverride || app.historyFullModeEnabled;
    }
    static bool g_headerDragging = false;
    static int g_headerDragStartX = 0; // screen X at drag start
    static int g_headerDragStartY = 0; // screen Y at drag start
    static RECT g_headerDragWindowRect = {}; // window rect at drag start
    static int g_bodyTop = 0; // updated each WM_PAINT — actual top of scrollable body
    static int g_bodyBottom = 0; // updated each WM_PAINT — actual bottom of scrollable body
    static int g_rowH = 0; // updated each WM_PAINT — row height in pixels
    static HistoryFoldersManager historyFoldersManager;

    // Ctrl+Z undo state for single-row Delete
    static std::wstring g_lastDeletedPath;
    static int g_lastDeletedIndex = -1;
    static bool g_lastDeletedWasFavorite = false;

    // ---------------------------------------------------------------------------
    // Folder validity cache.
    // Populated lazily by background validation; also set inline on Enter/click.
    // Unknown = not yet checked → painted as normal (no warning colour).
    // ---------------------------------------------------------------------------
    enum class FolderStatus { Unknown, Valid, Missing, Empty };

    // Keyed case-insensitively, like every other folder-path container here —
    // otherwise "D:\Pics" and "d:\pics" get separate status entries and the same
    // folder can be shown valid in one row and missing in another.
    static std::unordered_map<std::wstring, FolderStatus,
                              HistoryPath::HashCI, HistoryPath::EqualCI> g_statusCache;

    struct DirSizeInfo { int64_t bytes = 0; int count = 0; };
    static std::unordered_map<std::wstring, DirSizeInfo,
                              HistoryPath::HashCI, HistoryPath::EqualCI> g_dirSizeCache;

    // ---------------------------------------------------------------------------
    // SYMLINK / JUNCTION DETECTION
    //
    // Two Windows mechanisms alias a directory, and they are NOT the same thing:
    //   mklink /J  → a junction        → IO_REPARSE_TAG_MOUNT_POINT
    //   mklink /D  → a directory symlink → IO_REPARSE_TAG_SYMLINK
    //
    // std::filesystem::is_symlink() only recognises the second, so junctions —
    // the usual way to hang one drive's folder off another — come back false.
    //
    // Testing FILE_ATTRIBUTE_REPARSE_POINT alone is wrong in the other direction:
    // OneDrive placeholders, Dedup and WIM-boot files all carry that attribute
    // without being links, and every one of them would be mislabelled. The tag
    // itself is the only accurate test, and it also tells us WHICH kind it is,
    // which the hover popup needs.
    //
    // NOT covered: `subst` drives and mapped network drives. Those are DOS device
    // mappings rather than reparse points — nothing on the path carries a tag, so
    // no per-folder check can see them.
    // ---------------------------------------------------------------------------
    enum class LinkKind {
        None,
        Junction, // mklink /J  — IO_REPARSE_TAG_MOUNT_POINT
        Symlink,  // mklink /D  — IO_REPARSE_TAG_SYMLINK
        Mapped,   // resolves elsewhere with no reparse point: subst / network drive
    };

    struct LinkInfo {
        LinkKind kind = LinkKind::None;
        std::wstring target; // fully resolved destination; empty if unresolvable
    };

    // Cached: each miss costs a directory open. Cleared by F5, since a link can be
    // created or destroyed while qIV is running.
    static std::unordered_map<std::wstring, LinkInfo,
                              HistoryPath::HashCI, HistoryPath::EqualCI> g_symlinkCache;

    // Where does this path REALLY end up? Follows the whole chain, not just one
    // hop, which is what the user wants to see. FILE_FLAG_BACKUP_SEMANTICS is
    // mandatory — without it CreateFileW refuses to open a directory at all.
    static std::wstring ResolveFinalPath(const std::wstring &path) {
        HANDLE h = CreateFileW(path.c_str(), 0,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h == INVALID_HANDLE_VALUE) return {};

        std::wstring buf(MAX_PATH, L'\0');
        DWORD n = GetFinalPathNameByHandleW(h, buf.data(),
                                            static_cast<DWORD>(buf.size()),
                                            FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (n >= buf.size()) { // needed more room — n is the required length
            buf.resize(n + 1);
            n = GetFinalPathNameByHandleW(h, buf.data(),
                                          static_cast<DWORD>(buf.size()),
                                          FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        }
        CloseHandle(h);
        if (n == 0 || n >= buf.size()) return {};

        buf.resize(n);
        // GetFinalPathNameByHandleW always returns the \\?\ long-path form; strip
        // it so the popup shows the path the user would actually type.
        if (buf.rfind(L"\\\\?\\", 0) == 0) buf.erase(0, 4);
        return buf;
    }

    // Reparse kind of ONE path component, or None. FindFirstFileW is the only
    // call that hands back the tag: when FILE_ATTRIBUTE_REPARSE_POINT is set,
    // dwReserved0 holds it. Any other tag is a cloud placeholder / dedup stub,
    // which is not a link.
    static LinkKind ReparseKindOf(const std::wstring &p) {
        if (p.size() <= 3) return LinkKind::None; // drive root has no dir entry
        WIN32_FIND_DATAW fd{};
        HANDLE hFind = FindFirstFileW(p.c_str(), &fd);
        if (hFind == INVALID_HANDLE_VALUE) return LinkKind::None;
        FindClose(hFind);
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return LinkKind::None;
        if (fd.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT) return LinkKind::Junction;
        if (fd.dwReserved0 == IO_REPARSE_TAG_SYMLINK) return LinkKind::Symlink;
        return LinkKind::None;
    }

    // Is this drive letter a subst / mapped drive rather than a real volume?
    // QueryDosDeviceW is a registry-ish lookup with no filesystem I/O, so it is
    // safe to call for every row: a real volume answers "\Device\HarddiskVolumeN",
    // a subst answers "\??\C:\some\path".
    static bool IsMappedDrive(const std::wstring &path) {
        if (path.size() < 2 || path[1] != L':') return false;
        const wchar_t drive[3] = {path[0], L':', L'\0'};
        wchar_t target[MAX_PATH] = {};
        if (QueryDosDeviceW(drive, target, MAX_PATH) == 0) return false;
        return wcsncmp(target, L"\\??\\", 4) == 0;
    }

    // Pure probe — writes no global state, so the background scan thread can call
    // it. GetLinkInfo() is the caching wrapper around it for UI-thread callers.
    static LinkInfo ProbeLink(const std::wstring &path) {
        LinkInfo info;
        if (HistoryPath::IsBroken(path))
            return info;

        // Testing only the last component is not enough: the link is usually
        // higher up — D:\12_Wallpapers is the junction, and every row beneath it
        // inherits the aliasing without being a reparse point itself. So walk UP
        // the path looking for the nearest component that is one.
        //
        // This walk uses GetFileAttributesW, which is a metadata query. The
        // obvious alternative — resolve every row with GetFinalPathNameByHandleW
        // and compare — needs a CreateFileW per row, on the UI thread, inside
        // BuildDisplayList; with a long history that is dozens of directory opens
        // per rebuild and a hard stall on any disconnected network path. Resolve
        // ONLY once something has already proven the row is an alias.
        std::wstring linkComponent;
        {
            std::wstring probe = path;
            while (probe.size() > 3) {
                const DWORD attrs = GetFileAttributesW(probe.c_str());
                if (attrs != INVALID_FILE_ATTRIBUTES &&
                    (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
                    linkComponent = probe;
                    break;
                }
                const size_t sep = probe.find_last_of(L'\\');
                if (sep == std::wstring::npos || sep < 3) break;
                probe.resize(sep);
            }
        }

        if (!linkComponent.empty()) {
            // Only now read the tag, and only for the one component that has one.
            // Other tags (cloud placeholder, dedup stub) are not links.
            info.kind = ReparseKindOf(linkComponent);
        } else if (IsMappedDrive(path)) {
            info.kind = LinkKind::Mapped;
        }

        if (info.kind != LinkKind::None)
            info.target = ResolveFinalPath(path); // one open, links only
        return info;
    }

    // Caching wrapper for UI-thread callers (hover popup, SameRealFolder).
    // BuildDisplayList deliberately does NOT use this — see CachedIsSymlink.
    static const LinkInfo &GetLinkInfo(const std::wstring &path) {
        auto it = g_symlinkCache.find(path);
        if (it != g_symlinkCache.end()) return it->second;
        return g_symlinkCache[path] = ProbeLink(path);
    }

    // Cache-only lookup: never touches the filesystem. Rows whose link state has
    // not been probed yet simply draw without the marker until the background
    // scan fills it in — exactly how FolderStatus::Unknown behaves.
    static bool CachedIsSymlink(const std::wstring &path) {
        auto it = g_symlinkCache.find(path);
        return it != g_symlinkCache.end() && it->second.kind != LinkKind::None;
    }

    // Real on-disk identity: the resolved target when the path is an alias,
    // otherwise the path itself.
    static std::wstring RealPathOf(const std::wstring &p) {
        const LinkInfo &li = GetLinkInfo(p);
        return (li.kind != LinkKind::None && !li.target.empty()) ? li.target : p;
    }

    bool SameRealFolder(const std::wstring &a, const std::wstring &b) {
        if (HistoryPath::Equal(a, b)) return true; // identical spelling — no I/O
        // Different spellings may still be one directory. GetLinkInfo caches, and
        // returns None without touching the disk for ordinary paths, so this stays
        // cheap for the overwhelmingly common "genuinely different folders" case.
        return HistoryPath::Equal(RealPathOf(a), RealPathOf(b));
    }

    // ---------------------------------------------------------------------------
    // Symlink hover popup
    //   Line 1: what kind of link this is
    //   Line 2: where it actually resolves to
    // A tracking tooltip rather than a hit-test-rect one: the rows scroll, so the
    // registered rectangle would have to be re-armed on every paint. Tracking mode
    // lets WM_MOUSEMOVE decide, which the panel is already doing for hover anyway.
    // ---------------------------------------------------------------------------

    // Row the popup is currently describing, -1 = hidden. Kept so a mouse move
    // within the same slot does not re-show and flicker.
    static int g_linkTipRow = -1;

    // Hover targets, filled during WM_PAINT. Declared here rather than down with
    // the other footer rects so RefreshHistory — which sits above them — can clear
    // them as part of the F5 rebuild.
    //
    // g_linkRects is index-parallel to g_displayList, holding an empty RECT for
    // rows with no badge and for rows scrolled out of view.
    static std::vector<RECT> g_linkRects;
    // Footer total: its rect, and the popup text built alongside it during paint
    // (that is where the byte / file / excluded numbers already exist).
    static RECT         g_summaryRect = {};
    static std::wstring g_summaryTipText;

    static void HideLinkTip() {
        if (g_linkTipRow >= 0) ThemedTooltip::Hide();
        g_linkTipRow = -1;
    }

    // `row` is only an identity token for the "already showing this" guard — a row
    // index, or a negative sentinel for non-row targets such as the footer total.
    // -1 is reserved for "hidden".
    //
    // anchorClient is the rect being explained, in CLIENT coords. ThemedTooltip
    // polls the cursor against it and dismisses itself, so no caller has to get
    // mouse-leave bookkeeping right.
    static void ShowLinkTip(HWND hOwner, int row, const std::wstring &text,
                            POINT ptClient, const RECT &anchorClient) {
        if (text.empty()) { HideLinkTip(); return; }

        // The popup may have dismissed itself since we last showed it, so the
        // "same target" guard has to account for it no longer being on screen.
        if (row == g_linkTipRow && ThemedTooltip::IsVisible()) return;

        POINT ptScreen = ptClient;
        ClientToScreen(hOwner, &ptScreen);
        ptScreen.x += 16; // clear of the cursor
        ptScreen.y += 18;

        RECT anchor = anchorClient;
        POINT tl{anchor.left, anchor.top};
        POINT br{anchor.right, anchor.bottom};
        ClientToScreen(hOwner, &tl);
        ClientToScreen(hOwner, &br);
        anchor = RECT{tl.x, tl.y, br.x, br.y};

        ThemedTooltip::Show(hOwner, text, ptScreen, anchor);
        g_linkTipRow = row;
    }

    // ---------------------------------------------------------------------------
    // ROW BADGES
    //
    // A row can be several things at once — missing AND starred, or a junction
    // that is also a favorite — but there is one glyph slot. Giving each fact its
    // own column does not scale and wastes width on the common unmarked row, and
    // the previous "status wins, else star" rule silently hid the star: pressing
    // Space on a missing folder appeared to do nothing.
    //
    // So: collect every badge the row carries. One badge draws directly. Two or
    // more draw a stack placeholder, and hovering it lists them all, one per line.
    // Hovering a single badge explains that one, so the affordance is uniform.
    // ---------------------------------------------------------------------------
    struct RowBadge {
        const wchar_t *icon;
        COLORREF color;
        std::wstring text; // one line in the hover popup
    };

    static std::vector<RowBadge> BuildRowBadges(const std::wstring &path, bool isFavorite) {
        std::vector<RowBadge> badges;

        auto sit = g_statusCache.find(path);
        const FolderStatus status = (sit != g_statusCache.end()) ? sit->second
                                                                 : FolderStatus::Unknown;
        if (status == FolderStatus::Missing) {
            badges.push_back({Constants::Icon::WARNING,
                              Constants::Theme::HistoryPanel::PATH_DEAD_DRIVE,
                              Constants::Messages::BADGE_MISSING});
        } else if (status == FolderStatus::Empty) {
            badges.push_back({Constants::Icon::EMPTY,
                              Constants::Theme::HistoryPanel::PATH_EMPTY_DRIVE,
                              Constants::Messages::BADGE_EMPTY});
        }

        if (isFavorite) {
            badges.push_back({Constants::Icon::FAVORITES_MARK,
                              Constants::Theme::Markers::FAVORITES,
                              Constants::Messages::BADGE_FAVORITE});
        }

        auto lit = g_symlinkCache.find(path);
        if (lit != g_symlinkCache.end() && lit->second.kind != LinkKind::None) {
            const LinkInfo &li = lit->second;
            std::wstring line =
                    li.kind == LinkKind::Junction ? Constants::Messages::LINK_KIND_JUNCTION
                    : li.kind == LinkKind::Symlink ? Constants::Messages::LINK_KIND_SYMLINK
                                                   : Constants::Messages::LINK_KIND_MAPPED;
            line += L"  ";
            line += Constants::Icon::ARROW_RIGHT;
            line += L"  ";
            line += li.target.empty() ? Constants::Messages::LINK_TARGET_UNKNOWN : li.target;
            badges.push_back({Constants::Icon::SYMLINK_MARK,
                              Constants::Theme::Markers::SYMLINK, std::move(line)});
        }

        return badges;
    }

    // Cache-only identity: the resolved target for an alias, the path itself
    // otherwise. Never probes the filesystem, so it is safe inside WM_PAINT.
    static const std::wstring &CachedRealPathOf(const std::wstring &p) {
        auto it = g_symlinkCache.find(p);
        if (it != g_symlinkCache.end() && it->second.kind != LinkKind::None &&
            !it->second.target.empty())
            return it->second.target;
        return p;
    }

    // Footer totals across the whole scanned cache, counting each REAL directory
    // once.
    //
    // A junction and its target are two rows describing one set of files on disk.
    // Adding both would report a library as twice its true size — the numbers have
    // to answer "how much data is there", not "how many ways can I reach it".
    //
    // Two passes so the ORIGINAL wins: a real folder is counted first, and an
    // alias only contributes when nothing else already accounted for its target.
    // That way a junction whose target is not in the list still counts (its files
    // are real and reachable), but never on top of the folder it points at.
    // Three reasons a scanned folder contributes nothing, each reported
    // separately so the popup can explain WHY rather than just how many.
    struct HistoryTotals {
        int64_t bytes = 0;
        int     files = 0;
        int     scanned = 0; // folders with a known status — counted PLUS excluded
        std::vector<std::wstring> duplicates; // alias — its target is counted already
        std::vector<std::wstring> missing;    // gone from disk
        std::vector<std::wstring> empty;      // exists, holds no supported images

        size_t excludedCount() const {
            return duplicates.size() + missing.size() + empty.size();
        }
    };

    // Totals are derived from the caches, so they only change when a scan result
    // lands or the list is rebuilt. WM_PAINT fires far more often than that — on
    // every hover change — and recomputing there walks the whole status cache,
    // builds a set, sorts three vectors and rebuilds the popup string. At a
    // thousand folders that is real work per mouse move. Compute once, reuse.
    static bool          g_totalsDirty = true;
    static HistoryTotals g_totalsCache;

    static void InvalidateTotals() { g_totalsDirty = true; }

    static HistoryTotals ComputeHistoryTotals() {
        HistoryTotals t;

        // Driven by the STATUS cache, not the size cache: a missing folder has no
        // size entry at all (ApplyDirScan erases it), so it would be invisible to
        // a size-only walk and could never be reported as excluded.
        std::vector<std::wstring> valid;
        for (const auto &[p, status]: g_statusCache) {
            switch (status) {
                case FolderStatus::Missing: t.missing.push_back(p); break;
                case FolderStatus::Empty:   t.empty.push_back(p);   break;
                case FolderStatus::Valid:   valid.push_back(p);     break;
                default: break; // Unknown — not scanned yet, neither counted nor excluded
            }
        }

        // Real folders first so the ORIGINAL always wins the slot, aliases second
        // so one only counts when nothing already accounted for its target.
        FolderPathSet counted;
        counted.reserve(valid.size());
        auto take = [&](const std::wstring &p) {
            auto sit = g_dirSizeCache.find(p);
            if (sit == g_dirSizeCache.end()) return;
            t.bytes += sit->second.bytes;
            t.files += sit->second.count;
        };

        for (const auto &p: valid) {
            if (CachedIsSymlink(p)) continue; // pass 2
            if (!counted.insert(CachedRealPathOf(p)).second) continue;
            take(p);
        }
        for (const auto &p: valid) {
            if (!CachedIsSymlink(p)) continue;
            if (!counted.insert(CachedRealPathOf(p)).second) {
                t.duplicates.push_back(p);
                continue;
            }
            take(p);
        }

        // Everything with a known status: what was counted plus what was skipped.
        // Deliberately excludes Unknown rows — nothing is known about them yet, so
        // claiming them in a total would be a guess.
        t.scanned = static_cast<int>(valid.size() + t.missing.size() + t.empty.size());

        std::sort(t.duplicates.begin(), t.duplicates.end());
        std::sort(t.missing.begin(), t.missing.end());
        std::sort(t.empty.begin(), t.empty.end());
        return t;
    }

    // Cached accessor — the only one paint should call.
    static const HistoryTotals &HistoryTotalsCached() {
        if (g_totalsDirty) {
            g_totalsCache = ComputeHistoryTotals();
            g_totalsDirty = false;
        }
        return g_totalsCache;
    }

    // One excluded group: heading, blank line, then "* <n>. <path>" per entry.
    //
    // `nextIndex` numbers CONTINUOUSLY across all groups — the count in the header
    // says "5 dirs excluded", so the list under it has to run 1..5 rather than
    // restarting per group and leaving the reader to add up.
    //
    // showTarget appends the resolved destination, which is the whole point for
    // the duplicates group: it names the folder whose total already absorbed this
    // one. Empty group means no heading at all, so only real reasons appear.
    static void AppendExcludedGroup(std::wstring &out, const std::wstring &heading,
                                    const std::vector<std::wstring> &paths,
                                    int &nextIndex, bool showTarget) {
        if (paths.empty()) return;
        out += L"\n" + heading + L"\n";
        for (const auto &p: paths) {
            out += L"\n";
            out += Constants::Messages::EXCLUDED_BULLET;
            out += std::to_wstring(nextIndex++) + L". " + p;
            if (showTarget) {
                const std::wstring &target = CachedRealPathOf(p);
                if (!HistoryPath::Equal(target, p)) {
                    out += L" ";
                    out += Constants::Icon::ARROW_RIGHT;
                    out += L" " + target;
                }
            }
        }
    }

    // Favorites, counted by REAL directory rather than by row.
    //
    // Same reasoning as ComputeHistoryTotals: starring D:\Wallpapers\[Set 8] and
    // E:\Wallpapers\[Set 8] marks ONE folder, reached two ways. Counting rows
    // would report "2 / 10 favorites" for a single folder and burn two slots of
    // the cap, so the limit would bite at half its stated size on a machine that
    // uses junctions.
    //
    // Falls back to the row's own path when the link state is not cached yet, so
    // an unprobed alias simply counts as itself until the sweep lands.
    static int UniqueFavoriteCount() {
        const auto &favs = historyFoldersManager.favorites;
        FolderPathSet real;
        real.reserve(favs.size());
        for (const auto &p: favs) real.insert(CachedRealPathOf(p));
        return static_cast<int>(real.size());
    }

    // "<icon>  <explanation>" per line — what the hover popup shows.
    static std::wstring BadgeTipText(const std::vector<RowBadge> &badges) {
        std::wstring text;
        for (const auto &b: badges) {
            if (!text.empty()) text += L"\n";
            text += b.icon;
            text += L"  ";
            text += b.text;
        }
        return text;
    }

    // One folder's scan outcome — computed off the UI thread, applied on it.
    // Carries the link state as well as the status, so "is it there / does it
    // hold images / is it an alias" are answered by ONE pass at the same two
    // moments: the background sweep after the list is built, and the folder open.
    struct DirScanResult {
        std::wstring path;
        FolderStatus status = FolderStatus::Unknown;
        int64_t      bytes  = 0;
        int          count  = 0;
        LinkInfo     link;
    };

    // Generation guard: bumped on each RefreshHistory so stale background results
    // (folder list changed, or a newer refresh already queued) are discarded.
    static std::atomic<uint64_t> g_histScanGen{0};

    // True while a background sweep is walking folders.
    //
    // Set on the UI thread when a sweep is launched, cleared when the worker's
    // completion message arrives — not by the worker itself, so it can never be
    // cleared before the last batch has been applied.
    static bool g_scanRunning = false;

    // How many folder scans the worker batches before posting to the UI thread.
    // Small enough that rows light up steadily on a long history, large enough
    // that the message traffic stays negligible.
    static constexpr size_t SCAN_BATCH = 25;

    // The volume a path lives on: "D:\" or "\\server\share".
    // Used to probe reachability ONCE per volume instead of once per folder.
    static std::wstring PathRoot(const std::wstring &p) {
        if (p.size() >= 2 && p[0] == L'\\' && p[1] == L'\\') {
            // UNC: keep \\server\share, which is the unit that goes offline.
            size_t slash = p.find(L'\\', 2);              // end of \\server
            if (slash == std::wstring::npos) return p;
            slash = p.find(L'\\', slash + 1);             // end of \share
            return (slash == std::wstring::npos) ? p : p.substr(0, slash);
        }
        if (p.size() >= 3 && p[1] == L':') return p.substr(0, 3); // "D:\"
        return {};
    }

    // Pure filesystem scan: count image files and sum their sizes.
    // Writes no global state — safe to call on a worker thread.
    static DirScanResult ComputeDirScan(const std::wstring &path) {
        namespace fs = std::filesystem;
        DirScanResult r;
        r.path = path;
        // Unparseable row kept for display — never hand it to the filesystem.
        // IsBroken is a pure string check, safe to call from this worker thread.
        if (HistoryPath::IsBroken(path)) {
            r.status = FolderStatus::Missing;
            return r;
        }
        // Probed here, on the worker, for the same reason the image count is:
        // it is filesystem work and does not belong on the UI thread.
        r.link = ProbeLink(path);
        std::error_code ec;
        if (!fs::is_directory(fs::path(path), ec) || ec) {
            r.status = FolderStatus::Missing;
            return r;
        }
        int64_t totalBytes = 0;
        int count = 0;
        for (auto it = fs::directory_iterator(fs::path(path),
                         fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file(ec)) { ec.clear(); continue; }
            if (!is_image_ext(it->path().extension().wstring())) continue;
            // directory_entry::file_size() reuses the WIN32_FIND_DATA captured by
            // the iterator on Windows — no extra per-file GetFileAttributesEx call
            // (fs::file_size(path) would re-stat each entry).
            auto sz = it->file_size(ec);
            if (!ec) totalBytes += static_cast<int64_t>(sz);
            ec.clear();
            ++count;
        }
        r.status = (count > 0) ? FolderStatus::Valid : FolderStatus::Empty;
        r.bytes  = totalBytes;
        r.count  = count;
        return r;
    }

    // Apply a scan result to the UI-thread-owned status/size caches. UI thread only.
    static void ApplyDirScan(const DirScanResult &r) {
        InvalidateTotals();              // any scan result changes the footer
        g_symlinkCache[r.path] = r.link; // link state travels with the status
        if (r.status == FolderStatus::Missing) {
            g_statusCache[r.path] = FolderStatus::Missing;
            g_dirSizeCache.erase(r.path);
            return;
        }
        g_statusCache[r.path]  = r.status;
        g_dirSizeCache[r.path] = {r.bytes, r.count};
    }

    // THE re-check. Everything qIV knows about one folder — does it exist, does it
    // hold images, how big is it, is it an alias — recomputed and applied in one
    // pass, so no caller has to remember which of the four caches to poke.
    //
    // Called when a folder is opened. The background sweep answers for the rows
    // that were on screen when the list was built; this covers everything else,
    // and it is the moment the answers matter most. Same two checkpoints for all
    // four facts, which is the point.
    //
    // UI thread only — it writes the caches.
    static void RevalidateFolder(const std::wstring &path) {
        auto it = g_symlinkCache.find(path);
        const bool hadLink = (it != g_symlinkCache.end());
        const LinkKind linkBefore = hadLink ? it->second.kind : LinkKind::None;

        InvalidateTotals();
        g_statusCache.erase(path);  // erase first: GetFolderStatus and friends must
        g_dirSizeCache.erase(path); // not serve a stale answer if this re-entered
        g_symlinkCache.erase(path);

        const DirScanResult r = ComputeDirScan(path);

        // A junction almost always lives on a PARENT component (D:\12_Wallpapers,
        // not D:\12_Wallpapers\[Set 8]). If THIS folder's link state flipped, every
        // row beneath that parent was cached under an assumption that no longer
        // holds — drop the lot and let the sweep and later opens refill it.
        if (hadLink && r.link.kind != linkBefore)
            g_symlinkCache.clear();

        ApplyDirScan(r);
    }

    // Scan a folder synchronously and apply the result (UI thread convenience).
    static void ScanDirForHistory(const std::wstring &path) {
        ApplyDirScan(ComputeDirScan(path));
    }

    // Set whenever the display list changes (push, delete, restore, clear).
    // Background validation only runs when this is true, preventing redundant
    // filesystem work on rapid Tab / Tab / Tab presses.

    static FolderStatus GetFolderStatus(const std::wstring &path) {
        auto it = g_statusCache.find(path);
        if (it != g_statusCache.end() && it->second != FolderStatus::Unknown)
            return it->second;

        // Falling through means this call is about to WRITE a status, which moves
        // a folder between the counted and excluded groups.
        InvalidateTotals();

        // A line the loader could not parse is kept in the list so the user can
        // see it, and reported as Missing: it paints in the dead-folder colour and
        // navigation steps over it, which is exactly right for "this row does not
        // name a folder". Checked before touching the filesystem — the string may
        // contain characters no Win32 path call should ever be handed.
        if (HistoryPath::IsBroken(path))
            return g_statusCache[path] = FolderStatus::Missing;

        namespace fs = std::filesystem;
        std::error_code ec;
        if (!fs::is_directory(fs::path(path), ec) || ec) {
            return g_statusCache[path] = FolderStatus::Missing;
        }
        // Non-throwing iteration: it.increment(ec) instead of range-for, whose
        // operator++() throws filesystem_error on transient I/O errors. This
        // runs on the UI thread (Enter/click handlers) — a throw here would
        // escape the wndproc and terminate the process (0xC0000409).
        for (auto dirIt = fs::directory_iterator(
                     fs::path(path), fs::directory_options::skip_permission_denied, ec);
             !ec && dirIt != fs::directory_iterator(); dirIt.increment(ec)) {
            if (!dirIt->is_regular_file(ec)) {
                ec.clear();
                continue;
            }
            std::wstring ext = dirIt->path().extension().wstring();
            for (auto &c: ext) c = static_cast<wchar_t>(::towlower(c));
            for (size_t i = 0; i < Constants::Registry::SUPPORTED_EXTENSIONS_COUNT; ++i) {
                if (ext == Constants::Registry::SUPPORTED_EXTENSIONS[i])
                    return g_statusCache[path] = FolderStatus::Valid;
            }
        }
        return g_statusCache[path] = FolderStatus::Empty;
    }

    // ---------------------------------------------------------------------------
    // CalcTotalContentH — single formula for virtual scroll height used in
    // GetHistoryWindowBounds, WM_PAINT, WM_LBUTTONDOWN and WM_MOUSEMOVE.
    // Includes header area + rows + footer so window sizing and scroll are consistent.
    // ---------------------------------------------------------------------------
    static int CalcTotalContentH(int nEntries, UINT dpi) {
        int padding    = MulDiv(Constants::History::HISTORY_PADDING,     dpi, 96);
        int rowH       = MulDiv(Constants::History::HISTORY_ROW_HEIGHT,  dpi, 96);
        int footerH    = MulDiv(Constants::History::HISTORY_FONT_SIZE + 2 + 8, dpi, 96);
        int filterRowH = MulDiv(Constants::History::HISTORY_FILTER_ROW_H, dpi, 96);
        return padding * 2
               + MulDiv(2 * Constants::History::HISTORY_FONT_SIZE + 16, dpi, 96)
               + MulDiv(8, dpi, 96)
               + nEntries * rowH
               + footerH
               + 1           // separator between footer row and filter row
               + filterRowH;
    }

    static void BuildDisplayList();
    static void GetHistoryWindowBounds(HWND hRef, int &x, int &y, int &w, int &h);
    void InvalidateWalkSnapshot();
    static std::wstring CurrentOpenFolder();

    // The folder the app was last told to open, recorded by PushFolderHistory —
    // the single funnel every navigation passes through. Stored in the LIST's
    // spelling, so it can be compared with list entries directly.
    static std::wstring g_lastNavigatedFolder;

    // Set by the walk around its own OpenDirectory() call, so PushFolderHistory
    // can tell "the walk moved us" from "something else moved us".
    //
    // This deliberately does NOT compare paths. OpenDirectory() runs
    // fs::canonical(), which resolves junctions, subst drives and symlinks — the
    // path that comes back can be a completely different string from the one the
    // history list holds for the same folder. Comparing them made the walk think
    // the user had navigated away on every single press, which rebuilt the
    // snapshot against a freshly reordered MRU list and left the cursor pointing
    // into rows that had shifted underneath it.
    static bool g_walkOwnsNavigation = false;

    // Latched by PushFolderHistory when a navigation happened that the walk did
    // not perform. Consumed (and cleared) by the next walk step.
    static bool g_externalNavigation = false;

    // THE answer to "which folder is the app in?" for everything in this file:
    // the green current-folder row, the initial hover position, and the walk.
    //
    // Deriving it from app.playlist is wrong for empty folders — OpenDirectory
    // returns early for those without touching the playlist, so the playlist goes
    // on describing the folder you were in BEFORE. That is why navigating into an
    // empty folder used to paint the previous row green.
    static std::wstring AppCurrentFolder();

    // Set when the hover row was moved programmatically rather than by the user.
    // WM_PAINT consumes it and scrolls that row into view — the scroll maths needs
    // g_rowH / g_bodyTop / g_bodyBottom, and those are only known once the panel
    // has laid itself out, which has not happened yet at Show() time.
    static bool g_scrollHoverIntoView = false;

    // Lands the selection on the folder currently open in the viewer — the row
    // painted green — instead of the top of the list. Opening the panel to look
    // at where you are is the common case; starting at row 0 means scrolling back
    // to your own position every time. Falls back to the first row when the
    // current folder is not in the list (nothing open yet, or filtered out).
    // Defined below g_displayList, which it reads.
    static void HoverCurrentFolderRow();

    // Rebuilds the rows for the current EffectiveFullMode() and refits the window.
    // Split out from ToggleFullHistory so the tray item — which flips the same
    // AppState flag from outside this file — can produce the same visible result
    // instead of leaving a stale list on screen until the next repaint.
    static void ApplyFullHistoryMode(HWND hWnd) {
        g_view.scrollY = 0;
        BuildDisplayList();
        HoverCurrentFolderRow();
        int x, y, w, h;
        GetHistoryWindowBounds(g_hHistOwner ? g_hHistOwner : hWnd, x, y, w, h);
        SetWindowPos(hWnd, HWND_TOPMOST, x, y, w, h, SWP_FRAMECHANGED);
        InvalidateRect(hWnd, nullptr, TRUE);
    }

    // Ctrl+Tab while the panel has focus — a deliberate mode switch, so it does
    // write the preference. Toggling relative to what is ON SCREEN (not to the
    // stored flag) keeps it correct when the panel was opened via the one-shot
    // full-list override: the first press then switches to short and records it.
    static void ToggleFullHistory(HWND hWnd) {
        const bool nowFull = !EffectiveFullMode();
        g_fullModeOverride = false; // an explicit choice supersedes the one-shot view
        SetFullHistoryMode(nowFull);
        ApplyFullHistoryMode(hWnd);
    }

    // Scan every history folder OFF the UI thread — status, size and link state in
    // one pass. A folder with thousands of images needs a full directory_iterator
    // to count, which would otherwise freeze the panel on a disk-heavy history.
    // The worker only reads the filesystem into a local vector; the caches are
    // mutated solely on the UI thread in the WM_QIV_HISTORY_VALIDATED handler.
    // A generation guard discards results from a superseded run.
    //
    // Must run on EVERY open, not just F5: the row markers are read live from the
    // caches now, so without this the first Tab shows a list with no missing /
    // empty / link marks until something else happened to populate them.
    static void LaunchHistoryValidation(HWND hWnd, bool rescanAll, DWORD delayMs = 0) {
        // Only folders that need work. Opening the panel must not re-walk a
        // thousand directories that were already scanned this session — that cost
        // is paid once, or on F5 when the user explicitly asks for fresh answers.
        std::vector<std::wstring> folders;
        folders.reserve(historyFoldersManager.folderHistory.size());
        for (const auto &p: historyFoldersManager.folderHistory) {
            if (!rescanAll) {
                auto it = g_statusCache.find(p);
                if (it != g_statusCache.end() && it->second != FolderStatus::Unknown)
                    continue; // already known
            }
            folders.push_back(p);
        }
        if (folders.empty()) return;

        g_scanRunning = true;
        const uint64_t gen = g_histScanGen.fetch_add(1, std::memory_order_relaxed) + 1;
        std::thread([folders = std::move(folders), gen, hWnd, delayMs]() mutable {
            // A DELAYED sweep is the startup prefetch: nobody is waiting for it, so
            // it must not compete with the app's own startup I/O. Background mode
            // drops this thread's CPU *and* disk priority, so a thousand directory
            // walks cannot out-queue the read of the image the user is watching
            // for, and the delay keeps it off the disk entirely until that image
            // is on screen.
            //
            // An IMMEDIATE sweep — opening the panel, or F5 — is user-initiated and
            // the user is looking at it. Lowering its priority would be actively
            // wrong: it would make the one scan they explicitly asked for the
            // slowest one the app performs.
            const bool prefetch = (delayMs > 0);
            struct BgGuard {
                bool on;
                ~BgGuard() { if (on) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END); }
            } bgGuard{prefetch};

            if (prefetch) {
                Sleep(delayMs);
                SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
            }

            // Re-check after the delay: the user may have hit F5, or closed qIV.
            if (g_histScanGen.load(std::memory_order_relaxed) != gen) return;
            // Delivered in batches rather than one payload at the end. With a long
            // history the old all-or-nothing post meant a single slow folder held
            // back every other row's marker until the whole sweep finished; now the
            // list fills in as the answers arrive.
            auto *batch = new std::vector<DirScanResult>();
            batch->reserve(SCAN_BATCH);

            // Returns false when the sweep should stop. On every failure path the
            // batch is freed and nulled here, so the caller never has to reason
            // about who owns it — only the successful PostMessage hands ownership
            // to the UI thread.
            auto flush = [&]() -> bool {
                if (!batch) return false;
                if (batch->empty()) return true;
                if (g_histScanGen.load(std::memory_order_relaxed) != gen ||
                    !PostMessageW(hWnd, Constants::WM_QIV_HISTORY_VALIDATED,
                                  static_cast<WPARAM>(gen),
                                  reinterpret_cast<LPARAM>(batch))) {
                    delete batch;
                    batch = nullptr;
                    return false;
                }
                batch = new std::vector<DirScanResult>();
                batch->reserve(SCAN_BATCH);
                return true;
            };

            // Reachability, decided ONCE per volume.
            //
            // An unreachable share makes every filesystem call against it block
            // for the SMB timeout — tens of seconds each. With a folder-at-a-time
            // sweep, twenty rows on one dead server used to mean twenty timeouts
            // in series, and every folder queued behind them waited too. Probing
            // the volume root once turns that into a single timeout, after which
            // the rest of that volume is reported Missing immediately and the
            // sweep moves on to volumes that answer.
            std::unordered_map<std::wstring, bool,
                               HistoryPath::HashCI, HistoryPath::EqualCI> rootReachable;

            for (const auto &p: folders) {
                if (g_histScanGen.load(std::memory_order_relaxed) != gen) break;

                bool skip = false;
                if (const std::wstring root = PathRoot(p); !root.empty()) {
                    auto rit = rootReachable.find(root);
                    if (rit == rootReachable.end()) {
                        const DWORD a = GetFileAttributesW(root.c_str());
                        rit = rootReachable.emplace(root, a != INVALID_FILE_ATTRIBUTES).first;
                    }
                    skip = !rit->second;
                }

                if (skip) {
                    DirScanResult r;
                    r.path = p;
                    r.status = FolderStatus::Missing; // volume is not answering
                    batch->push_back(std::move(r));
                } else {
                    batch->push_back(ComputeDirScan(p));
                }
                if (batch->size() >= SCAN_BATCH && !flush()) return;
            }
            flush();
            delete batch; // trailing empty batch, or nullptr after a failed flush

            // Completion signal: a null payload. Posted last, so the UI clears the
            // "Loading" state only after every batch ahead of it has been applied.
            PostMessageW(hWnd, Constants::WM_QIV_HISTORY_VALIDATED,
                         static_cast<WPARAM>(gen), 0);
        }).detach();
    }

    static void RefreshHistory(HWND hWnd) {
        // F5 means "re-read the world" — a FORCED rebuild that reuses nothing.
        //
        // A full RELOAD, not the two-way merge: merging only ever adds, so a line
        // the user deleted by hand from qivHistory.txt would survive in RAM and
        // reappear on the next save. Reloading makes the file authoritative, which
        // is what "refresh" has to mean for a file the user can edit.
        //
        // Flush first: appends are queued on the write thread, so a folder visited
        // moments ago may not have reached the file yet. Reloading without this
        // would read a file that does not contain it and silently drop it.
        g_writeQueue.Flush();
        historyFoldersManager.LoadHistoryFromDisk();

        // Every cache dropped, not merely refreshed: a folder can be deleted,
        // emptied, refilled, or turned into a junction while qIV is running, and
        // entries for folders that have since left the list would otherwise linger
        // and keep skewing the footer totals. Rows go unmarked for a moment and
        // the batched sweep refills them — image counts and sizes included, since
        // ComputeDirScan re-walks every folder from scratch.
        g_statusCache.clear();
        g_dirSizeCache.clear();
        g_symlinkCache.clear();
        InvalidateTotals();
        InvalidateWalkSnapshot(); // the row set may have changed underneath a walk

        // Derived UI state goes too. A popup on screen right now was built from
        // the caches just discarded, so leaving it up would show pre-refresh
        // numbers and paths until the cursor happened to move. The rects are
        // rebuilt by the next paint, the texts by the next hover.
        HideLinkTip();
        g_summaryTipText.clear();
        g_summaryRect = RECT{0, 0, 0, 0};
        g_linkRects.clear();

        BuildDisplayList();

        // Selection restarts too: row indices mean nothing across a rebuild, and
        // a saved row could now be past the end of a shorter list.
        g_savedHoverRow = -1;
        HoverCurrentFolderRow(); // re-lands on the folder actually open, scrolls to it
        int x, y, w, h;
        GetHistoryWindowBounds(g_hHistOwner ? g_hHistOwner : hWnd, x, y, w, h);
        SetWindowPos(hWnd, HWND_TOPMOST, x, y, w, h, SWP_FRAMECHANGED);

        LaunchHistoryValidation(hWnd, /*rescanAll=*/true); // F5 = re-read the world

        // Confirm the keypress. The visible result — status / link markers, sizes,
        // totals — arrives from the background scan a moment later, so without
        // this F5 looks like it did nothing at all.
        // Posted to the OWNER: the centre overlay belongs to the main viewer, not
        // to this panel.
        g_overlayManager.PostCenterMessage(
                g_hHistOwner ? g_hHistOwner : hWnd,
                std::wstring(Constants::Messages::HISTORY_REFRESHED_MSG) +
                        std::to_wstring(historyFoldersManager.folderHistory.size()));

        InvalidateRect(hWnd, nullptr, TRUE);
    }

    // Convert a VK_Fx code to its display label ("F3", "F5", …).
    static std::wstring FKeyLabel(UINT vk) {
        if (vk >= VK_F1 && vk <= VK_F24)
            return L"F" + std::to_wstring(vk - VK_F1 + 1);
        UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        wchar_t buf[16] = {};
        GetKeyNameTextW(static_cast<LONG>(sc) << 16, buf, 16);
        return buf;
    }

    static std::vector<RECT> g_rowRects;
    static std::vector<RECT> g_indexRects; // clickable rects for directory indexes (parallel to g_displayList)
    static RECT g_exeLinkRect = {};        // clickable rect for the "QIV" exe-dir link
    static RECT g_f5IndexRect = {};        // clickable rect for [Fkey] Dir toggle in footer
    static RECT g_cacheIndexRect = {};     // clickable rect for [Fkey] Cache toggle in footer
    static RECT g_shortcutF5Rect = {};     // clickable "F5" in the shortcuts hint line
    static RECT g_shortcutCtrlTabRect = {};// clickable "Ctrl+Tab" in the shortcuts hint line
    static UI::InputBox g_filter;          // filter input — owns text, ✕ button, keyboard/mouse

    // Display list: what the panel actually renders.
    //
    // Link state is deliberately NOT stored here. It is read live from
    // g_symlinkCache at paint time, exactly like FolderStatus is: the background
    // sweep fills those caches AFTER the list is built, and nothing rebuilds the
    // list when its results arrive. A copy taken at build time would therefore be
    // frozen at "not a link" — which is why the 🔗 markers vanished after F5.
    struct DisplayEntry {
        std::wstring path;
        bool isFavorite;
        int  matchPositions[Common::FUZZY_MAX_QUERY] = {};
        int  matchPosCount  = 0;
    };

    static std::vector<DisplayEntry> g_displayList;

    // Declared above — see there for why the hover starts on the current folder.
    static void HoverCurrentFolderRow() {
        g_hoverRow = g_displayList.empty() ? -1 : 0;
        const std::wstring current = AppCurrentFolder();
        if (!current.empty()) {
            for (int i = 0; i < static_cast<int>(g_displayList.size()); ++i) {
                if (HistoryPath::Equal(g_displayList[i].path, current)) {
                    g_hoverRow = i;
                    break;
                }
            }
        }
        g_scrollHoverIntoView = true;
    }

    // ---------------------------------------------------------------------------
    // BuildDisplayList
    //   Rebuilds g_displayList from the current MRU vector + favorites set.
    //   Respects HISTORY_FAVORITES_POSITION and per-category display caps.
    // ---------------------------------------------------------------------------
    static void BuildDisplayList() {
        // Preserve known statuses — only clear entries that are no longer in the list.
        // Unknown entries get validated lazily by the background timer.
        g_displayList.clear();

        const auto &history = historyFoldersManager.folderHistory;
        g_displayList.reserve(history.size());
        const auto &favSet = historyFoldersManager.favorites;
        // Last line of defence against a duplicate row reaching the panel. The
        // loader and PushFolderHistory both dedupe already, but this list is the
        // one thing the user actually sees and the one the walk steps through —
        // a repeated row there would make a walk appear to stall on one folder.
        FolderPathSet emitted;
        emitted.reserve(history.size());
        auto alreadyEmitted = [&emitted](const std::wstring &p) {
            return !emitted.insert(p).second;
        };
        const int favPos = Constants::History::HISTORY_FAVORITES_POSITION;
        // TWO CAPS, TWO SETTINGS, and they are not the ones the old names
        // suggested. historyMaxFavs caps how many favorites may EXIST (it
        // refuses the star when full); historyMaxFavsShown caps how many are
        // DRAWN here, which is what historyMaxDirs does for normal rows.
        // This line used to be a hardcoded INT_MAX with a comment saying
        // favorites are always shown in full — that was the missing setting,
        // not a decision.
        //
        // Full-history mode lifts BOTH, for the same reason: it is the "show me
        // everything" view, and a starred row still hidden inside it would read
        // as the favorite having been lost.
        const int maxNormal = EffectiveFullMode() ? INT_MAX : app.historyMaxDirs;
        const int maxFavs   = EffectiveFullMode() ? INT_MAX : app.historyMaxFavsShown;

        if (favPos == 2) {
            // In-place: iterate MRU order, count normals and favs separately
            int normalCount = 0;
            int favCount = 0;
            for (const auto &path: history) {
                if (alreadyEmitted(path)) continue;
                bool isFav = (favSet.count(path) > 0);
                if (isFav) {
                    if (favCount >= maxFavs) continue;
                    ++favCount;
                } else {
                    if (normalCount >= maxNormal) continue;
                    ++normalCount;
                }
                g_displayList.push_back({path, isFav});
                if (normalCount >= maxNormal && favCount >= maxFavs)
                    break;
            }
        } else {
            // Separate favorites and normals, then combine
            std::vector<DisplayEntry> favRows;
            std::vector<DisplayEntry> normalRows;
            favRows.reserve(history.size());
            normalRows.reserve(history.size());

            for (const auto &path: history) {
                if (alreadyEmitted(path)) continue;
                bool isFav = (favSet.count(path) > 0);
                if (isFav && static_cast<int>(favRows.size()) < maxFavs)
                    favRows.push_back({path, true});
                else if (!isFav && static_cast<int>(normalRows.size()) < maxNormal)
                    normalRows.push_back({path, false});
            }

            if (favPos == 0) {
                // Favorites on top
                for (auto &e: favRows) g_displayList.push_back(std::move(e));
                for (auto &e: normalRows) g_displayList.push_back(std::move(e));
            } else {
                // Favorites on bottom
                for (auto &e: normalRows) g_displayList.push_back(std::move(e));
                for (auto &e: favRows) g_displayList.push_back(std::move(e));
            }
        }
        // Mark unchecked entries as Unknown so WM_PAINT shows them in neutral colour.
        for (const auto &e: g_displayList)
            g_statusCache.try_emplace(e.path, FolderStatus::Unknown);

        // Apply live filter — wildcard or fuzzy match on full path, MRU order preserved.
        if (!g_filter.IsEmpty()) {
            int qLen = static_cast<int>(
                std::min(g_filter.GetText().size(),
                         static_cast<size_t>(Common::FUZZY_MAX_QUERY)));
            wchar_t lq[Common::FUZZY_MAX_QUERY + 1];
            Common::LowerCopy(g_filter.GetText().c_str(), qLen, lq);

            const bool hasWildcard = Common::IsWildcardQuery(lq, qLen);

            g_displayList.erase(
                std::remove_if(g_displayList.begin(), g_displayList.end(),
                    [&](DisplayEntry &e) {
                        int pLen = static_cast<int>(
                            std::min(e.path.size(), static_cast<size_t>(1023)));
                        wchar_t lpath[1024];
                        Common::LowerCopy(e.path.c_str(), pLen, lpath);
                        if (hasWildcard) {
                            Common::FuzzyMatchResult wm;
                            if (!Common::WildcardMatch(lq, lpath, &wm))
                                return true;
                            e.matchPosCount = wm.posCount;
                            for (int i = 0; i < wm.posCount; ++i)
                                e.matchPositions[i] = wm.positions[i];
                            return false;
                        }
                        Common::FuzzyMatchResult fm;
                        if (!Common::FuzzyMatch(lq, qLen, lpath, pLen, fm))
                            return true;
                        e.matchPosCount = fm.posCount;
                        for (int i = 0; i < fm.posCount; ++i)
                            e.matchPositions[i] = fm.positions[i];
                        return false;
                    }),
                g_displayList.end());
        }
    }

    // ---------------------------------------------------------------------------
    // Free functions — used by FileHandler, UIManager, CommandExecuter
    // ---------------------------------------------------------------------------

    bool IsFolderValidForViewer(const std::wstring &folderPath) {
        return GetFolderStatus(folderPath) == FolderStatus::Valid;
    }

    void InvalidateHistoryFolderStatus(const std::wstring &path) {
        g_statusCache[path] = FolderStatus::Missing;
        HistoryListWnd &hw = uiManager.getHistoryListWindow();
        HWND hwnd = hw.GetHwnd();
        if (hwnd) InvalidateRect(hwnd, nullptr, FALSE);
    }

    void NotifyFolderContentsChanged(const std::wstring &path) {
        RevalidateFolder(path); // status + size + link, one pass
        HistoryListWnd &hw = uiManager.getHistoryListWindow();
        HWND hwnd = hw.GetHwnd();
        if (hwnd) InvalidateRect(hwnd, nullptr, FALSE);
    }

    void StartBackgroundHistoryScan() {
        HWND h = uiManager.getHistoryListWindow().GetHwnd();
        if (!h) return; // panel not created yet — Show() will kick it off instead
        // Build the row set first: the sweep walks folderHistory, and BuildDisplayList
        // is what seeds g_statusCache with Unknown entries for them.
        BuildDisplayList();
        LaunchHistoryValidation(h, /*rescanAll=*/false,
                                Constants::History::HISTORY_SCAN_STARTUP_DELAY_MS);
    }

    void LoadFolderHistoryFromDisk() {
        historyFoldersManager.LoadHistoryFromDisk();
        // No full/short seeding needed — app.historyFullModeEnabled is already
        // loaded from the registry by RegistryManager and is read directly.
    }

    void PushFolderHistory(const std::wstring &rawFolderPath, bool folderHasImages) {
        // Normalize on the way in, exactly as the disk loader does, so a path
        // arriving from drag-drop, the command line or the shell cannot create a
        // second row that differs only by case, a trailing backslash, quotes or
        // stray whitespace.
        std::wstring folderPath;
        if (!HistoryPath::Normalize(rawFolderPath, folderPath))
            return;

        auto &history = historyFoldersManager.folderHistory;
        // Case-insensitive: Windows folders are, so operator== is the wrong test.
        auto it = std::find_if(history.begin(), history.end(),
                               [&](const std::wstring &p) {
                                   return HistoryPath::Equal(p, folderPath);
                               });

        // TWO SETTINGS DECIDE WHETHER THIS NAVIGATION IS RECORDED:
        //
        //   historyEnabled     off  — record nothing at all
        //   historyImagesOnly  on   — record only folders that hold images
        //
        // When neither allows it, nothing enters the RAM list and nothing is
        // appended to qivHistory.txt. Existing rows are left exactly as they
        // are, INCLUDING THEIR ORDER: promoting an entry to the front is
        // recording too, and a switch that says "stop recording" must not
        // quietly rewrite the MRU.
        //
        // THE FUNCTION STILL RUNS TO THE END, and that is the whole subtlety
        // here. g_lastNavigatedFolder below is not history — it is the answer to
        // "which folder is the app in?", and WalkOrigin (the Alt-arrow folder
        // walk), the blank-screen placeholder and the overlay folder line all
        // read it. An early return here would switch off navigation along with
        // the history and look exactly like the walk keys being dead.
        //
        // folderHasImages is not probed here. Every caller already knows —
        // OpenDirectory's synchronous first-image scan is what decides which of
        // its two PushFolderHistory calls runs — so asking again would be a
        // directory enumeration per navigation on the UI thread, which is
        // exactly the kind of work that must never touch the open path.
        const bool record = app.historyEnabled &&
                            (folderHasImages || !app.historyImagesOnly);
        if (!record) {
            // The STORED spelling when this folder is already known, for the
            // same reason the recording path below keeps it: the value is
            // compared against list entries downstream, and fs::canonical() may
            // have handed us a different name for the same folder. `it` is the
            // search that was already done above — no second lookup.
            g_lastNavigatedFolder = (it != history.end()) ? *it : folderPath;
        } else if (it != history.end()) {
            // Already exists: promote to front (MRU), no file write needed.
            // Keeps the stored spelling rather than the incoming one, so the row
            // does not flicker between casings as the same folder is revisited.
            std::wstring tmp = *it;
            history.erase(it);
            history.insert(history.begin(), tmp);
            // Record the STORED spelling, not the incoming one — this value is
            // compared against list entries, and fs::canonical() upstream may
            // have handed us a different name for the same folder.
            g_lastNavigatedFolder = tmp;
        } else {
            // Genuinely new: prepend to RAM list
            history.insert(history.begin(), folderPath);
            if (static_cast<int>(history.size()) > app.historyMaxDirsSave)
                history.resize(static_cast<size_t>(app.historyMaxDirsSave));
            historyFoldersManager.AppendNewFolderToDisk(folderPath);
            g_lastNavigatedFolder = folderPath;
        }

        // Where the app was last told to go. EVERY navigation funnels through
        // here — the walk keys, the wheel, Enter in this panel, F2, drag-drop,
        // the command line — which makes this the one dependable answer to
        // "which folder is the app in?".
        //
        // app.playlist is NOT that answer: OpenDirectory's empty-directory branch
        // returns without touching the playlist, so after navigating into an empty
        // folder the playlist still describes the PREVIOUS one.
        //
        // If this navigation was not the walk's own doing, latch it so the walk
        // knows to resync instead of trusting its cursor.
        if (!g_walkOwnsNavigation)
            g_externalNavigation = true;

        // Repaint only — the row ORDER on screen is deliberately left alone while
        // the panel is open, so the list does not reshuffle under the user on
        // every step of a walk. Only the green "you are here" marker moves.
        auto &histWnd = uiManager.getHistoryListWindow();
        if (histWnd.IsVisible())
            InvalidateRect(histWnd.GetHwnd(), nullptr, FALSE);
    }

    // The public face of AppCurrentFolder — see the header for why the folder
    // walk cannot ask app.playlist instead.
    std::wstring CurrentFolder() {
        return AppCurrentFolder();
    }

    void NotifyCurrentFolder(const std::wstring &folderPath) {
        std::wstring norm;
        if (!HistoryPath::Normalize(folderPath, norm)) return;

        // Deliberately does NOT touch g_externalNavigation: this reports where the
        // viewer ended up, it does not mean the user chose to go somewhere. The
        // walk's own landings arrive here too, and treating them as external
        // would make it resync away from its own cursor.
        g_lastNavigatedFolder = norm;

        auto &histWnd = uiManager.getHistoryListWindow();
        if (histWnd.IsVisible())
            InvalidateRect(histWnd.GetHwnd(), nullptr, FALSE);
    }

    void ToggleFavorite(int rowIndex) {
        if (rowIndex < 0 || rowIndex >= static_cast<int>(g_displayList.size()))
            return;

        const std::wstring &path = g_displayList[rowIndex].path;
        auto &favSet = historyFoldersManager.favorites;

        if (favSet.count(path) > 0) {
            favSet.erase(path);
        } else {
            // Cap on unique folders, not rows — see UniqueFavoriteCount.
            if (UniqueFavoriteCount() >= app.historyMaxFavs) {
                g_overlayManager.PostCenterMessage(
                    g_hHistOwner,
                    L"Favorites full (" + std::to_wstring(app.historyMaxFavs) + L" max)");
                return;
            }
            favSet.insert(path);
        }

        // Only rewrite the small favorites file — history file is untouched
        historyFoldersManager.RewriteFavoritesToDisk();
        // The row moved between categories, so any frozen walk is now wrong.
        InvalidateWalkSnapshot();
    }

    void ClearHistoryKeepFavorites() {
        // Backup first — before any RAM or file change
        historyFoldersManager.BackupHistoryToDisk();

        auto &history = historyFoldersManager.folderHistory;
        const auto &favSet = historyFoldersManager.favorites;

        // Remove all non-favorite entries from the MRU vector
        history.erase(
                std::remove_if(history.begin(), history.end(),
                               [&](const std::wstring &p) {
                                   return favSet.count(p) == 0;
                               }),
                history.end()
                );

        // Rewrite history file only — favorites file is untouched
        historyFoldersManager.RewriteHistoryToDisk();
        g_hoverRow = -1;
        InvalidateWalkSnapshot(); // rows disappeared
        InvalidateTotals();       // and so did their bytes and file counts
    }

    std::wstring HistoryFilePath() {
        return historyFoldersManager.GetFilePath();
    }

    // ── Maintenance: prune the history list ─────────────────────────────────
    //
    // Both of these BACK UP FIRST, exactly as ClearHistoryKeepFavorites does.
    // qivHistory.txt is the only copy of this list — there is no undo, and a
    // user who mis-clicks one of these has lost a folder trail built over
    // months. The backup is the undo.
    //
    // NEITHER TOUCHES FAVORITES. Starring a folder is an explicit act, and a
    // cleanup that silently dropped a starred entry because the drive was
    // unplugged today would be a data-loss bug wearing a tidy-up label.

    HistoryCleanupResult RemoveInvalidHistoryEntries() {
        HistoryCleanupResult r;
        historyFoldersManager.BackupHistoryToDisk();

        auto &history = historyFoldersManager.folderHistory;
        const auto &favSet = historyFoldersManager.favorites;

        std::vector<std::wstring> keep;
        keep.reserve(history.size());

        for (const std::wstring &path : history) {
            // A starred row survives whatever its state — see the note above.
            if (favSet.count(path)) {
                keep.push_back(path);
                continue;
            }

            // UNPARSEABLE: a pure string test, so this half is always exact and
            // needs no disk at all. It is what catches "C;\asfdasfd" — a typo'd
            // colon, a stray quote, a line that is not a path in any reading.
            if (HistoryPath::IsBroken(path)) {
                ++r.unparseable;
                continue;
            }

            // MISSING / EMPTY come from the background sweep's cache, never from
            // a fresh probe. Re-checking here would be one directory enumeration
            // per row, synchronously, on a list that can hold thousands — over
            // an unplugged network share that is a frozen window, not a cleanup.
            auto sit = g_statusCache.find(path);
            const FolderStatus status = (sit != g_statusCache.end()) ? sit->second
                                                                     : FolderStatus::Unknown;
            switch (status) {
                case FolderStatus::Missing: ++r.missing; continue;
                case FolderStatus::Empty:   ++r.empty;   continue;
                case FolderStatus::Unknown:
                    // NOT YET SCANNED, so NOT removed. Unknown means "no answer",
                    // and deleting on no answer is how a cleanup eats a list whose
                    // scan had not finished. Counted so the report can say the run
                    // was partial rather than quietly look complete.
                    ++r.unchecked;
                    break;
                case FolderStatus::Valid:
                    break;
            }
            keep.push_back(path);
        }

        r.removed = static_cast<int>(history.size() - keep.size());
        if (r.removed > 0) {
            history.swap(keep);
            historyFoldersManager.RewriteHistoryToDisk();
            g_hoverRow = -1;
            InvalidateWalkSnapshot();
            InvalidateTotals();
        }
        return r;
    }

    int RemoveDuplicateHistoryEntries() {
        historyFoldersManager.BackupHistoryToDisk();

        auto &history = historyFoldersManager.folderHistory;

        // FIRST occurrence wins, and the list is MRU-ordered, so the copy that
        // survives is the most recently visited one. Keeping the last instead
        // would silently demote a folder you opened this morning to wherever its
        // oldest duplicate happened to sit.
        //
        // Case-insensitive, because Windows paths are: "D:\Pics" and "d:\pics"
        // are one folder and must collapse to one row.
        FolderPathSet seen;
        seen.reserve(history.size());
        std::vector<std::wstring> keep;
        keep.reserve(history.size());
        for (const std::wstring &path : history) {
            if (!seen.insert(path).second) continue;
            keep.push_back(path);
        }

        const int removed = static_cast<int>(history.size() - keep.size());
        // The RAM list is deduped on load and by PushFolderHistory, so this
        // usually removes nothing — and the rewrite is STILL the point. The file
        // is append-only, so duplicates accumulate there and are only collapsed
        // when RAM is written back over it.
        history.swap(keep);
        historyFoldersManager.RewriteHistoryToDisk();
        if (removed > 0) {
            g_hoverRow = -1;
            InvalidateWalkSnapshot();
            InvalidateTotals();
        }
        return removed;
    }

    void ClearFavoritesKeepHistory() {
        // Backup first — before any RAM or file change
        historyFoldersManager.BackupFavoritesToDisk();

        historyFoldersManager.favorites.clear();

        // Rewrite favorites file only — history file is untouched
        historyFoldersManager.RewriteFavoritesToDisk();
        g_hoverRow = -1;
        InvalidateWalkSnapshot(); // every row changed category
        InvalidateTotals();       // the footer counts favorites separately
    }

    // BOTH LISTS AND BOTH FILES, in one action.
    //
    // Not "call the other two in a row": ClearHistoryKeepFavorites deliberately
    // KEEPS favorites in folderHistory, so running it before clearing favorites
    // would leave the starred paths sitting in qivHistory.txt with nothing
    // marking them, and running it after would rewrite the file twice. Emptying
    // both containers and writing both files once each is the honest shape.
    void ClearHistoryAndFavorites() {
        // Both backups first, before anything is emptied — this is the only one
        // of the three that can lose everything at once, so it is the one whose
        // undo matters most. Restore History && Favorites reads both.
        historyFoldersManager.BackupHistoryToDisk();
        historyFoldersManager.BackupFavoritesToDisk();

        historyFoldersManager.folderHistory.clear();
        historyFoldersManager.favorites.clear();

        historyFoldersManager.RewriteHistoryToDisk();
        historyFoldersManager.RewriteFavoritesToDisk();
        g_hoverRow = -1;
        InvalidateWalkSnapshot();
        InvalidateTotals();
    }

    const std::vector<std::wstring> &GetFolderHistory() {
        return historyFoldersManager.folderHistory;
    }

    // ---------------------------------------------------------------------------
    //  SnapshotHistoryForRemote  —  the folder list as a remote client sees it
    //
    //  WHY THIS EXISTS RATHER THAN THE REMOTE READING THE FILE. QueryHistory used
    //  to build a fresh HistoryFoldersManager and LoadHistoryFromDisk() on every
    //  request: two file opens on the UI thread, and — the real defect — an order
    //  that cannot include this session's promotions. qivHistory.txt is
    //  append-only by design; PushFolderHistory moves a REVISITED folder to the
    //  front of the RAM vector and deliberately does not write (see its "already
    //  exists: promote to front (MRU), no file write needed" branch). So the
    //  panel showed one order and the phone showed another, and opening a folder
    //  from the phone promoted a list the phone was never shown.
    //
    //  THE RAM LIST IS THE SOURCE, NOT g_displayList. The display list is a VIEW:
    //  it carries the typed filter, the row caps and the full-mode override, and
    //  it is empty on an instance where the panel was never opened, because
    //  BuildDisplayList only runs from Show()/F5/the full-mode toggle. Serving it
    //  would mean a filter typed on the desktop silently shortening a list on a
    //  phone that can neither see nor clear it, and a panel that was never opened
    //  answering "no folders". Both are worse than the bug being fixed.
    //
    //  So: no caps, no filter, no full-mode — the whole list, every time.
    //
    //  FAVOURITES FIRST, and that ordering has to happen here: the client renders
    //  the rows in the order they arrive and does no grouping of its own. The
    //  desktop's own favourites-position setting (top/bottom/in-place) is NOT
    //  consulted, because it describes where they sit in a panel the phone user
    //  is not looking at.
    //
    //  Broken rows are dropped, as they were before. The panel paints them red as
    //  a warning worth seeing; a phone can only try to open them and fail.
    // ---------------------------------------------------------------------------
    std::vector<std::pair<std::wstring, bool>> SnapshotHistoryForRemote() {
        const std::vector<std::wstring> &history = historyFoldersManager.folderHistory;
        const FolderPathSet             &favs    = historyFoldersManager.favorites;

        // A favourite that is NOT in the history list at all, collected first so
        // the emit below stays one straight pass.
        //
        // It cannot be produced by starring — ToggleFavorite acts on a row that is
        // in the list by definition, and ClearHistoryKeepFavorites deliberately
        // keeps favourites in it. A hand-edited qivFavorites.txt can, and that
        // file is documented as hand-editable. Before this, such an entry was
        // invisible to a remote for the same reason it is invisible to the walks
        // below: they walk the HISTORY. Reported as a favourite with no position,
        // which is exactly what it is.
        //
        // SORTED, because FolderPathSet is an unordered_set: iterating it raw
        // would let the same data come back in a different order from one run to
        // the next, and a list that reshuffles for no reason reads as a bug.
        std::vector<std::wstring> orphans;
        for (const std::wstring &fav : favs) {
            if (HistoryPath::IsBroken(fav)) continue;

            bool inHistory = false;
            for (const std::wstring &folder : history) {
                if (HistoryPath::Equal(folder, fav)) { inHistory = true; break; }
            }
            if (inHistory) continue;

            orphans.push_back(fav);
        }
        std::sort(orphans.begin(), orphans.end());

        std::vector<std::pair<std::wstring, bool>> out;
        out.reserve(history.size() + orphans.size());

        // One: the starred folders, keeping their MRU order among themselves.
        for (const std::wstring &folder : history) {
            if (HistoryPath::IsBroken(folder)) continue;
            if (favs.count(folder) == 0)       continue;
            out.push_back({folder, true});
        }

        // Two: the starred folders that have no position, behind the ones that do.
        for (const std::wstring &orphan : orphans) {
            out.push_back({orphan, true});
        }

        // Three: everything else, again in MRU order.
        for (const std::wstring &folder : history) {
            if (HistoryPath::IsBroken(folder)) continue;
            if (favs.count(folder) != 0)       continue;
            out.push_back({folder, false});
        }

        return out;
    }

    // ===========================================================================
    //  FOLDER WALKING  —  one implementation, three callers
    //
    //  The horizontal mouse wheel, PageUp/PageDown and Insert/Delete all step
    //  through the SAME list the History panel renders. They differ only in
    //  which rows they may land on (WalkScope) and which way they travel.
    //
    //  Why a frozen snapshot:
    //    OpenDirectory() -> PushFolderHistory() promotes the opened folder to
    //    index 0 of the MRU store. Re-reading the live list on every step would
    //    therefore find the folder you just landed on sitting at the top, and
    //    the next step would go straight back where you came from — the walk
    //    ping-pongs between two folders forever. Freezing the list for the
    //    duration of a walk is what makes stepping mean anything.
    //
    //  The green "you are here" row needs no work — WM_PAINT derives it from
    //  app.playlist[app.currentIndex], so it follows once OpenDirectory lands.
    // ===========================================================================

    // Frozen copy of the display list for the walk in progress.
    struct WalkRow {
        std::wstring path;
        bool isFavorite = false;
    };
    static std::vector<WalkRow> g_walkSnap;
    // The folder THIS walk last TARGETED — set even when opening it then failed.
    // If the app is somewhere else on the next press, the user navigated by some
    // other means and the walk has to resync to wherever they went.
    static std::wstring g_walkAnchor;
    // Where the walk currently sits in g_walkSnap, -1 = not established.
    //
    // This is the authoritative position, NOT the app's current folder. Opening
    // a folder can fail — it was deleted after its status was cached, or it lost
    // its last image — and then the viewer never moves, so deriving the position
    // from the viewer would rewind the walk to the start of the list on the next
    // press. The cursor advances on every row the walk visits, successful or not,
    // so a dead folder costs one step instead of resetting the whole walk.
    static int g_walkCursor = -1;
    // Panel full/short mode the snapshot was taken under.
    static bool g_walkSnapFullMode = false;
    // Bumped whenever the set of rows changes (favorite toggled, list cleared).
    // A plain MRU promotion deliberately does NOT bump this.
    static int g_historyListVersion = 0;
    static int g_walkSnapVersion = -1;

    void InvalidateWalkSnapshot() {
        ++g_historyListVersion;
    }

    // Folder of the image currently on screen — derived exactly as WM_PAINT does.
    static std::wstring CurrentOpenFolder() {
        if (app.playlist.empty() || app.currentIndex < 0 ||
            app.currentIndex >= static_cast<int>(app.playlist.size()))
            return {};
        const std::wstring &cur = app.playlist[app.currentIndex];
        const size_t sep = cur.find_last_of(L"\\/");
        return (sep == std::wstring::npos) ? std::wstring{} : cur.substr(0, sep);
    }

    static std::wstring AppCurrentFolder() {
        // The recorded navigation wins; the playlist is only a fallback for the
        // window before anything has been opened this session.
        return !g_lastNavigatedFolder.empty() ? g_lastNavigatedFolder
                                              : CurrentOpenFolder();
    }

    bool WalkHistoryFolder(HWND hOwner, WalkScope scope, bool reverse) {
        const std::wstring currentFolder = AppCurrentFolder();

        // Did something OTHER than this walk move the app? Read from the latch,
        // never by comparing paths — see g_walkOwnsNavigation for why comparing
        // is unreliable. Consumed here so a single external move triggers exactly
        // one resync.
        const bool movedExternally = g_externalNavigation;
        g_externalNavigation = false;

        // ---- Refresh the snapshot only when it can no longer be trusted -------
        const bool stale =
                g_walkSnap.empty() ||                        // nothing captured yet
                g_walkSnapVersion != g_historyListVersion || // rows added/removed/recategorised
                g_walkSnapFullMode != EffectiveFullMode() || // full <-> short
                movedExternally;

        if (stale) {
            // BuildDisplayList reads EffectiveFullMode() and the display caps, so
            // the snapshot holds exactly the rows the panel would draw — including
            // while a Ctrl+Tab one-shot full view is on screen.
            BuildDisplayList();
            g_walkSnap.clear();
            g_walkSnap.reserve(g_displayList.size());
            for (const auto &e: g_displayList)
                g_walkSnap.push_back({e.path, e.isFavorite});
            g_walkSnapFullMode = EffectiveFullMode();
            g_walkSnapVersion = g_historyListVersion;

            // Row indices mean nothing across a rebuild — re-derive the cursor by
            // path. Prefer where the user actually is; fall back to the last row
            // this walk aimed at, so a rebuild triggered by something other than
            // the user moving (a favorite toggled, say) keeps our place.
            if (movedExternally)
                g_walkAnchor = currentFolder; // adopt wherever the user went

            const int previousCursor = g_walkCursor;
            g_walkCursor = -1;
            if (!g_walkAnchor.empty()) {
                for (int i = 0; i < static_cast<int>(g_walkSnap.size()); ++i) {
                    if (HistoryPath::Equal(g_walkSnap[i].path, g_walkAnchor)) {
                        g_walkCursor = i;
                        break;
                    }
                }
            }
            // Anchor is not in the new list — renamed, deleted, capped out of
            // short mode, hidden by the filter, or simply spelled differently.
            // Hold the position we had instead of falling back to -1, which
            // would send the next step to row 0 and restart the whole walk.
            if (g_walkCursor < 0 && previousCursor >= 0 && !g_walkSnap.empty())
                g_walkCursor = std::min(previousCursor,
                                        static_cast<int>(g_walkSnap.size()) - 1);
        }

        const int total = static_cast<int>(g_walkSnap.size());
        const bool favoritesOnly = (scope == WalkScope::FavoritesOnly);
        const wchar_t *emptyMsg = favoritesOnly
                                          ? Constants::Messages::WALK_NO_FAVORITE_FOLDERS
                                          : Constants::Messages::WALK_NO_HISTORY_FOLDERS;
        if (total == 0) {
            g_walkCursor = -1;
            g_overlayManager.PostCenterMessage(hOwner, emptyMsg,
                                               OverlayManager::MsgSeverity::Error);
            return false;
        }
        if (g_walkCursor >= total) g_walkCursor = -1; // list shrank under us

        // ---- Where to step from ----------------------------------------------
        const int direction = reverse ? -1 : +1;
        // No cursor yet (first ever walk, or the anchor is not in the list):
        // start just off the end we travel from, so the first step lands on the
        // first eligible row.
        const int startRow = (g_walkCursor >= 0) ? g_walkCursor : (reverse ? total : -1);

        // Basename for the centre overlay — full paths are too long to read.
        auto folderName = [](const std::wstring &full) {
            const size_t sep = full.find_last_of(L"\\/");
            return (sep != std::wstring::npos && sep + 1 < full.size())
                           ? full.substr(sep + 1)
                           : full;
        };

        // ---- One lap, wrapping, first USABLE row wins -------------------------
        // Only MISSING folders are stepped over. An EMPTY folder is opened, the
        // same as pressing Enter on it in this panel — see the status branch below.
        //
        // Skips are NOT posted as they happen: MID_CENTER holds one message, so
        // the landing message would overwrite them a moment later and the user
        // would never see them. They are collected and folded into the single
        // message posted at the end.
        int skipped = 0;
        std::wstring lastSkipText;      // "<reason> <n>/<total> <name>" of the last skip
        bool anyRowRepainted = false;

        for (int step = 1; step <= total; ++step) {
            int row = startRow + direction * step;
            row = ((row % total) + total) % total; // positive modulo — wraps both ways

            const WalkRow &entry = g_walkSnap[row];
            // Wrong category for this scope — not a row this walk can occupy, so
            // the cursor must NOT move onto it.
            if (scope == WalkScope::FavoritesOnly && !entry.isFavorite) continue;
            if (scope == WalkScope::NonFavoritesOnly && entry.isFavorite) continue;
            // Never re-open what is already on screen. Compared against the
            // VIEWER's folder rather than g_walkAnchor — the anchor can hold a
            // different spelling of the same path (see fs::canonical above).
            if (!currentFolder.empty() && HistoryPath::Equal(entry.path, currentFolder))
                continue;

            // Eligible row: claim it as the new position before deciding whether
            // it can actually be opened. A dead folder therefore consumes one step
            // rather than leaving the cursor behind for the next keypress to redo.
            g_walkCursor = row;

            // Resolves Unknown by hitting the filesystem and caches the result,
            // so the panel repaints this row in its dead / missing colour too.
            const FolderStatus status = GetFolderStatus(entry.path);

            // MISSING is the only status that is stepped over. The folder is not
            // there, so there is nothing to navigate to.
            if (status == FolderStatus::Missing) {
                ++skipped;
                lastSkipText = std::wstring(Constants::Messages::FOLDER_DEAD_MISSING)
                               + L" " + std::to_wstring(row + 1) + L"/" +
                               std::to_wstring(total) + L" " + folderName(entry.path);
                anyRowRepainted = true;
                // g_walkAnchor is deliberately NOT touched here: it means "the
                // folder this walk put the viewer in", and a skipped folder was
                // never opened. Setting it would guarantee a mismatch against the
                // viewer on the next press, which reads as external navigation —
                // which is how a walk ended up landing back on the folder it
                // started from, promoting it to the top of the MRU list.
                // g_walkCursor above already records that this row was consumed.
                continue; // step over it and keep looking
            }

            // EMPTY is NOT skipped — the walk opens it, exactly as pressing Enter
            // on that row in this panel does. The folder genuinely exists and you
            // navigated to it, so it becomes the current folder, turns green, and
            // shows the empty-folder placeholder; F5 recovers it once images
            // appear. Skipping instead meant the walk moved somewhere the panel
            // did not agree with, and every piece of state downstream — the green
            // row, the MRU order, the walk anchor — drifted apart from there.
            const std::wstring folder = entry.path; // copy — OpenDirectory rebuilds g_displayList
            const std::wstring name = folderName(folder);

            // DIRECTION FIRST, THE SAME ARROW THE FOLDER-TREE WALK USES.
            //
            // Alt+Left/Right/Up/Down announce with ⬅️ ➡️ ⬆️ ⬇️ and read at a
            // glance: the icon says which way you went. This walk said only WHAT
            // it landed on — 📁 for a history folder, ★ for a favourite — and
            // used the identical glyph whichever direction the wheel was
            // turned, so the one thing a walk most needs to report was the one
            // thing missing. Two ways of moving between folders, two vocabularies.
            //
            // The kind marker is KEPT, not replaced. It carries something the
            // arrow does not — whether this row is starred — and dropping it to
            // unify the icons would trade one missing fact for another. So the
            // line now reads direction, then kind, then where you are:
            //
            //     ⬅️ ★ 3/12 Holidays
            //
            // Prefix comes from the ROW, not from the scope, so every caller —
            // wheel included — produces the identical message for a given folder.
            // For the two key pairs this is fixed anyway (their scope already
            // pins the category); for the wheel it correctly marks starred rows.
            const wchar_t *kind = entry.isFavorite
                                            ? Constants::Messages::WALK_FAVORITE_FOLDER
                                            : Constants::Messages::WALK_HISTORY_FOLDER;

            // `reverse` is the walk's own direction argument, so this cannot
            // disagree with the step that was actually taken.
            //
            // NOT `arrow` — that name is taken further down by the "→" that
            // separates a skip report from the destination, and the two are
            // different things sharing one scope.
            const wchar_t *directionArrow = reverse ? Constants::Messages::WALK_ARROW_PREV
                                                    : Constants::Messages::WALK_ARROW_NEXT;

            // row + 1 is literally the number the panel paints next to that row.
            std::wstring text = std::wstring(directionArrow) + L"  " + kind + L" " +
                                std::to_wstring(row + 1) + L"/" +
                                std::to_wstring(total) + L" " + name;

            // Landing on an empty folder is legitimate but worth saying out loud,
            // since the viewer will show a placeholder rather than an image.
            const bool landedEmpty = (status == FolderStatus::Empty);
            if (landedEmpty)
                text += std::wstring(L"  ") + Constants::Messages::FOLDER_DEAD_EMPTY;

            // Something was stepped over on the way here — say so, and colour the
            // whole message as a warning so it is visibly not an ordinary hop.
            // MID_CENTER is single-line, hence a prefix rather than a second line.
            const std::wstring arrow = std::wstring(L"  ") +
                                       Constants::Icon::ARROW_RIGHT + L"  ";
            if (skipped == 1)
                text = lastSkipText + arrow + text;
            else if (skipped > 1)
                text = std::wstring(Constants::Icon::WARNING) +
                       Constants::Messages::WALK_SKIPPED +
                       std::to_wstring(skipped) + arrow + text;

            g_overlayManager.PostCenterMessage(hOwner, text,
                (skipped > 0 || landedEmpty) ? OverlayManager::MsgSeverity::Warning
                                             : OverlayManager::MsgSeverity::Normal);

            if (anyRowRepainted) {
                auto &histWnd = uiManager.getHistoryListWindow();
                if (histWnd.IsVisible())
                    InvalidateRect(histWnd.GetHwnd(), nullptr, FALSE);
            }

            // Remember where we put the user. g_walkCursor was already set to this
            // row above. The latch tells PushFolderHistory — which OpenDirectory
            // calls synchronously, on this thread — that the navigation about to
            // happen is ours, so it must not be treated as the user moving away.
            g_walkAnchor = folder;
            g_walkOwnsNavigation = true;
            OpenDirectory(hOwner, folder);
            g_walkOwnsNavigation = false; // cleared even if OpenDirectory bailed early

            // Re-assert OUR spelling of the folder, after OpenDirectory has had
            // its say. OpenDirectory resolves junctions with fs::canonical(), so
            // opening D:\Wallpapers\[Set 8] — a junction — records
            // E:\Wallpapers\[Set 8] instead. Both rows exist in the list and both
            // are legitimate, but the one the user walked to is THIS one: it is
            // the row that must go green, and the row the cursor sits on. Without
            // this the marker jumped to the resolved row while the walk carried on
            // from the row you actually stepped to.
            NotifyCurrentFolder(folder);
            return true;
        }

        // Nothing usable anywhere in this scope. If the lap hit dead folders,
        // report the last one in red rather than the vaguer "nothing here".
        if (skipped > 0) {
            g_overlayManager.PostCenterMessage(hOwner, lastSkipText,
                                               OverlayManager::MsgSeverity::Error);
            auto &histWnd = uiManager.getHistoryListWindow();
            if (histWnd.IsVisible())
                InvalidateRect(histWnd.GetHwnd(), nullptr, FALSE);
        } else {
            g_overlayManager.PostCenterMessage(hOwner, emptyMsg,
                                               OverlayManager::MsgSeverity::Error);
        }
        return false;
    }

    // ---------------------------------------------------------------------------
    // GetHistoryWindowBounds  —  centered on the parent's monitor
    // ---------------------------------------------------------------------------
    static void GetHistoryWindowBounds(HWND hRef, int &x, int &y, int &w, int &h) {
        HMONITOR hMonitor = MonitorFromWindow(hRef, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {sizeof(mi)};
        GetMonitorInfoW(hMonitor, &mi);

        int monX = mi.rcMonitor.left;
        int monY = mi.rcMonitor.top;
        int monW = mi.rcMonitor.right - mi.rcMonitor.left;
        int monH = mi.rcMonitor.bottom - mi.rcMonitor.top;

        UINT dpi = static_cast<UINT>(app.dpiScale * 96.0f);

        int entries = std::max(1, static_cast<int>(g_displayList.size()));
        int totalH = CalcTotalContentH(entries, dpi);

        int minW = MulDiv(Constants::History::HISTORY_MIN_W, dpi, 96);
        int maxW = MulDiv(Constants::History::HISTORY_MAX_W, dpi, 96);
        int minH = MulDiv(Constants::History::HISTORY_MIN_H, dpi, 96);
        int maxH = MulDiv(Constants::History::HISTORY_MAX_H, dpi, 96);

        w = std::clamp(static_cast<int>(monW * 0.30f), minW, maxW);
        h = std::clamp(std::min(totalH, static_cast<int>(monH * 0.80f)), minH, std::min(maxH, static_cast<int>(monH * 0.80f)));
        x = monX + (monW - w) / 2;
        y = monY + (monH - h) / 2;
    }

    void RefreshHistoryFullMode() {
        auto &histWnd = uiManager.getHistoryListWindow();
        HWND h = histWnd.GetHwnd();
        if (!h || !IsWindowVisible(h)) return; // closed panel picks it up on Show()
        ApplyFullHistoryMode(h);
    }

    // Ctrl+Tab from the MAIN APP — "show me the whole history".
    //
    // This is a request to VIEW the full list, not to change the default mode,
    // so it sets the one-shot override and deliberately leaves
    // app.historyFullModeEnabled alone. Plain Tab afterwards reopens in whatever
    // mode the user last actually chose.
    void ToggleHistoryFull() {
        auto &histWnd = uiManager.getHistoryListWindow();
        if (!histWnd.GetHwnd()) return;
        if (IsWindowVisible(histWnd.GetHwnd())) {
            if (EffectiveFullMode()) {
                // Already showing everything — second press closes it.
                g_fullModeOverride = false;
                histWnd.Hide();
            } else {
                g_fullModeOverride = true; // view-only: no preference write
                g_view.scrollY = 0;
                BuildDisplayList();
                HoverCurrentFolderRow();
                InvalidateRect(histWnd.GetHwnd(), nullptr, TRUE);
            }
        } else {
            g_fullModeOverride = true; // view-only: no preference write
            BuildDisplayList();
                int x, y, w, h;
            GetHistoryWindowBounds(g_hHistOwner ? g_hHistOwner : histWnd.GetHwnd(), x, y, w, h);
            SetWindowPos(histWnd.GetHwnd(), HWND_TOPMOST, x, y, w, h, SWP_FRAMECHANGED);
            g_view.scrollY = 0;
            HoverCurrentFolderRow();
            ShowWindow(histWnd.GetHwnd(), SW_SHOW);
            SetForegroundWindow(histWnd.GetHwnd());
            InvalidateRect(histWnd.GetHwnd(), nullptr, TRUE);
        }
    }

    // ---------------------------------------------------------------------------
    // Keyboard handling
    // ---------------------------------------------------------------------------
    // Esc: if the filter box has text, clear it (same as the ✕ button) and keep
    // the panel open. Empty filter → return false so the base hides the panel.
    // Clear() fires g_filter.OnChanged, which rebuilds the list + repaints.
    bool HistoryListWnd::OnLocalHide() {
        return g_filter.RouteKey(VK_ESCAPE, m_hWnd) == InputResult::RequestClear;
    }

    bool HistoryListWnd::OnKeyDown(WPARAM vk, bool ctrl, bool shift, bool alt) {
        if (vk == VK_TAB && ctrl) {
            ToggleFullHistory(m_hWnd);
            return true;
        }

        if (vk == 'Z' && ctrl && !shift && !alt && g_lastDeletedIndex >= 0) {
            auto &hist = historyFoldersManager.folderHistory;
            int insertAt = std::min(g_lastDeletedIndex, static_cast<int>(hist.size()));
            hist.insert(hist.begin() + insertAt, g_lastDeletedPath);
            historyFoldersManager.RewriteHistoryToDisk();
            if (g_lastDeletedWasFavorite) {
                auto &favSet = historyFoldersManager.favorites;
                if (UniqueFavoriteCount() < app.historyMaxFavs) {
                    favSet.insert(g_lastDeletedPath);
                    historyFoldersManager.RewriteFavoritesToDisk();
                } else {
                    g_overlayManager.PostCenterMessage(
                        g_hHistOwner,
                        L"Favorites full (" + std::to_wstring(app.historyMaxFavs) + L" max) — not restored");
                }
            }
            g_lastDeletedIndex = -1;
            InvalidateWalkSnapshot(); // a row came back
            BuildDisplayList();
            int newMax = static_cast<int>(g_displayList.size());
            if (g_hoverRow >= newMax) g_hoverRow = newMax - 1;
            int x, y, w, h;
            GetHistoryWindowBounds(g_hHistOwner ? g_hHistOwner : m_hWnd, x, y, w, h);
            SetWindowPos(m_hWnd, HWND_TOPMOST, x, y, w, h, SWP_FRAMECHANGED);
            InvalidateRect(m_hWnd, nullptr, TRUE);
            return true;
        }

        if (vk == VK_F5 && !ctrl && !shift && !alt) {
            RefreshHistory(m_hWnd);
            RefreshCachedFileSize(); // the .txt may have changed size since Show()
            return true;
        }

        int navMax = static_cast<int>(g_displayList.size());
        switch (vk) {
            case Shortcuts::SC_PANEL_HISTORY_TOGGLE:
                ToggleHistoryWindow();
                return true;

            case VK_UP:
                if (navMax > 0) {
                    g_hoverRow = (g_hoverRow <= 0) ? navMax - 1 : g_hoverRow - 1;
                    if (g_rowH > 0) {
                        int bodyH = g_bodyBottom - g_bodyTop;
                        if (g_hoverRow == navMax - 1) {
                            g_view.scrollY = std::max(0, navMax * g_rowH - bodyH);
                        } else {
                            int rowStart = g_hoverRow * g_rowH;
                            if (rowStart < g_view.scrollY)
                                g_view.scrollY = rowStart;
                        }
                    }
                    InvalidateRect(m_hWnd, nullptr, FALSE);
                }
                return true;

            case VK_DOWN:
                if (navMax > 0) {
                    g_hoverRow = (g_hoverRow < navMax - 1) ? g_hoverRow + 1 : 0;
                    if (g_rowH > 0) {
                        if (g_hoverRow == 0) {
                            g_view.scrollY = 0;
                        } else {
                            int bodyH = g_bodyBottom - g_bodyTop;
                            int rowEnd = (g_hoverRow + 1) * g_rowH;
                            if (rowEnd - g_view.scrollY > bodyH)
                                g_view.scrollY = rowEnd - bodyH;
                        }
                    }
                    InvalidateRect(m_hWnd, nullptr, FALSE);
                }
                return true;

            case VK_RETURN: {
                if (g_hoverRow >= 0 && g_hoverRow < navMax) {
                    std::wstring folder = g_displayList[g_hoverRow].path;
                    {
                        FolderStatus fs = GetFolderStatus(folder);
                        if (fs == FolderStatus::Missing) {
                            if (g_hHistOwner)
                                g_overlayManager.PostCenterMessage(g_hHistOwner,
                                    Constants::Messages::FOLDER_DEAD_MISSING);
                            return true;
                        }
                        // Empty folders fall through — both OpenDirectory and
                        // SpawnDirWndForFolder handle them so the user can paste into them.
                    }
                    bool shiftHeld = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                    if (shiftHeld) {
                        // Creating or hiding a spawned panel briefly steals focus,
                        // firing OnKillFocus which resets g_hoverRow. Save and
                        // restore so the selection stays on the row after the call.
                        const int savedRow = g_hoverRow;
                        SpawnedDirWnd *existing = uiManager.FindSpawnedDirWnd(folder);
                        if (existing) {
                            existing->Hide();
                        } else {
                            uiManager.SpawnDirWndForFolder(folder, m_hWnd);
                        }
                        g_hoverRow = savedRow;
                        InvalidateRect(m_hWnd, nullptr, FALSE);
                    } else {
                        ShowWindow(m_hWnd, SW_HIDE);
                        OpenDirectory(g_hHistOwner, folder);
                        // Re-assert the row's own spelling: OpenDirectory resolves
                        // junctions, so it records the TARGET path and the green
                        // marker would land on the target's row instead of the one
                        // just opened.
                        NotifyCurrentFolder(folder);
                    }
                }
                return true;
            }

            case Shortcuts::HISTORY_FAVORITES_TOGGLE_KEY:
                if (g_hoverRow >= 0 && g_hoverRow < navMax) {
                    ToggleFavorite(g_hoverRow);
                    BuildDisplayList();
                    int newMax = static_cast<int>(g_displayList.size());
                    if (g_hoverRow >= newMax)
                        g_hoverRow = newMax - 1;
                    int x, y, w, h;
                    GetHistoryWindowBounds(g_hHistOwner ? g_hHistOwner : m_hWnd, x, y, w, h);
                    SetWindowPos(m_hWnd, HWND_TOPMOST, x, y, w, h, SWP_FRAMECHANGED);
                    InvalidateRect(m_hWnd, nullptr, TRUE);
                }
                return true;

            // ── The navigation cluster belongs to the MAIN APP ───────────────
            // Home / End / PageUp / PageDown / Insert / Delete drive image and
            // folder navigation, and must do the same thing whether or not this
            // panel has focus — returning false forwards them to the app
            // pipeline. Walking the list while looking at it is the whole point.
            //
            // Exception: while the filter box holds text, Home / End are caret
            // keys and plain Delete is forward-delete. Text editing wins until
            // the filter is cleared (Esc), then the keys navigate again.
            case VK_HOME:
            case VK_END:
                if (!ctrl && !shift && !alt && !g_filter.IsEmpty()) {
                    if (g_filter.RouteKey(vk, m_hWnd) == InputResult::ConsumedRepaint)
                        InvalidateRect(m_hWnd, nullptr, FALSE);
                    return true;
                }
                return false;

            case VK_PRIOR:
            case VK_NEXT:
            case VK_INSERT:
                return false;

            case VK_DELETE:
                if (ctrl && shift && alt) {
                    ClearFavoritesKeepHistory();
                    BuildDisplayList();
                    {
                        int x, y, w, h;
                        GetHistoryWindowBounds(g_hHistOwner ? g_hHistOwner : m_hWnd, x, y, w, h);
                        SetWindowPos(m_hWnd, HWND_TOPMOST, x, y, w, h, SWP_FRAMECHANGED);
                    }
                    InvalidateRect(m_hWnd, nullptr, TRUE);
                } else if (ctrl && shift) {
                    ClearHistoryKeepFavorites();
                    BuildDisplayList();
                    {
                        int x, y, w, h;
                        GetHistoryWindowBounds(g_hHistOwner ? g_hHistOwner : m_hWnd, x, y, w, h);
                        SetWindowPos(m_hWnd, HWND_TOPMOST, x, y, w, h, SWP_FRAMECHANGED);
                    }
                    InvalidateRect(m_hWnd, nullptr, TRUE);
                } else if (!ctrl && !shift && !alt) {
                    // Filter has text → forward-delete in the input box.
                    if (!g_filter.IsEmpty()) {
                        if (g_filter.RouteKey(VK_DELETE, m_hWnd) == InputResult::ConsumedRepaint)
                            InvalidateRect(m_hWnd, nullptr, FALSE);
                        return true;
                    }
                    // Otherwise plain Delete is a navigation key — hand it to the
                    // app (previous favorite folder). Deleting the hovered row
                    // moved to Ctrl+Delete, handled below.
                    return false;
                } else if (ctrl && !shift && !alt) {
                    if (g_hoverRow >= 0 && g_hoverRow < navMax) {
                        const std::wstring &path = g_displayList[g_hoverRow].path;
                        bool wasFav = g_displayList[g_hoverRow].isFavorite;

                        auto &hist = historyFoldersManager.folderHistory;
                        int histIdx = -1;
                        for (int i = 0; i < static_cast<int>(hist.size()); ++i) {
                            if (hist[i] == path) {
                                histIdx = i;
                                break;
                            }
                        }

                        g_lastDeletedPath = path;
                        g_lastDeletedIndex = histIdx;
                        g_lastDeletedWasFavorite = wasFav;

                        if (histIdx >= 0)
                            hist.erase(hist.begin() + histIdx);
                        historyFoldersManager.RewriteHistoryToDisk();

                        if (wasFav) {
                            historyFoldersManager.favorites.erase(path);
                            historyFoldersManager.RewriteFavoritesToDisk();
                        }

                        InvalidateWalkSnapshot(); // a row disappeared
                        BuildDisplayList();
                        int newMax = static_cast<int>(g_displayList.size());
                        if (g_hoverRow >= newMax) g_hoverRow = newMax - 1;
                        int x, y, w, h;
                        GetHistoryWindowBounds(g_hHistOwner ? g_hHistOwner : m_hWnd, x, y, w, h);
                        SetWindowPos(m_hWnd, HWND_TOPMOST, x, y, w, h, SWP_FRAMECHANGED);
                        InvalidateRect(m_hWnd, nullptr, TRUE);
                    }
                }
                return true;

            default:
                // The filter box decides: editing keys, Ctrl+A/C/X/V, forward
                // policy (F-keys/modifiers → Ignored), and printable-swallow are
                // all folded into RouteKey.
                switch (g_filter.RouteKey(vk, m_hWnd)) {
                    case InputResult::Ignored:         return false; // forward to app pipeline
                    case InputResult::RequestClose:    return false; // (Esc arrives via OnLocalHide)
                    case InputResult::RequestClear:    InvalidateRect(m_hWnd, nullptr, FALSE); return true;
                    case InputResult::ConsumedRepaint: InvalidateRect(m_hWnd, nullptr, FALSE); return true;
                    case InputResult::Consumed:        return true;
                }
                return true;
        }
    }

    // ---------------------------------------------------------------------------
    // Back-buffer helpers
    // ---------------------------------------------------------------------------
    void HistoryListWnd::EnsureBackBuffer(HDC refDC, int w, int h) {
        if (m_bbDC && w == m_bbW && h == m_bbH) return;
        DestroyBackBuffer();
        m_bbDC = CreateCompatibleDC(refDC);
        m_bbBmp = CreateCompatibleBitmap(refDC, w, h);
        m_bbBmpOld = static_cast<HBITMAP>(SelectObject(m_bbDC, m_bbBmp));
        m_bbW = w;
        m_bbH = h;
    }

    void HistoryListWnd::DestroyBackBuffer() {
        if (m_bbDC) {
            if (m_bbBmpOld) SelectObject(m_bbDC, m_bbBmpOld);
            DeleteDC(m_bbDC);
            m_bbDC = nullptr;
        }
        if (m_bbBmp) {
            DeleteObject(m_bbBmp);
            m_bbBmp = nullptr;
        }
        m_bbBmpOld = nullptr;
        m_bbW = m_bbH = 0;
    }

    // ---------------------------------------------------------------------------
    // OnMButtonUp — side-effects before the base class hides the panel
    // ---------------------------------------------------------------------------
    bool HistoryListWnd::OnMButtonUp(int mx, int my) {
        for (int i = 0; i < static_cast<int>(g_rowRects.size()); ++i) {
            const RECT &r = g_rowRects[i];
            if (mx >= r.left && mx < r.right && my >= r.top && my < r.bottom) {
                if (i >= static_cast<int>(g_displayList.size())) break;
                const std::wstring &folder = g_displayList[i].path;
                if (GetFolderStatus(folder) == FolderStatus::Missing) {
                    if (g_hHistOwner)
                        g_overlayManager.PostCenterMessage(g_hHistOwner,
                            Constants::Messages::FOLDER_DEAD_MISSING);
                    return true;
                }
                SpawnedDirWnd *existing = uiManager.FindSpawnedDirWnd(folder);
                if (existing)
                    existing->Hide();
                else
                    uiManager.SpawnDirWndForFolder(folder, m_hWnd);
                return true;
            }
        }
        return true;
    }

    // ---------------------------------------------------------------------------
    // Window procedure
    // ---------------------------------------------------------------------------
    LRESULT HistoryListWnd::HandlePanelMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_ERASEBKGND) return 1;
        switch (message) {
            case Constants::WM_QIV_HISTORY_VALIDATED: {
                // Background folder scan finished — apply results on the UI thread
                // (the only thread that touches g_statusCache / g_dirSizeCache).
                auto *results = reinterpret_cast<std::vector<DirScanResult> *>(lParam);
                if (!results) {
                    // Null payload = that sweep finished. Ignore it if a newer
                    // sweep has since started, or the indicator would vanish
                    // while work is still running.
                    if (static_cast<uint64_t>(wParam) ==
                        g_histScanGen.load(std::memory_order_relaxed)) {
                        g_scanRunning = false;
                        InvalidateRect(m_hWnd, nullptr, TRUE);
                    }
                    return 0;
                }
                // Discard results from a refresh that has since been superseded.
                if (static_cast<uint64_t>(wParam) ==
                    g_histScanGen.load(std::memory_order_relaxed)) {
                    for (const auto &r : *results) ApplyDirScan(r);
                    InvalidateRect(m_hWnd, nullptr, TRUE);
                }
                delete results;
                return 0;
            }
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC screenDC = BeginPaint(m_hWnd, &ps);
                RECT rc;
                GetClientRect(m_hWnd, &rc);

                EnsureBackBuffer(screenDC, rc.right, rc.bottom);
                HDC hdc = m_bbDC;

                UINT dpi = static_cast<UINT>(app.dpiScale * 96.0f);
                int padding = MulDiv(Constants::History::HISTORY_PADDING, dpi, 96);
                int rowH = MulDiv(Constants::History::HISTORY_ROW_HEIGHT, dpi, 96);
                int fontSize = MulDiv(Constants::History::HISTORY_FONT_SIZE, dpi, 96);
                int titleSz = MulDiv(Constants::History::HISTORY_FONT_SIZE + 2, dpi, 96);
                int indexW = MulDiv(28, dpi, 96);
                // Single badge column — see BuildRowBadges for why several marks
                // share one slot instead of each getting a column.
                int starW = MulDiv(18, dpi, 96);

                // Scrollbar geometry — computed before any drawing.
                int SB_W = static_cast<int>(
                    UI::ScrollBarThicknessPx(app.dpiScale));
                int totalContentH = CalcTotalContentH(
                        static_cast<int>(g_displayList.size()), dpi);
                int windowH = rc.bottom - rc.top;
                int maxScroll = std::max(0, totalContentH - windowH);
                g_view.scrollY = std::clamp(g_view.scrollY, 0, maxScroll);
                bool needsScrollbar = (maxScroll > 0);

                // The thumb is sized from these two. Content is measured against
                // the WINDOW here — as the clamp above is — while the bar is
                // drawn down the body band only, so the view rect is set later
                // beside the track rather than derived from Layout.
                g_view.contentH = totalContentH;

                // Background — use active color if this panel is active
                FillRect(hdc, &rc, UI::Gdi::Brush(GetBgColor()));
                SetBkMode(hdc, TRANSPARENT);

                int listFontSz = MulDiv(Constants::History::HISTORY_LIST_FONT_SIZE, dpi, 96);
                if (static_cast<int>(dpi) != m_cachedFontDpi) {
                    if (m_hFontTitle) {
                        DeleteObject(m_hFontTitle);
                        m_hFontTitle = nullptr;
                    }
                    if (m_hFontBody) {
                        DeleteObject(m_hFontBody);
                        m_hFontBody = nullptr;
                    }
                    if (m_hFontList) {
                        DeleteObject(m_hFontList);
                        m_hFontList = nullptr;
                    }
                    if (m_hFontIndexLink) {
                        DeleteObject(m_hFontIndexLink);
                        m_hFontIndexLink = nullptr;
                    }
                    if (m_hFontLink) {
                        DeleteObject(m_hFontLink);
                        m_hFontLink = nullptr;
                    }
                    m_hFontTitle = CreateFontW(
                            titleSz, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
                    m_hFontBody = CreateFontW(
                            fontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
                    m_hFontList = CreateFontW(
                            listFontSz, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
                    m_hFontIndexLink = CreateFontW(
                            listFontSz, 0, 0, 0, FW_NORMAL, FALSE, Constants::Links::UNDERLINE, FALSE,
                            DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
                    m_hFontLink = CreateFontW(
                            fontSize, 0, 0, 0, FW_NORMAL, FALSE, Constants::Links::UNDERLINE, FALSE,
                            DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
                    m_cachedFontDpi = static_cast<int>(dpi);
                }

                // Push counts into the real title bar
                int totalSaved = static_cast<int>(historyFoldersManager.folderHistory.size());
                int totalShown = static_cast<int>(g_displayList.size());
                // Unique folders, matching what the cap actually enforces —
                // otherwise a junction and its target read as two favorites.
                int favCount = UniqueFavoriteCount();
                {
                    // THE SAME 📜 THE WALK OVERLAY LEADS WITH, and that is the
                    // point of it being here. A centre message that flashes for
                    // a second teaches nothing on its own; seeing the identical
                    // mark on the title bar of the panel those rows live in is
                    // what makes the overlay legible the next time it appears.
                    // The star and the symlink mark below already work that way
                    // — named in this caption, recognised wherever they appear.
                    std::wstring caption = std::wstring(Constants::Icon::HISTORY)
                                           + L"  Folder History  (showing "
                                           + std::to_wstring(totalShown) + L" of "
                                           + std::to_wstring(totalSaved) + L" saved)   " + Constants::Icon::FAVORITES_MARK + L" = Space (toggle fav)   "
                                           + std::to_wstring(favCount) + L" / "
                                           + std::to_wstring(app.historyMaxFavs)
                                           + L" favorites   "
                                           + Constants::Icon::SYMLINK_MARK
                                           + L" = symlink / junction";
                    SetWindowTextW(m_hWnd, caption.c_str());
                }

                // Single shortcuts line
                SelectObject(hdc, m_hFontBody);
                int hintTop = rc.top + padding;
                int hintBot = hintTop + MulDiv(fontSize + 2, dpi, 96);
                {
                    constexpr wchar_t SHORTCUTS[] =
                            L"F5 = refresh     Del = delete entry     Ctrl+Z = restore"
                            L"     Ctrl+Tab = full list"
                            L"     Ctrl+Shift+Del = clear history"
                            L"     Ctrl+Alt+Shift+Del = clear favorites";
                    SetTextColor(hdc, RGB(150, 150, 150));
                    RECT scRect = {rc.left + padding, hintTop, rc.right - padding, hintBot};
                    DrawTextW(hdc, SHORTCUTS, -1, &scRect,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                }

                g_exeLinkRect = {};
                g_shortcutF5Rect = {};
                g_shortcutCtrlTabRect = {};

                // Shortcuts line — "F5" and "Ctrl+Tab" are drawn as clickable links
                {
                    LONG curX    = rc.left + padding;
                    LONG rightBound = rc.right - padding;

                    auto drawShortcutPlain = [&](const wchar_t *text) {
                        if (!text || !*text || curX >= rightBound) return;
                        SelectObject(hdc, m_hFontBody);
                        SetTextColor(hdc, RGB(150, 150, 150));
                        SIZE sz = {};
                        GetTextExtentPoint32W(hdc, text, static_cast<int>(wcslen(text)), &sz);
                        RECT r = {curX, hintTop, std::min(curX + sz.cx, rightBound), hintBot};
                        DrawTextW(hdc, text, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                        curX += sz.cx;
                    };
                    auto drawShortcutLink = [&](const wchar_t *text, RECT &outRect) {
                        if (!text || !*text || curX >= rightBound) return;
                        SelectObject(hdc, m_hFontLink);
                        SetTextColor(hdc, Constants::Theme::Markers::INFO);
                        SIZE sz = {};
                        GetTextExtentPoint32W(hdc, text, static_cast<int>(wcslen(text)), &sz);
                        outRect = {curX, hintTop, curX + sz.cx, hintBot};
                        DrawTextW(hdc, text, -1, &outRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                        curX += sz.cx;
                    };

                    drawShortcutLink(L"F5", g_shortcutF5Rect);
                    drawShortcutPlain(L" = refresh     Del = delete entry     Ctrl+Z = restore     ");
                    drawShortcutLink(L"Ctrl+Tab", g_shortcutCtrlTabRect);
                    drawShortcutPlain(L" = full list     Ctrl+Shift+Del = clear history     Ctrl+Alt+Shift+Del = clear favorites");
                }

                SelectObject(hdc, m_hFontTitle);

                // Separator (fixed) — placed after shortcuts line
                int sepY = hintBot + MulDiv(4, dpi, 96);
                HPEN hOldPen = (HPEN) SelectObject(hdc, UI::Gdi::Pen(RGB(50, 50, 50)));
                MoveToEx(hdc, rc.left + padding, sepY, nullptr);
                LineTo(hdc, rc.right - padding, sepY);
                SelectObject(hdc, hOldPen);

                int filterRowPx = MulDiv(Constants::History::HISTORY_FILTER_ROW_H, dpi, 96);
                int footerSepY = rc.bottom - filterRowPx - 1 - MulDiv(fontSize + 2 + 8, dpi, 96);

                // Rows (scrolled) — clipped to the body area between separator and footer
                SelectObject(hdc, m_hFontList);
                g_rowRects.clear();
                g_rowRects.reserve(g_displayList.size());
                g_indexRects.clear();
                g_indexRects.reserve(g_displayList.size());
                g_linkRects.clear();
                g_linkRects.reserve(g_displayList.size());
                int rowsTop = sepY + MulDiv(6, dpi, 96);
                int bodyBottom = footerSepY;
                g_bodyTop = rowsTop;
                g_bodyBottom = bodyBottom;
                g_rowH = rowH;

                // A programmatic hover move (Show, mode switch) asked for its row
                // to be brought into view. This is the first moment the maths is
                // possible — rowH and the body extent only exist once the panel
                // has laid itself out — and it happens before the rows are drawn,
                // so the corrected offset applies to this very paint.
                if (g_scrollHoverIntoView) {
                    g_scrollHoverIntoView = false;
                    const int rowCount = static_cast<int>(g_displayList.size());
                    const int bodyH = bodyBottom - rowsTop;
                    if (g_hoverRow >= 0 && rowH > 0 && bodyH > 0 && rowCount > 0) {
                        // Centre the row when the list is long enough to scroll,
                        // then clamp to the ends so we never scroll past the list.
                        const int maxOffset = std::max(0, rowCount * rowH - bodyH);
                        int desired = g_hoverRow * rowH - (bodyH - rowH) / 2;
                        g_view.scrollY = std::clamp(desired, 0, maxOffset);
                    }
                }

                // "Loading ..." while the sweep runs — drawn before the rows so the
                // rows paint over it as their answers land, and centred in the body
                // rather than replacing it: the list is fully usable meanwhile.
                if (g_scanRunning) {
                    HFONT hScan = CreateFontW(
                            -MulDiv(Constants::History::HISTORY_FONT_SIZE +
                                            Constants::Theme::HistoryPanel::SCANNING_FONT_BOOST,
                                    dpi, 72),
                            0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            VARIABLE_PITCH, L"Segoe UI");
                    HFONT hOldScan = static_cast<HFONT>(SelectObject(hdc, hScan));
                    SetTextColor(hdc, Constants::Theme::HistoryPanel::SCANNING_TEXT);
                    RECT scanRect = {rc.left, rowsTop, rc.right, bodyBottom};
                    DrawTextW(hdc, Constants::Messages::HISTORY_SCANNING, -1, &scanRect,
                              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    SelectObject(hdc, hOldScan);
                    DeleteObject(hScan);
                    SelectObject(hdc, m_hFontList); // restore the row font
                }

                SaveDC(hdc);
                IntersectClipRect(hdc, rc.left, rowsTop, rc.right, bodyBottom);

                // The folder currently open in the main app — drives the green
                // "you are here" row. Uses the recorded navigation rather than the
                // playlist so an empty folder highlights ITSELF, not the folder
                // that was open before it.
                const std::wstring currentFolder = AppCurrentFolder();

                if (g_displayList.empty()) {
                    int y = rowsTop - g_view.scrollY;
                    SetTextColor(hdc, RGB(100, 100, 100));
                    RECT emptyRect = {rc.left + padding, y, rc.right - padding, y + rowH};
                    DrawTextW(hdc, L"No folders visited yet.", -1, &emptyRect,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                } else {
                    for (int i = 0; i < static_cast<int>(g_displayList.size()); ++i) {
                        int rowTop = rowsTop - g_view.scrollY + i * rowH;
                        int rowBottom = rowTop + rowH;

                        const DisplayEntry &entry = g_displayList[i];
                        // Always push — index must match g_displayList for hit-testing.
                        g_rowRects.push_back({rc.left, rowTop, rc.right, rowBottom});

                        // Skip drawing rows outside the visible area.
                        if (rowBottom <= rowsTop || rowTop >= rc.bottom) {
                            g_indexRects.push_back({0, 0, 0, 0}); // placeholder for off-screen row
                            g_linkRects.push_back({0, 0, 0, 0});  // keep index-parallel
                            continue;
                        }

                        RECT rowRect = {rc.left, rowTop, rc.right, rowBottom};

                        const bool isCurrent = (!currentFolder.empty() &&
                                                HistoryPath::Equal(entry.path, currentFolder));
                        auto _sit = g_statusCache.find(entry.path);
                        const FolderStatus rowStatus = (_sit != g_statusCache.end())
                                                           ? _sit->second
                                                           : FolderStatus::Unknown;
                        const bool isMissing = (rowStatus == FolderStatus::Missing);
                        const bool isEmpty   = (rowStatus == FolderStatus::Empty);
                        // Read live from the cache, same as rowStatus above — the
                        // background sweep fills it after the list was built.
                        const bool isLink    = CachedIsSymlink(entry.path);

                        // Hover background
                        if (i == g_hoverRow) {
                            FillRect(hdc, &rowRect, UI::Gdi::Brush(
                                    isMissing
                                        ? Constants::Theme::Markers::CRITICAL
                                        : isEmpty
                                              ? RGB(50, 35, 10)
                                              : entry.isFavorite
                                                    ? RGB(50, 50, 10)
                                                    : isLink
                                                          ? Constants::Theme::HistoryPanel::ROW_HOVER_SYMLINK
                                                          : RGB(40, 60, 80)));
                        }

                        // Row index number — red for missing, orange for empty, green for current, blue otherwise
                        SetTextColor(hdc, isMissing
                                              ? Constants::Theme::HistoryPanel::PATH_DEAD_DRIVE
                                              : isEmpty
                                                    ? Constants::Theme::HistoryPanel::PATH_EMPTY_DRIVE
                                                    : (isCurrent
                                                           ? Constants::Theme::HistoryPanel::PATH_DRIVE_CURRENT
                                                           : Constants::Theme::Markers::INFO));
                        SelectObject(hdc, m_hFontIndexLink);
                        std::wstring idxStr = std::to_wstring(i + 1);
                        RECT idxRect = {
                            rc.left + padding, rowTop,
                            rc.left + padding + indexW, rowBottom
                        };
                        DrawTextW(hdc, idxStr.c_str(), -1, &idxRect,
                                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                        // Store index rect for click detection
                        g_indexRects.push_back(idxRect);
                        SelectObject(hdc, m_hFontList);

                        // Badge slot — ONE column for every mark this row carries.
                        // One badge draws itself; two or more collapse to the stack
                        // placeholder and are listed on hover. The rect is recorded
                        // for every row (empty when there is nothing to show) so
                        // g_linkRects stays index-parallel to g_displayList.
                        {
                            const auto badges = BuildRowBadges(entry.path, entry.isFavorite);
                            RECT slotRect = {
                                rc.left + padding + indexW + MulDiv(4, dpi, 96), rowTop,
                                rc.left + padding + indexW + MulDiv(4, dpi, 96) + starW, rowBottom
                            };
                            if (badges.size() == 1) {
                                SetTextColor(hdc, badges[0].color);
                                DrawTextW(hdc, badges[0].icon, -1, &slotRect,
                                          DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                            } else if (badges.size() > 1) {
                                // Its own neutral colour — the stack stands for
                                // several states at once, so borrowing any one of
                                // their colours would misreport the row at a glance.
                                SetTextColor(hdc, Constants::Theme::Markers::BADGE_STACK);
                                DrawTextW(hdc, Constants::Icon::BADGE_STACK, -1, &slotRect,
                                          DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                            }
                            g_linkRects.push_back(badges.empty() ? RECT{0, 0, 0, 0} : slotRect);
                        }

                        // Path text — three segments: drive, middle, folder
                        const bool isHov = (i == g_hoverRow);
                        const bool isFav = entry.isFavorite;

                        // Normal-row colors: light alternatives when in white mode
                        const COLORREF clrNormDrive  = app.isDarkThemed ? Constants::Theme::HistoryPanel::PATH_DRIVE  : Constants::Theme::HistoryPanel::PATH_DRIVE_LIGHT;
                        const COLORREF clrNormMiddle = app.isDarkThemed ? Constants::Theme::HistoryPanel::PATH_MIDDLE : Constants::Theme::HistoryPanel::PATH_MIDDLE_LIGHT;
                        const COLORREF clrNormFolder = app.isDarkThemed ? Constants::Theme::HistoryPanel::PATH_FOLDER : Constants::Theme::HistoryPanel::PATH_FOLDER_LIGHT;

                        // The drive letter is the segment that carries the "this is
                        // an alias" tint — it is the part that actually differs
                        // between a junction and its target (D: vs E:), so tinting
                        // it is what makes the two rows tell their own story.
                        // Status and "you are here" still win: those describe
                        // whether the row is usable, which matters more than how it
                        // is spelled. Favorite wins too — that is a user choice.
                        const COLORREF clrLinkDrive = isHov
                                                              ? Constants::Theme::HistoryPanel::PATH_SYMLINK_DRIVE_HOVER
                                                              : Constants::Theme::HistoryPanel::PATH_SYMLINK_DRIVE;
                        COLORREF driveColor = isMissing
                                                  ? Constants::Theme::HistoryPanel::PATH_DEAD_DRIVE
                                                  : isEmpty
                                                        ? Constants::Theme::HistoryPanel::PATH_EMPTY_DRIVE
                                                        : (isCurrent
                                                               ? Constants::Theme::HistoryPanel::PATH_DRIVE_CURRENT
                                                               : (isFav
                                                                      ? (isHov ? Constants::Theme::HistoryPanel::PATH_DRIVE_FAV_HOVER : Constants::Theme::HistoryPanel::PATH_DRIVE_FAV)
                                                                      : (isLink
                                                                             ? clrLinkDrive
                                                                             : (isHov
                                                                                    ? Constants::Theme::HistoryPanel::PATH_DRIVE_HOVER
                                                                                    : clrNormDrive))));
                        COLORREF middleColor = isMissing
                                                   ? Constants::Theme::HistoryPanel::PATH_DEAD_MIDDLE
                                                   : isEmpty
                                                         ? Constants::Theme::HistoryPanel::PATH_EMPTY_MIDDLE
                                                         : (isCurrent
                                                                ? Constants::Theme::HistoryPanel::PATH_MIDDLE_CURRENT
                                                                : (isFav
                                                                       ? Constants::Theme::Markers::FAVORITES
                                                                       : (isHov
                                                                              ? RGB(255, 255, 255)
                                                                              : clrNormMiddle)));
                        COLORREF folderColor = isMissing
                                                   ? Constants::Theme::HistoryPanel::PATH_DEAD_FOLDER
                                                   : isEmpty
                                                         ? Constants::Theme::HistoryPanel::PATH_EMPTY_FOLDER
                                                         : (isCurrent
                                                                ? Constants::Theme::HistoryPanel::PATH_FOLDER_CURRENT
                                                                : (isFav
                                                                       ? (isHov ? Constants::Theme::HistoryPanel::PATH_FOLDER_FAV_HOVER : Constants::Theme::HistoryPanel::PATH_FOLDER_FAV)
                                                                       : (isHov
                                                                              ? Constants::Theme::HistoryPanel::PATH_FOLDER_HOVER
                                                                              : clrNormFolder)));

                        // Split path into (drive, middle, folder)
                        const std::wstring &fp = entry.path;
                        std::wstring segDrive, segMiddle, segFolder;
                        int folderStartIdx = 0; // index in fp where segFolder begins (before posLabel)
                        if (fp.size() >= 2 && fp[1] == L':') {
                            segDrive = fp.substr(0, 2);
                            size_t lastSep = fp.find_last_of(L"\\/");
                            if (lastSep != std::wstring::npos && lastSep >= 2) {
                                segMiddle = fp.substr(2, lastSep - 1); // "\rest\of\path\"
                                folderStartIdx = static_cast<int>(lastSep + 1);
                                segFolder = fp.substr(lastSep + 1); // "FolderName"
                            } else {
                                folderStartIdx = 2;
                                segFolder = fp.substr(2);
                            }
                        } else {
                            segMiddle = fp;
                        }
                        const int folderRawLen = static_cast<int>(segFolder.size());

                        // Append spawned DirWnd position label if this folder has one open
                        std::wstring posLabel = uiManager.GetSpawnedDirWndPositionLabel(fp);
                        segFolder += posLabel;

                        // Build isHL array from stored match positions
                        const int fpLen = static_cast<int>(fp.size());
                        bool isHL[1024] = {};
                        if (!g_filter.IsEmpty() && entry.matchPosCount > 0) {
                            for (int k = 0; k < entry.matchPosCount; ++k) {
                                int pos = entry.matchPositions[k];
                                if (pos >= 0 && pos < fpLen && pos < 1024)
                                    isHL[pos] = true;
                            }
                        }
                        const bool hasHL = !g_filter.IsEmpty() && entry.matchPosCount > 0;

                        // Baseline y for DrawMatchText (vertically centers text in the row)
                        TEXTMETRIC tm;
                        GetTextMetrics(hdc, &tm);
                        const int textBaseY = rowTop + (rowH - tm.tmHeight) / 2;


                        // Size/count column — drawn on the far right, only when a SpawnedDirWnd is open for this folder
                        auto [sizeStr, imgCount] = uiManager.GetSpawnedDirWndSizeInfo(fp);
                        // Fall back to the scanned cache (populated by F5) when no DirWnd is open for this folder
                        if (sizeStr.empty()) {
                            auto cit = g_dirSizeCache.find(fp);
                            if (cit != g_dirSizeCache.end()) {
                                sizeStr  = ThumbnailPanelWnd::FormatDirSize(cit->second.bytes);
                                imgCount = cit->second.count;
                            }
                        }
                        const bool hasSizeInfo = !sizeStr.empty();
                        int sizeColW = hasSizeInfo ? MulDiv(100, dpi, 96) : 0;

                        LONG rowLeft = rc.left + padding + indexW + starW + MulDiv(10, dpi, 96);
                        LONG rowRight = rc.right - padding - (needsScrollbar ? SB_W + 2 : 0) - sizeColW;
                        LONG curX = rowLeft;

                        // 1. Drive letter
                        if (!segDrive.empty() && curX < rowRight) {
                            SIZE sz = {};
                            GetTextExtentPoint32W(hdc, segDrive.c_str(),
                                                  static_cast<int>(segDrive.size()), &sz);
                            RECT dr = {curX, rowTop, curX + sz.cx, rowBottom};
                            if (hasHL) {
                                const COLORREF clrHL = Constants::Theme::ThemedColor(1.0f, 0.87f, 0.0f, app.themeFactor);
                                Common::DrawMatchText(hdc, segDrive.c_str(),
                                    static_cast<int>(segDrive.size()),
                                    isHL, curX, textBaseY, dr, driveColor, clrHL);
                            } else {
                                SetTextColor(hdc, driveColor);
                                DrawTextW(hdc, segDrive.c_str(), -1, &dr,
                                          DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                            }
                            curX += sz.cx;
                        }

                        // 2. Middle path — reserve space for folder before drawing
                        if (!segMiddle.empty() && curX < rowRight) {
                            LONG folderReserve = 0;
                            if (!segFolder.empty()) {
                                SIZE szF = {};
                                GetTextExtentPoint32W(hdc, segFolder.c_str(),
                                                      static_cast<int>(segFolder.size()), &szF);
                                folderReserve = std::min(szF.cx, (rowRight - curX) * 2 / 5);
                            }
                            LONG midRight = rowRight - folderReserve;
                            SIZE szM = {};
                            GetTextExtentPoint32W(hdc, segMiddle.c_str(),
                                                  static_cast<int>(segMiddle.size()), &szM);
                            RECT mr = {curX, rowTop, midRight, rowBottom};
                            if (hasHL) {
                                const COLORREF clrHL = Constants::Theme::ThemedColor(1.0f, 0.87f, 0.0f, app.themeFactor);
                                // Middle chars start at index driveLen (2) in the full path
                                Common::DrawMatchText(hdc, segMiddle.c_str(),
                                    static_cast<int>(segMiddle.size()),
                                    isHL + static_cast<int>(segDrive.size()),
                                    curX, textBaseY, mr, middleColor, clrHL);
                            } else {
                                SetTextColor(hdc, middleColor);
                                DrawTextW(hdc, segMiddle.c_str(), -1, &mr,
                                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                            }
                            curX = (szM.cx < midRight - curX) ? curX + szM.cx : midRight;
                        }

                        // 3. Folder name
                        if (!segFolder.empty() && curX < rowRight) {
                            RECT fr = {curX, rowTop, rowRight, rowBottom};
                            if (hasHL && folderRawLen > 0) {
                                const COLORREF clrHL = Constants::Theme::ThemedColor(1.0f, 0.87f, 0.0f, app.themeFactor);
                                // Highlight only the raw folder chars (not posLabel suffix)
                                Common::DrawMatchText(hdc, segFolder.c_str(), folderRawLen,
                                    isHL + folderStartIdx, curX, textBaseY, fr,
                                    folderColor, clrHL);
                                // Draw posLabel suffix without highlight
                                if (!posLabel.empty()) {
                                    SIZE szRaw = {};
                                    GetTextExtentPoint32W(hdc, segFolder.c_str(), folderRawLen, &szRaw);
                                    LONG labelX = curX + szRaw.cx;
                                    if (labelX < rowRight) {
                                        RECT lr = {labelX, rowTop, rowRight, rowBottom};
                                        SetTextColor(hdc, folderColor);
                                        DrawTextW(hdc, posLabel.c_str(), -1, &lr,
                                                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                                    }
                                }
                            } else {
                                SetTextColor(hdc, folderColor);
                                DrawTextW(hdc, segFolder.c_str(), -1, &fr,
                                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                            }
                        }

                        // 4. Size/count column — right-aligned after the path, only when a SpawnedDirWnd is open
                        if (hasSizeInfo) {
                            std::wstring sizeCountStr = sizeStr + L"/" + std::to_wstring(imgCount);
                            LONG colLeft  = rowRight;
                            LONG colRight = rc.right - padding - (needsScrollbar ? SB_W + 2 : 0);
                            // An alias row shows its size in the link colour rather
                            // than the usual green: the files are real, but they
                            // were counted under the folder they actually live in,
                            // so this figure is informational and is NOT part of
                            // the footer total. The colour is the only thing that
                            // says so — see ComputeHistoryTotals.
                            SetTextColor(hdc, isMissing
                                                  ? Constants::Theme::HistoryPanel::PATH_DEAD_MIDDLE
                                                  : isEmpty
                                                        ? Constants::Theme::HistoryPanel::PATH_EMPTY_MIDDLE
                                                        : isLink
                                                              ? Constants::Theme::Markers::SYMLINK
                                                              : Constants::Theme::Markers::OK);
                            RECT scr = {colLeft, rowTop, colRight, rowBottom};
                            DrawTextW(hdc, sizeCountStr.c_str(), -1, &scr,
                                      DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                        }
                    }
                }

                RestoreDC(hdc, -1);

                // Scrollbar — body area only, between header and footer.
                //
                // The track is set by hand rather than by Layout because the bar
                // occupies the BODY band while the rows are laid out against the
                // whole window; Layout's single region cannot express that.
                //
                // The view is set UNCONDITIONALLY, bar or no bar. It is what the
                // base clamps the wheel against and what sizes the thumb, so a
                // view left empty on a short list would let the wheel run away
                // and be silently corrected by the next paint.
                g_view.view = {rc.left, rowsTop, rc.right - SB_W, bodyBottom};
                g_view.ClearBars();
                if (needsScrollbar)
                    g_view.vTrack = {rc.right - SB_W, rowsTop, rc.right, bodyBottom};
                DrawBars(hdc, g_view, app.dpiScale,
                         UI::ThemeScrollBarColors(app.themeFactor));

                // Footer separator line
                {
                    HPEN hOldFooterPen = (HPEN) SelectObject(hdc, UI::Gdi::Pen(RGB(50, 50, 50)));
                    MoveToEx(hdc, rc.left + padding, footerSepY, nullptr);
                    LineTo(hdc, rc.right - padding, footerSepY);
                    SelectObject(hdc, hOldFooterPen);
                }

                // FOOTER — QIV link | [Fkey] Cache | [Fkey] Dir  ···  summary  history-size
                {
                    int footerTop = footerSepY + MulDiv(4, dpi, 96);
                    int footerBot = footerSepY + MulDiv(fontSize + 2 + 4, dpi, 96);

                    SelectObject(hdc, m_hFontBody);
                    SetTextColor(hdc, RGB(150, 150, 150));

                    const UI::PanelLayout &layout = uiManager.GetLayout();
                    const UI::SlotInfo *slots[] = {&layout.center, &layout.top, &layout.right, &layout.bottom, &layout.left};
                    LONG rightEdge = rc.right - padding;
                    const int gap   = MulDiv(8, dpi, 96);

                    // RIGHT: summary just left of file size, then file size rightmost
                    const std::wstring &sizeValue = m_cachedSizeStr;
                    SIZE szFileSize = {};
                    GetTextExtentPoint32W(hdc, sizeValue.c_str(),
                                          static_cast<int>(sizeValue.size()), &szFileSize);
                    LONG fileSizeLeft = rightEdge - szFileSize.cx;

                    // Summary total: scanned cache (F5) when available, else live open-DirWnd sum
                    std::wstring summaryStr;
                    g_summaryTipText.clear();
                    {
                        if (!g_dirSizeCache.empty()) {
                            // Aliases, missing and empty folders all drop out —
                            // see ComputeHistoryTotals. Cached: recomputing this
                            // per paint is O(rows) plus three sorts.
                            const HistoryTotals &t = HistoryTotalsCached();
                            summaryStr = ThumbnailPanelWnd::FormatDirSize(t.bytes)
                                         + L"/" + std::to_wstring(t.files);
                            // Third field only when something was actually left
                            // out — an ordinary history should not grow a "/0".
                            if (t.excludedCount() > 0)
                                summaryStr += L"/" + std::to_wstring(t.excludedCount());

                            // Hover text, built here where the numbers are known.
                            const std::wstring dirs = Constants::Messages::WORD_DIRS;
                            g_summaryTipText =
                                    std::wstring(Constants::Messages::TOTAL_HEADER) + L"\n" +
                                    Constants::Messages::TOTAL_SIZE_LABEL +
                                    ThumbnailPanelWnd::FormatDirSize(t.bytes) + L"\n" +
                                    Constants::Messages::TOTAL_FILES_LABEL +
                                    std::to_wstring(t.files) + L"\n" +
                                    Constants::Messages::TOTAL_DIRS_LABEL +
                                    std::to_wstring(t.scanned);
                            if (t.excludedCount() > 0) {
                                g_summaryTipText += L"\n";
                                g_summaryTipText += Constants::Messages::TOTAL_SEPARATOR;
                                // Indented one space: a breakdown of the Dirs line
                                // above, not a fourth headline figure.
                                g_summaryTipText += L"\n " + std::to_wstring(t.excludedCount()) +
                                                    L" " + dirs +
                                                    Constants::Messages::TOTAL_EXCLUDED_SUFFIX;
                                int n = 1; // continuous numbering across all groups
                                AppendExcludedGroup(g_summaryTipText,
                                                    Constants::Messages::EXCLUDED_DUPLICATES,
                                                    t.duplicates, n, /*showTarget=*/true);
                                AppendExcludedGroup(g_summaryTipText,
                                                    Constants::Messages::EXCLUDED_MISSING + dirs + L":",
                                                    t.missing, n, /*showTarget=*/false);
                                AppendExcludedGroup(g_summaryTipText,
                                                    Constants::Messages::EXCLUDED_EMPTY + dirs + L":",
                                                    t.empty, n, /*showTarget=*/false);
                            }
                        } else {
                            auto [liveStr, liveCount] = uiManager.GetAllOpenDirWndsSummary();
                            if (!liveStr.empty())
                                summaryStr = liveStr + L"/" + std::to_wstring(liveCount);
                        }
                    }
                    LONG summaryLeft = fileSizeLeft;
                    g_summaryRect = RECT{0, 0, 0, 0};
                    if (!summaryStr.empty()) {
                        SIZE szSummary = {};
                        GetTextExtentPoint32W(hdc, summaryStr.c_str(),
                                              static_cast<int>(summaryStr.size()), &szSummary);
                        summaryLeft = fileSizeLeft - gap - szSummary.cx;
                        SetTextColor(hdc, Constants::Theme::Markers::OK);
                        RECT summaryRect = {summaryLeft, footerTop, summaryLeft + szSummary.cx, footerBot};
                        DrawTextW(hdc, summaryStr.c_str(), -1, &summaryRect,
                                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                        g_summaryRect = summaryRect; // hover target for the breakdown
                    }
                    SetTextColor(hdc, Constants::Theme::HistoryPanel::SIZE_HIGHLIGHT);
                    RECT sizeRect = {fileSizeLeft, footerTop, rightEdge, footerBot};
                    DrawTextW(hdc, sizeValue.c_str(), -1, &sizeRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                    // LEFT: QIV link first, then [Fkey] Cache, then [Fkey] Dir
                    {
                        LONG curX     = rc.left + padding;
                        LONG leftBound = summaryLeft - gap;

                        // QIV→dir link — first item, clickable
                        {
                            std::wstring linkText = L"QIV.exe/path="
                                                    + std::filesystem::path(Persistence::Registry::GetExePathW()).parent_path().wstring() + L"\\";
                            SIZE szLink = {};
                            SelectObject(hdc, m_hFontLink);
                            GetTextExtentPoint32W(hdc, linkText.c_str(),
                                                  static_cast<int>(linkText.size()), &szLink);
                            SetTextColor(hdc, Constants::Theme::Markers::INFO);
                            g_exeLinkRect = {curX, footerTop, curX + szLink.cx, footerBot};
                            DrawTextW(hdc, linkText.c_str(), -1, &g_exeLinkRect,
                                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                            curX += szLink.cx + gap;
                            SelectObject(hdc, m_hFontBody);
                        }

                        // Cache toggle key
                        std::wstring cacheKeyLabel = L"[" + FKeyLabel(Shortcuts::SC_PANEL_CACHE_TOGGLE) + L"]";
                        SetTextColor(hdc, Constants::Theme::Markers::INFO);
                        SelectObject(hdc, m_hFontLink);
                        SIZE szToggle = {};
                        GetTextExtentPoint32W(hdc, cacheKeyLabel.c_str(),
                                              static_cast<int>(cacheKeyLabel.size()), &szToggle);
                        g_cacheIndexRect = {curX, footerTop, curX + szToggle.cx, footerBot};
                        DrawTextW(hdc, cacheKeyLabel.c_str(), -1, &g_cacheIndexRect,
                                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                        curX += szToggle.cx;

                        // Cache status
                        SelectObject(hdc, m_hFontBody);
                        SetTextColor(hdc, RGB(150, 150, 150));
                        bool cacheFound = false;
                        std::wstring cachePosName;
                        for (auto *slot: slots) {
                            if (slot->panel == &uiManager.getCacheWindow() && slot->panel->IsVisible()) {
                                cachePosName = slot->name;
                                cacheFound = true;
                                break;
                            }
                        }
                        std::wstring cacheRest = cacheFound ? (L"Cache->" + cachePosName) : L"Cache->Hidden";
                        SIZE szCacheRest = {};
                        GetTextExtentPoint32W(hdc, cacheRest.c_str(),
                                              static_cast<int>(cacheRest.size()), &szCacheRest);
                        RECT cacheRestRect = {curX, footerTop, curX + szCacheRest.cx, footerBot};
                        DrawTextW(hdc, cacheRest.c_str(), -1, &cacheRestRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                        curX += szCacheRest.cx + gap;

                        // Dir-panel toggle key
                        std::wstring dirKeyLabel = L"[" + FKeyLabel(Shortcuts::SC_PANEL_DIR_TOGGLE) + L"]";
                        SetTextColor(hdc, Constants::Theme::Markers::INFO);
                        SelectObject(hdc, m_hFontLink);
                        SIZE szF5 = {};
                        GetTextExtentPoint32W(hdc, dirKeyLabel.c_str(),
                                              static_cast<int>(dirKeyLabel.size()), &szF5);
                        g_f5IndexRect = {curX, footerTop, curX + szF5.cx, footerBot};
                        DrawTextW(hdc, dirKeyLabel.c_str(), -1, &g_f5IndexRect,
                                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                        curX += szF5.cx;

                        // Dir status
                        SelectObject(hdc, m_hFontBody);
                        SetTextColor(hdc, RGB(150, 150, 150));
                        bool f5Found = false;
                        std::wstring f5PosName;
                        int f5HistoryIndex = -1;
                        for (auto *slot: slots) {
                            if (slot->panel == &uiManager.getDirWindow() && slot->panel->IsVisible()) {
                                if (app.currentIndex >= 0 && app.currentIndex < static_cast<int>(app.playlist.size())) {
                                    std::wstring currentImagePath = app.playlist[app.currentIndex];
                                    std::wstring f5Folder = std::filesystem::path(currentImagePath).parent_path().wstring();
                                    const auto &history = historyFoldersManager.folderHistory;
                                    for (int i = 0; i < static_cast<int>(history.size()); ++i) {
                                        if (_wcsicmp(f5Folder.c_str(), history[i].c_str()) == 0) {
                                            f5HistoryIndex = i + 1;
                                            break;
                                        }
                                    }
                                }
                                f5PosName = slot->name;
                                f5Found = true;
                                break;
                            }
                        }
                        std::wstring f5Rest = f5Found
                            ? ((f5HistoryIndex > 0) ? (L"Dir->#" + std::to_wstring(f5HistoryIndex) + L" " + f5PosName) : (L"Dir->? " + f5PosName))
                            : L"Dir->Hidden";
                        SIZE szF5Rest = {};
                        GetTextExtentPoint32W(hdc, f5Rest.c_str(),
                                              static_cast<int>(f5Rest.size()), &szF5Rest);
                        RECT f5RestRect = {curX, footerTop, std::min(curX + szF5Rest.cx, leftBound), footerBot};
                        DrawTextW(hdc, f5Rest.c_str(), -1, &f5RestRect,
                                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                    }
                }

                // Filter row — input box with built-in ✕ (consistent with Find / JumpTo)
                {
                    int filterH    = MulDiv(Constants::History::HISTORY_FILTER_ROW_H, dpi, 96);
                    int filterSepY = rc.bottom - filterH;
                    int gap        = MulDiv(4,  dpi, 96);
                    int inset      = MulDiv(3,  dpi, 96);

                    // Separator above filter row
                    HPEN hOldFP = (HPEN)SelectObject(hdc,
                        UI::Gdi::Pen(Constants::Theme::ThemedGray(0.22f, app.themeFactor)));
                    MoveToEx(hdc, rc.left + padding, filterSepY, nullptr);
                    LineTo(hdc, rc.right - padding, filterSepY);
                    SelectObject(hdc, hOldFP);

                    SelectObject(hdc, m_hFontBody);
                    RECT boxRect = { rc.left + padding, filterSepY + inset,
                                     rc.right - padding, rc.bottom - inset };
                    g_filter.Draw(hdc, m_hFontBody, boxRect, gap, GetFocus() == m_hWnd);
                }

                BitBlt(screenDC, 0, 0, rc.right, rc.bottom, hdc, 0, 0, SRCCOPY);
                EndPaint(m_hWnd, &ps);
                return 0;
            }

            case WM_CHAR: {
                wchar_t ch = static_cast<wchar_t>(wParam);
                // Space with empty filter → let OnKeyDown's favorite-toggle handle it
                if (ch == L' ' && g_filter.IsEmpty()) return 0;
                if (g_filter.RouteChar(ch, m_hWnd) == InputResult::ConsumedRepaint)
                    InvalidateRect(m_hWnd, nullptr, FALSE); // OnChanged also repaints
                return 0;
            }

            case WM_GETMINMAXINFO: {
                UINT dpiMM = static_cast<UINT>(app.dpiScale * 96.0f);
                auto *mmi = reinterpret_cast<MINMAXINFO *>(lParam);
                mmi->ptMinTrackSize.x = MulDiv(Constants::History::HISTORY_MIN_W, dpiMM, 96);
                mmi->ptMinTrackSize.y = MulDiv(Constants::History::HISTORY_MIN_H, dpiMM, 96);
                mmi->ptMaxTrackSize.x = MulDiv(Constants::History::HISTORY_MAX_W, dpiMM, 96);
                mmi->ptMaxTrackSize.y = MulDiv(Constants::History::HISTORY_MAX_H, dpiMM, 96);
                return 0;
            }

            case WM_NCHITTEST: {
                POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                RECT wrc;
                GetWindowRect(m_hWnd, &wrc);
                const int border = std::max(4, static_cast<int>(6 * app.dpiScale));
                bool top = pt.y < wrc.top + border;
                bool bottom = pt.y >= wrc.bottom - border;
                bool left = pt.x < wrc.left + border;
                bool right = pt.x >= wrc.right - border;
                if (top && left) return HTTOPLEFT;
                if (top && right) return HTTOPRIGHT;
                if (bottom && left) return HTBOTTOMLEFT;
                if (bottom && right) return HTBOTTOMRIGHT;
                if (top) return HTTOP;
                if (bottom) return HTBOTTOM;
                if (left) return HTLEFT;
                if (right) return HTRIGHT;
                return HTCLIENT;
            }

            case WM_LBUTTONDOWN: {
                int mx = GET_X_LPARAM(lParam);
                int my = GET_Y_LPARAM(lParam);
                RECT rc2{};
                GetClientRect(m_hWnd, &rc2);
                UINT dpi2 = static_cast<UINT>(app.dpiScale * 96.0f);

                // Filter input (✕ button or click-to-position-caret) — handle
                // before anything else. Ignored → falls through to header/rows.
                if (g_filter.RouteMouse(WM_LBUTTONDOWN, wParam, lParam, m_hWnd) == InputResult::ConsumedRepaint) {
                    InvalidateRect(m_hWnd, nullptr, FALSE);
                    return 0;
                }

                // Calculate header area
                int padding2 = MulDiv(Constants::History::HISTORY_PADDING, dpi2, 96);
                int titleSz2 = MulDiv(Constants::History::HISTORY_FONT_SIZE + 2, dpi2, 96);
                int fontSize2 = MulDiv(Constants::History::HISTORY_FONT_SIZE, dpi2, 96);
                int headerBottom = padding2 + titleSz2 + 4 + MulDiv(2, dpi2, 96) + fontSize2 + 2 + MulDiv(4, dpi2, 96);

                // Check if clicking in header area — track screen coords to avoid drift
                // Skip drag-start when the click lands on a shortcut link.
                {
                    POINT ptLink = {mx, my};
                    if ((g_shortcutF5Rect.right     > g_shortcutF5Rect.left     && PtInRect(&g_shortcutF5Rect,     ptLink)) ||
                        (g_shortcutCtrlTabRect.right > g_shortcutCtrlTabRect.left && PtInRect(&g_shortcutCtrlTabRect, ptLink)))
                        return 0;
                }
                if (my < headerBottom) {
                    g_headerDragging = true;
                    POINT ptScreen = {mx, my};
                    ClientToScreen(m_hWnd, &ptScreen);
                    g_headerDragStartX = ptScreen.x;
                    g_headerDragStartY = ptScreen.y;
                    GetWindowRect(m_hWnd, &g_headerDragWindowRect);
                    SetCapture(m_hWnd);
                    return 0;
                }

                // No scrollbar-drag block: the base consumes thumb and track
                // clicks before this panel is asked, and it distinguishes the
                // two — this code treated the whole bar column as a grab handle,
                // so a click on empty track teleported the list instead of
                // paging it.
                return 0;
            }

            // No wheel case — the base consumes it, so nothing here would run.
            // The link tip is dropped from OnScrolled instead, which fires for
            // EVERY scroll rather than only the wheel: paging the track and
            // dragging the thumb move rows under the cursor just as much.

            case WM_MOUSEMOVE: {
                int mx = GET_X_LPARAM(lParam);
                int my = GET_Y_LPARAM(lParam);

                // Windows synthesizes a WM_MOUSEMOVE (cursor stationary) whenever
                // the window stack under the cursor changes — e.g. a spawned panel
                // opening/closing. Acting on it would stomp the keyboard selection
                // with whatever row happens to sit under the resting cursor.
                // Only react when the cursor actually moved.
                static POINT s_lastHoverPos = {LONG_MIN, LONG_MIN};
                if (mx == s_lastHoverPos.x && my == s_lastHoverPos.y)
                    return 0;
                s_lastHoverPos = {mx, my};

                // Ask for WM_MOUSELEAVE. Without this it never arrives, and the
                // cursor can leave the panel — most easily straight down past the
                // footer — with no further WM_MOUSEMOVE to dismiss a hover popup,
                // stranding it on screen. Re-armed every move; Windows disarms the
                // request as soon as it fires.
                {
                    TRACKMOUSEEVENT tme{sizeof(TRACKMOUSEEVENT)};
                    tme.dwFlags = TME_LEAVE;
                    tme.hwndTrack = m_hWnd;
                    TrackMouseEvent(&tme);
                }

                // ✕ hover color — repaint only when state changes
                if (g_filter.RouteMouse(WM_MOUSEMOVE, wParam, lParam, m_hWnd) == InputResult::ConsumedRepaint)
                    InvalidateRect(m_hWnd, nullptr, FALSE);

                // Badge slot hover → popup listing every mark on that row, one per
                // line. g_linkRects is index-parallel to g_displayList and holds an
                // empty rect for unmarked and off-screen rows, so PtInRect alone is
                // a sufficient test.
                {
                    const POINT ptLink = {mx, my};
                    int linkRow = -1;
                    const int nLink = std::min(static_cast<int>(g_linkRects.size()),
                                               static_cast<int>(g_displayList.size()));
                    for (int i = 0; i < nLink; ++i) {
                        if (g_linkRects[i].right > g_linkRects[i].left &&
                            PtInRect(&g_linkRects[i], ptLink)) {
                            linkRow = i;
                            break;
                        }
                    }
                    if (linkRow >= 0) {
                        ShowLinkTip(m_hWnd, linkRow,
                                    BadgeTipText(BuildRowBadges(g_displayList[linkRow].path,
                                                                g_displayList[linkRow].isFavorite)),
                                    ptLink, g_linkRects[linkRow]);
                    } else if (g_summaryRect.right > g_summaryRect.left &&
                               PtInRect(&g_summaryRect, ptLink) && !g_summaryTipText.empty()) {
                        // Footer total → size / file count / what was left out.
                        // Row index -2 so it cannot collide with a real row and
                        // the "same target, don't re-show" guard still works.
                        ShowLinkTip(m_hWnd, -2, g_summaryTipText, ptLink, g_summaryRect);
                    } else {
                        HideLinkTip();
                    }
                }

                // Handle header dragging to move window
                if (g_headerDragging) {
                    POINT ptScreen = {mx, my};
                    ClientToScreen(m_hWnd, &ptScreen);
                    int newX = g_headerDragWindowRect.left + (ptScreen.x - g_headerDragStartX);
                    int newY = g_headerDragWindowRect.top + (ptScreen.y - g_headerDragStartY);
                    SetWindowPos(m_hWnd, nullptr, newX, newY, 0, 0,
                                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                    return 0;
                }

                // Whether something above the rows already owns the cursor. The
                // row logic at the bottom must not overwrite it — that is what
                // this flag is for, and its absence was a bug: the arrow set
                // here ran on EVERY move while the row's hand was set only on
                // the frame the hovered row CHANGED, so the hand appeared for
                // one message at each row boundary and the arrow came back
                // inside the row. Exactly backwards.
                bool cursorClaimed = false;

                if (g_scanRunning) {
                    // Arrow-with-circle: work is happening in the background. The
                    // panel is NOT blocked — the user can scroll, pick a folder, or
                    // close it, and the sweep carries on either way.
                    SetCursor(Constants::Cursors::CURR_APPSTARTING);
                    cursorClaimed = true;
                } else if (PtInRect(&g_view.vTrack, {mx, my})) {
                    // The scrollbar's cursor belongs to FloatingPanelWnd now, and
                    // it has already answered for this position — the panel used
                    // to test the right-edge strip itself, with its own width.
                    cursorClaimed = true;
                } else {
                    POINT ptMov = {mx, my};
                    const RECT& cr = g_filter.GetClearRect();
                    if ((cr.right > cr.left) && PtInRect(&cr, ptMov)) {
                        SetCursor(Constants::Cursors::CURR_CLICK);
                        cursorClaimed = true;
                    }
                }

                // A thumb drag never reaches here — the base holds capture for
                // the whole of it, so the hover logic below cannot fight one.

                int newHover = -1;
                for (int i = 0; i < static_cast<int>(g_rowRects.size()); ++i) {
                    const RECT &r = g_rowRects[i];
                    if (mx >= r.left && mx < r.right && my >= r.top && my < r.bottom) {
                        newHover = i;
                        break;
                    }
                }
                // Hand cursor over clickable links/indexes
                {
                    POINT pt = {mx, my};
                    if (g_shortcutF5Rect.right > g_shortcutF5Rect.left && PtInRect(&g_shortcutF5Rect, pt)) {
                        SetCursor(Constants::Cursors::CURR_CLICK);
                        return 0;
                    }
                    if (g_shortcutCtrlTabRect.right > g_shortcutCtrlTabRect.left && PtInRect(&g_shortcutCtrlTabRect, pt)) {
                        SetCursor(Constants::Cursors::CURR_CLICK);
                        return 0;
                    }
                    if (g_exeLinkRect.right > g_exeLinkRect.left && PtInRect(&g_exeLinkRect, pt)) {
                        SetCursor(Constants::Cursors::CURR_CLICK);
                        return 0;
                    }
                    if (g_cacheIndexRect.right > g_cacheIndexRect.left && PtInRect(&g_cacheIndexRect, pt)) {
                        SetCursor(Constants::Cursors::CURR_CLICK);
                        return 0;
                    }
                    if (g_f5IndexRect.right > g_f5IndexRect.left && PtInRect(&g_f5IndexRect, pt)) {
                        SetCursor(Constants::Cursors::CURR_CLICK);
                        return 0;
                    }
                }
                // Hand cursor over history row indexes
                for (const auto &idxRect: g_indexRects) {
                    if (idxRect.right > idxRect.left) {
                        POINT pt = {mx, my};
                        if (PtInRect(&idxRect, pt)) {
                        SetCursor(Constants::Cursors::CURR_CLICK);
                        return 0;
                        }
                    }
                }

                // CURSOR EVERY MOVE, REPAINT ONLY ON CHANGE. These two were tied
                // together and they answer different questions: the cursor is
                // "where is the pointer now", which is true on every message,
                // while the repaint is "did the highlighted row change", which
                // is not. Tying them made the hand a one-frame flash at each row
                // boundary — see the note by cursorClaimed above.
                if (!cursorClaimed)
                    SetCursor((newHover >= 0) ? Constants::Cursors::CURR_CLICK
                                              : Constants::Cursors::CURR_DEFAULT);

                if (newHover != g_hoverRow) {
                    g_hoverRow = newHover;
                    InvalidateRect(m_hWnd, nullptr, FALSE);
                }
                return 0;
            }

            case WM_LBUTTONUP: {
                if (g_headerDragging) {
                    g_headerDragging = false;
                    ReleaseCapture();
                    return 0;
                }
                // A scrollbar release never arrives here — the base owns that
                // button-up — so it cannot be mistaken for the filter's.
                // A drag-select started in the filter box owns this release — end
                // it here (otherwise the box only drops m_dragging on the next
                // WM_MOUSEMOVE) and return, so a drag that happens to end over a
                // history row cannot also open that folder.
                if (g_filter.RouteMouse(WM_LBUTTONUP, wParam, lParam, m_hWnd) != InputResult::Ignored) {
                    InvalidateRect(m_hWnd, nullptr, FALSE);
                    return 0;
                }
                int mx = GET_X_LPARAM(lParam);
                int my = GET_Y_LPARAM(lParam);

                // "F5" shortcut click — reload history from disk, rebuild, then scan statuses
                if (g_shortcutF5Rect.right > g_shortcutF5Rect.left) {
                    POINT pt = {mx, my};
                    if (PtInRect(&g_shortcutF5Rect, pt)) {
                        RefreshHistory(m_hWnd);
                        return 0;
                    }
                }

                // "Ctrl+Tab" shortcut click — toggle full history view
                if (g_shortcutCtrlTabRect.right > g_shortcutCtrlTabRect.left) {
                    POINT pt = {mx, my};
                    if (PtInRect(&g_shortcutCtrlTabRect, pt)) {
                        ToggleFullHistory(m_hWnd);
                        return 0;
                    }
                }

                // Exe-dir link click
                if (g_exeLinkRect.right > g_exeLinkRect.left) {
                    POINT pt = {mx, my};
                    if (PtInRect(&g_exeLinkRect, pt)) {
                        std::wstring dir = std::filesystem::path(Persistence::Registry::GetExePathW()).parent_path().wstring();
                        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOW);
                        return 0;
                    }
                }

                // [F6] Dir label click
                if (g_f5IndexRect.right > g_f5IndexRect.left) {
                    POINT pt = {mx, my};
                    if (PtInRect(&g_f5IndexRect, pt)) {
                        InputManager::ExecuteCommand(g_hHistOwner, Command::ToggleDir);
                        return 0;
                    }
                }

                // [F3] Cache label click
                if (g_cacheIndexRect.right > g_cacheIndexRect.left) {
                    POINT pt = {mx, my};
                    if (PtInRect(&g_cacheIndexRect, pt)) {
                        InputManager::ExecuteCommand(g_hHistOwner, Command::ToggleCache);
                        return 0;
                    }
                }

                // History row index click — open in explorer
                for (int i = 0; i < static_cast<int>(g_indexRects.size()); ++i) {
                    const RECT &idxRect = g_indexRects[i];
                    if (idxRect.right > idxRect.left) {
                        POINT pt = {mx, my};
                        if (PtInRect(&idxRect, pt)) {
                            if (i < static_cast<int>(g_displayList.size())) {
                                std::wstring folder = g_displayList[i].path;
                                ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOW);
                            }
                            return 0;
                        }
                    }
                }

                // History row path click — open in app
                for (int i = 0; i < static_cast<int>(g_rowRects.size()); ++i) {
                    const RECT &r = g_rowRects[i];
                    if (mx >= r.left && mx < r.right && my >= r.top && my < r.bottom) {
                        std::wstring folder = g_displayList[i].path;
                        FolderStatus fs = GetFolderStatus(folder);
                        if (fs == FolderStatus::Missing) {
                            if (g_hHistOwner)
                                g_overlayManager.PostCenterMessage(g_hHistOwner,
                                    Constants::Messages::FOLDER_DEAD_MISSING);
                            return 0;
                        }
                        // Empty folders fall through — OpenDirectory handles them.
                        ShowWindow(m_hWnd, SW_HIDE);
                        OpenDirectory(g_hHistOwner, folder);
                        NotifyCurrentFolder(folder); // keep green on the clicked row, not the link target
                        return 0;
                    }
                }
                return 0;
            }

            case WM_RBUTTONUP: {
                // Right-click inside the filter → Cut/Copy/Paste menu.
                if (g_filter.RouteMouse(WM_RBUTTONUP, wParam, lParam, m_hWnd) == InputResult::ConsumedRepaint)
                    InvalidateRect(m_hWnd, nullptr, FALSE);
                return 0;
            }

            case WM_MOUSELEAVE: {
                g_filter.RouteMouse(WM_MOUSELEAVE, wParam, lParam, m_hWnd);
                g_hoverRow = -1;
                HideLinkTip(); // cursor left the panel — the popup must not linger
                InvalidateRect(m_hWnd, nullptr, FALSE);
                return 0;
            }

            case WM_DESTROY:
                // Hide, do NOT destroy: the popup is an app-wide singleton owned by
                // the main window and reused by every panel.
                HideLinkTip();
                break; // let the base router run its own WM_DESTROY handling

            case WM_CLOSE:
                HideLinkTip();
                ShowWindow(m_hWnd, SW_HIDE);
                return 0;

        }

        return DefWindowProcW(m_hWnd, message, wParam, lParam);
    }

    // ---------------------------------------------------------------------------
    // Public API
    // ---------------------------------------------------------------------------
    void HistoryListWnd::Init(HINSTANCE hInstance, HWND hParent, int8_t /*position*/) {
        Init(hInstance, hParent);
    }

    void HistoryListWnd::Init(HINSTANCE hInstance, HWND hParent) {
        g_hHistOwner = hParent;
        g_filter.SetPlaceholder(L"type to filter…");
        g_filter.SetMaxLength(Common::FUZZY_MAX_QUERY);
        BuildDisplayList();
        int x, y, w, h;
        GetHistoryWindowBounds(hParent, x, y, w, h);
        InitFloating(hInstance, hParent, L"QIV_HistoryWindow", L"Folder History",
                     w, h, CS_DBLCLKS);
        if (!m_hWnd) return;

        g_filter.OnChanged = [this](const std::wstring&) {
            BuildDisplayList();
            int fx, fy, fw, fh;
            GetHistoryWindowBounds(g_hHistOwner ? g_hHistOwner : m_hWnd, fx, fy, fw, fh);
            SetWindowPos(m_hWnd, HWND_TOPMOST, fx, fy, fw, fh, SWP_FRAMECHANGED);
            InvalidateRect(m_hWnd, nullptr, TRUE);
        };

        SetWindowPos(m_hWnd, nullptr, x, y, 0, 0, SWP_NOZORDER | SWP_NOSIZE);
        ShowWindow(m_hWnd, SW_HIDE);
    }

    // --- Scrolling, driven by FloatingPanelWnd -------------------------------

    UI::ScrollView *HistoryListWnd::ScrollViewAt(POINT) { return &g_view; }

    // One wheel "line" is one history row. Was hard-coded to a single row per
    // notch, which made this the slowest list in the app; the count is the
    // user's Mouse setting now, like everywhere else.
    int HistoryListWnd::ScrollLinePx(const UI::ScrollView &) const {
        const UINT dpi = static_cast<UINT>(app.dpiScale * 96.0f);
        return MulDiv(Constants::History::HISTORY_ROW_HEIGHT, dpi, 96);
    }

    void HistoryListWnd::OnScrolled() {
        HideLinkTip();
        if (m_hWnd) InvalidateRect(m_hWnd, nullptr, FALSE);
    }

    void HistoryListWnd::OnSetFocus() {
        UI::SetActivePanelWindow(m_hWnd);
        // Restore the selection that OnKillFocus saved when focus bounced away
        // (spawned panel open/close, overlay activation, etc.).
        if (g_hoverRow < 0 && g_savedHoverRow >= 0) {
            int navMax = static_cast<int>(g_displayList.size());
            g_hoverRow = (g_savedHoverRow < navMax)
                             ? g_savedHoverRow
                             : (navMax > 0 ? navMax - 1 : -1);
        }
    }

    void HistoryListWnd::OnKillFocus() {
        if (g_hoverRow >= 0) g_savedHoverRow = g_hoverRow;
        g_hoverRow = -1;
        HideLinkTip(); // a TTF_TRACK tip stays up until told otherwise
        // InvalidateRect is already called by FloatingPanelWnd before this hook.
    }

    // Caches the qivHistory.txt size for the footer so WM_PAINT needs no I/O.
    // Re-run on F5 as well as on open: the file grows as folders are visited and
    // is rewritten by clears, so a value captured once at Show() goes stale.
    void HistoryListWnd::RefreshCachedFileSize() {
        std::error_code ec;
        auto bytes = std::filesystem::file_size(historyFoldersManager.GetFilePath(), ec);
        if (ec) {
            m_cachedSizeStr = L"History - n/a";
            return;
        }
        wchar_t buf[64];
        if (bytes >= 1024ULL * 1024)
            swprintf_s(buf, L"History - %.3f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
        else if (bytes >= 1024)
            swprintf_s(buf, L"History - %.3f KB", static_cast<double>(bytes) / 1024.0);
        else
            swprintf_s(buf, L"History - %llu Bytes", static_cast<unsigned long long>(bytes));
        m_cachedSizeStr = buf;
    }

    void HistoryListWnd::Show() {
        if (!m_hWnd) return;
        g_filter.Reset(); // silent — Show() drives layout directly
        // An ordinary open (plain Tab, tray, context menu) always starts from the
        // saved preference, so drop any one-shot "show everything" override left
        // by a previous Ctrl+Tab. The preference itself is NOT reset here — it is
        // app.historyFullModeEnabled and already holds what the user last chose;
        // overwriting it was what made a mode switch vanish on close/reopen.
        // ToggleHistoryFull() bypasses Show(), so its override survives this.
        g_fullModeOverride = false;
        BuildDisplayList();
        int x, y, w, h;
        GetHistoryWindowBounds(g_hHistOwner ? g_hHistOwner : m_hWnd, x, y, w, h);
        SetWindowPos(m_hWnd, HWND_TOPMOST, x, y, w, h, SWP_FRAMECHANGED);
        g_view.scrollY = 0;
        g_savedHoverRow = -1;   // nothing to restore on a fresh open
        HoverCurrentFolderRow(); // land on the folder you are actually in
        // Every open validates, not just F5 — the missing / empty / link markers
        // are read live from the caches, so a first Tab with nothing cached would
        // otherwise paint an unmarked list. Only UNSCANNED folders are visited, so
        // reopening the panel is free; F5 is the "check everything again" path.
        LaunchHistoryValidation(m_hWnd, /*rescanAll=*/false);

        RefreshCachedFileSize();

        ShowWindow(m_hWnd, SW_SHOW);
        SetForegroundWindow(m_hWnd);
        InvalidateRect(m_hWnd, nullptr, TRUE);
    }

    void HistoryListWnd::Toggle() {
        if (!m_hWnd) return;
        IsWindowVisible(m_hWnd) ? Hide() : Show();
    }

    void HistoryListWnd::ToggleHistoryWindow() {
        Toggle();
    }

    void HistoryListWnd::PushFolderHistory(const std::wstring &folderPath,
                                           bool folderHasImages) {
        UI::PushFolderHistory(folderPath, folderHasImages);
    }

    const std::vector<std::wstring> &HistoryListWnd::GetFolderHistory() {
        return UI::GetFolderHistory();
    }
} // namespace UI
