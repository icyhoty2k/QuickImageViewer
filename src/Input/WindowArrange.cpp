// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#include "WindowArrange.h"
#include "AppCommands.h"
#include "../AppState.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>

extern AppState app;

namespace WindowArrange {

    namespace {
        // The wire form of a placement request. Fixed-width fields and a magic
        // value, because the sender is only USUALLY another copy of this
        // program — anything on the desktop can send WM_COPYDATA.
        struct PlaceRequest {
            uint32_t magic;   // PLACE_MAGIC
            uint32_t version; // PLACE_VERSION
            uint32_t flags;   // PLACE_REMEMBER
            int32_t  cycleStep; // the group's next Ctrl+Alt+Space step
            int32_t  left, top, right, bottom; // screen coordinates
        };
        // Restore every window the cycle moved to where it was before.
        struct RestoreRequest {
            uint32_t magic;
            uint32_t version;
            int32_t  cycleStep;
        };
        constexpr uint32_t PLACE_MAGIC   = 0x50564951; // "QIVP"
        constexpr uint32_t PLACE_VERSION = 2;
        // Remember where the window was before the FIRST such placement, so a
        // later restore can put it back. Set by the Ctrl+Alt+Space cycle only.
        constexpr uint32_t PLACE_REMEMBER = 1u;

        // Where this window was before the cycle first moved it, and where the
        // cycle last put it. A window found anywhere else at restore time was
        // moved by hand since, and is left alone.
        struct Remembered {
            bool valid      = false;
            bool fullscreen = false;
            RECT original{};
            RECT placed{};
        };
        Remembered s_remembered;   // UI thread only, like everything here

        // The cycle position, shared by every window: each request carries the
        // NEXT step, so pressing the key in any window continues the same cycle.
        int s_cycleStep = 0;
        // A lone window's own walk through SINGLE_STEPS. Per window: there is
        // nobody to share it with.
        int s_singleStep = 0;

        // Short, and ABORTIFHUNG: a copy stuck in a long operation is skipped
        // instead of freezing the menu that asked.
        constexpr UINT PLACE_TIMEOUT_MS = 1000;

        // The smallest slot worth placing a window into. Below this the
        // viewer's own chrome does not fit.
        constexpr int MIN_SLOT_PX = 100;

        BOOL CALLBACK CollectInstance(HWND hWnd, LPARAM lParam) {
            auto *out = reinterpret_cast<std::vector<HWND> *>(lParam);
            if (!IsWindowVisible(hWnd)) return TRUE;
            wchar_t cls[256];
            if (GetClassNameW(hWnd, cls, static_cast<int>(std::size(cls))) == 0) return TRUE;
            if (app.windowClassName == cls) out->push_back(hWnd);
            return TRUE;
        }

        // Columns × rows for a layout and window count.
        void GridShape(Layout layout, size_t n, int &cols, int &rows) {
            switch (layout) {
                case Layout::SideBySide: cols = 2; rows = 1; return;
                case Layout::Stacked:    cols = 1; rows = 2; return;
                case Layout::Columns:    cols = 3; rows = 1; return;
                case Layout::Rows:       cols = 1; rows = 3; return;
                case Layout::Corners:    cols = 2; rows = 2; return;
                case Layout::Grid:
                default: {
                    cols = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n))));
                    cols = std::max(cols, 1);
                    const size_t c = static_cast<size_t>(cols);
                    rows = static_cast<int>((n + c - 1) / c);
                    return;
                }
            }
        }

        RECT CellRect(const RECT &wa, int cols, int rows, int col, int row) {
            const int w = wa.right - wa.left;
            const int h = wa.bottom - wa.top;
            // Edges from the shared integer formula, so neighbouring cells meet
            // exactly and the last one absorbs the remainder pixel.
            return RECT{
                wa.left + w * col / cols,
                wa.top  + h * row / rows,
                wa.left + w * (col + 1) / cols,
                wa.top  + h * (row + 1) / rows,
            };
        }

        LONG CenterX(const RECT &r) { return (r.left + r.right) / 2; }
        LONG CenterY(const RECT &r) { return (r.top + r.bottom) / 2; }

        // Orders the windows so each moves as little as possible: by height
        // first, cut into rows of `cols`, then each row left to right. A window
        // already in the top-left lands in the top-left slot.
        void OrderForSlots(std::vector<HWND> &wins, size_t cols) {
            struct Item { HWND h; RECT r; };
            std::vector<Item> items;
            items.reserve(wins.size());
            for (HWND h : wins) {
                RECT r{};
                GetWindowRect(h, &r);
                items.push_back({h, r});
            }
            std::stable_sort(items.begin(), items.end(), [](const Item &a, const Item &b) {
                return CenterY(a.r) < CenterY(b.r);
            });
            for (size_t i = 0; i < items.size(); i += cols) {
                const auto first = items.begin() + static_cast<std::ptrdiff_t>(i);
                const auto last  = items.begin() +
                                   static_cast<std::ptrdiff_t>(std::min(items.size(), i + cols));
                std::stable_sort(first, last, [](const Item &a, const Item &b) {
                    return CenterX(a.r) < CenterX(b.r);
                });
            }
            for (size_t i = 0; i < items.size(); ++i) wins[i] = items[i].h;
        }

        // Sends one WM_COPYDATA request. True when the target confirmed.
        bool SendCopyData(HWND from, HWND target, ULONG_PTR kind, void *data, DWORD size) {
            COPYDATASTRUCT cds{};
            cds.dwData = kind;
            cds.cbData = size;
            cds.lpData = data;
            DWORD_PTR result = 0;
            const LRESULT ok = SendMessageTimeoutW(target, WM_COPYDATA,
                                                   reinterpret_cast<WPARAM>(from),
                                                   reinterpret_cast<LPARAM>(&cds),
                                                   SMTO_NORMAL | SMTO_ABORTIFHUNG,
                                                   PLACE_TIMEOUT_MS, &result);
            return ok != 0 && result == TRUE;
        }

        // Asks another instance to place itself.
        bool SendPlace(HWND from, HWND target, const RECT &r, uint32_t flags, int step) {
            PlaceRequest req{PLACE_MAGIC, PLACE_VERSION, flags, step,
                             r.left, r.top, r.right, r.bottom};
            return SendCopyData(from, target, COPYDATA_PLACE, &req, sizeof(req));
        }

        // Places this window; with `remember`, first records where it was.
        void PlaceSelfImpl(HWND hWnd, const RECT &rect, bool remember);

        // Puts this window back where the cycle found it. True when it moved.
        bool RestoreSelf(HWND hWnd) {
            if (!s_remembered.valid) return false;
            const Remembered r = s_remembered;
            s_remembered = Remembered{};

            // Moved by hand since the cycle placed it: that is where the user
            // wants it now, and restoring would undo their choice.
            RECT now{};
            if (!GetWindowRect(hWnd, &now) || !EqualRect(&now, &r.placed)) return false;
            // The old place must still be on a monitor — screens change.
            if (!MonitorFromRect(&r.original, MONITOR_DEFAULTTONULL)) return false;

            PlaceSelfImpl(hWnd, r.original, false);
            // Back into fullscreen the way the window left it: ToggleFullscreen
            // records the windowed rect it is leaving, which is `original`.
            if (r.fullscreen && !app.isFullscreen) AppCommands::ToggleFullscreen(hWnd);
            return true;
        }

        // The Ctrl+Alt+Space layouts for a window count, in cycle order. The
        // step after the last is "restore".
        std::vector<Layout> CycleLayouts(size_t n) {
            if (n == 2) return {Layout::SideBySide, Layout::Stacked};
            if (n == 3) return {Layout::Columns, Layout::Rows};
            if (n == 4) return {Layout::Corners};
            if (n >= 5) return {Layout::Grid};
            return {};
        }

        Result ArrangeImpl(HWND self, Layout layout, uint32_t flags, int nextStep);
    }

    std::vector<HWND> FindInstances() {
        std::vector<HWND> out;
        EnumWindows(CollectInstance, reinterpret_cast<LPARAM>(&out));
        return out;
    }

    size_t RequiredCount(Layout layout) {
        switch (layout) {
            case Layout::SideBySide:
            case Layout::Stacked:    return 2;
            case Layout::Columns:
            case Layout::Rows:       return 3;
            case Layout::Corners:    return 4;
            case Layout::Grid:
            default:                 return 0;
        }
    }

    bool IsAvailable(Layout layout, size_t count) {
        const size_t need = RequiredCount(layout);
        return need == 0 ? count >= 2 : count == need;
    }

    Result Arrange(HWND self, Layout layout) {
        // A menu pick is a one-off: it neither remembers positions nor moves
        // the Ctrl+Alt+Space cycle, which carries on from wherever it was.
        return ArrangeImpl(self, layout, 0u, s_cycleStep);
    }

    const wchar_t *LayoutName(Layout layout) {
        switch (layout) {
            case Layout::SideBySide: return L"Side by Side";
            case Layout::Stacked:    return L"Top and Bottom";
            case Layout::Columns:    return L"Three Columns";
            case Layout::Rows:       return L"Three Rows";
            case Layout::Corners:    return L"Four Corners";
            case Layout::Grid:
            default:                 return L"Grid";
        }
    }

    CycleResult Cycle(HWND self) {
        CycleResult res;
        const std::vector<HWND> wins = FindInstances();
        res.found = wins.size();

        // A lone window walks its own positions; the group cycle (and any
        // restore it still owes) is a multi-window thing.
        if (wins.size() <= 1) {
            res.kind = CycleResult::Kind::Single;
            res.singleStep = s_singleStep;
            s_singleStep = (s_singleStep + 1) % SINGLE_STEPS;
            return res;
        }

        const std::vector<Layout> layouts = CycleLayouts(wins.size());

        // The count can change between presses; a step past this count's last
        // layout means "restore" — exactly what the cycle owes at that point.
        int step = s_cycleStep;
        if (step < 0 || static_cast<size_t>(step) > layouts.size()) step = 0;

        if (static_cast<size_t>(step) < layouts.size()) {
            res.kind = CycleResult::Kind::Arranged;
            res.layout = layouts[static_cast<size_t>(step)];
            const Result r = ArrangeImpl(self, res.layout, PLACE_REMEMBER, step + 1);
            res.placed = r.placed;
            return res;
        }

        // Restore: every window back to before the cycle began. A window that
        // was never moved by it, or was moved by hand since, stays put.
        res.kind = CycleResult::Kind::Restored;
        s_cycleStep = 0;
        RestoreRequest req{PLACE_MAGIC, PLACE_VERSION, 0};
        for (HWND h : wins) {
            if (h == self) {
                if (RestoreSelf(self)) ++res.placed;
            } else if (SendCopyData(self, h, COPYDATA_RESTORE, &req, sizeof(req))) {
                ++res.placed;
            }
        }
        return res;
    }

    namespace {
    Result ArrangeImpl(HWND self, Layout layout, uint32_t flags, int nextStep) {
        Result res;
        std::vector<HWND> wins = FindInstances();
        res.found = wins.size();
        if (!IsAvailable(layout, wins.size())) return res;

        MONITORINFO mi = {sizeof(mi)};
        if (!GetMonitorInfoW(MonitorFromWindow(self, MONITOR_DEFAULTTONEAREST), &mi)) return res;
        const RECT &wa = mi.rcWork;

        int cols = 1, rows = 1;
        GridShape(layout, wins.size(), cols, rows);
        if ((wa.right - wa.left) / cols < MIN_SLOT_PX ||
            (wa.bottom - wa.top) / rows < MIN_SLOT_PX)
            return res;

        const size_t ucols = static_cast<size_t>(cols);
        OrderForSlots(wins, ucols);
        s_cycleStep = nextStep;
        const bool remember = (flags & PLACE_REMEMBER) != 0;
        for (size_t i = 0; i < wins.size(); ++i) {
            const RECT cell = CellRect(wa, cols, rows,
                                       static_cast<int>(i % ucols), static_cast<int>(i / ucols));
            if (wins[i] == self) {
                PlaceSelfImpl(self, cell, remember);
                ++res.placed;
            } else if (SendPlace(self, wins[i], cell, flags, nextStep)) {
                ++res.placed;
            }
        }
        return res;
    }

    void PlaceSelfImpl(HWND hWnd, const RECT &rect, bool remember) {
        // The FIRST remembered placement records the way back; later steps of
        // the same cycle keep it, so restore returns to before the cycle began.
        if (remember && !s_remembered.valid) {
            s_remembered.valid = true;
            s_remembered.fullscreen = app.isFullscreen;
            if (app.isFullscreen)
                s_remembered.original = app.savedWindowRect; // the windowed rect
            else
                GetWindowRect(hWnd, &s_remembered.original);
        }
        PlaceSelf(hWnd, rect);
        if (remember && s_remembered.valid) GetWindowRect(hWnd, &s_remembered.placed);
    }
    } // namespace

    void PlaceSelf(HWND hWnd, const RECT &rect) {
        // Fullscreen first: the window is borderless and topmost there, and a
        // plain resize would leave app.isFullscreen claiming otherwise.
        if (app.isFullscreen) AppCommands::ToggleFullscreen(hWnd);
        if (IsIconic(hWnd) || IsZoomed(hWnd)) ShowWindow(hWnd, SW_RESTORE);

        const int w = rect.right - rect.left;
        const int h = rect.bottom - rect.top;
        constexpr UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED;
        SetWindowPos(hWnd, nullptr, rect.left, rect.top, w, h, flags);

        // A move onto a monitor of different DPI raises WM_DPICHANGED, whose
        // handler applies Windows' suggested rect over ours. The window is on
        // the new DPI now, so a second placement sticks.
        RECT now{};
        if (GetWindowRect(hWnd, &now) && !EqualRect(&now, &rect))
            SetWindowPos(hWnd, nullptr, rect.left, rect.top, w, h, flags);

        app.isAutosized = false; // it no longer fills the area it was fitted to
        InvalidateRect(hWnd, nullptr, FALSE);
    }

    BOOL HandlePlaceRequest(HWND hWnd, const COPYDATASTRUCT &cds) {
        if (cds.dwData != COPYDATA_PLACE) return FALSE;
        if (!cds.lpData || cds.cbData != sizeof(PlaceRequest)) return FALSE;
        // A kiosk-locked viewer accepts no input; being moved by another
        // window is input too.
        if (app.isLocked) return FALSE;

        PlaceRequest req;
        memcpy(&req, cds.lpData, sizeof(req));
        if (req.magic != PLACE_MAGIC || req.version != PLACE_VERSION) return FALSE;

        const RECT r{req.left, req.top, req.right, req.bottom};
        if (r.right - r.left < MIN_SLOT_PX || r.bottom - r.top < MIN_SLOT_PX) return FALSE;
        // Only somewhere a person can see it — a rect off every monitor would
        // put the window where nothing can reach it.
        if (!MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) return FALSE;

        s_cycleStep = req.cycleStep;
        PlaceSelfImpl(hWnd, r, (req.flags & PLACE_REMEMBER) != 0);
        return TRUE;
    }

    BOOL HandleRestoreRequest(HWND hWnd, const COPYDATASTRUCT &cds) {
        if (cds.dwData != COPYDATA_RESTORE) return FALSE;
        if (!cds.lpData || cds.cbData != sizeof(RestoreRequest)) return FALSE;
        if (app.isLocked) return FALSE;

        RestoreRequest req;
        memcpy(&req, cds.lpData, sizeof(req));
        if (req.magic != PLACE_MAGIC || req.version != PLACE_VERSION) return FALSE;

        s_cycleStep = req.cycleStep;
        return RestoreSelf(hWnd) ? TRUE : FALSE;
    }
}
