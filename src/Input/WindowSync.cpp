// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#include "WindowSync.h"
#include "WindowArrange.h"
#include "../AppState.h"
#include "../Overlays/OverlayManager.h"
#include "../Platform/ConstantsStrings.h"
#include <iterator>
#include <string>
#include <vector>

extern AppState app;

namespace WindowSync {

    namespace {
        // Wire codes. FIXED: a running copy of an older or newer build reads
        // these, so values are never renumbered or reused — add new ones only.
        enum SyncAction : WPARAM {
            ACT_SET_OFF    = 1,
            ACT_SET_ON     = 2,
            ACT_NEXT       = 10,
            ACT_PREV       = 11,
            ACT_FIRST      = 12,
            ACT_LAST       = 13,
            ACT_ZOOM_IN    = 20,
            ACT_ZOOM_OUT   = 21,
            ACT_ZOOM_RESET = 22,
            ACT_PAN_LEFT   = 30,
            ACT_PAN_RIGHT  = 31,
            ACT_PAN_UP     = 32,
            ACT_PAN_DOWN   = 33,
            ACT_ROTATE_CW  = 40,
            ACT_ROTATE_CCW = 41,
            ACT_FLIP_H     = 42,
            ACT_FLIP_V     = 43,
        };

        struct Mapping { Command cmd; WPARAM action; };

        // The ONE list of synced actions, both directions.
        constexpr Mapping MAP[] = {
            {Command::NextImage,     ACT_NEXT},
            {Command::PrevImage,     ACT_PREV},
            {Command::GoToFirstImage,ACT_FIRST},
            {Command::GoToLastImage, ACT_LAST},
            {Command::ZoomIn,        ACT_ZOOM_IN},
            {Command::ZoomOut,       ACT_ZOOM_OUT},
            {Command::ZoomReset,     ACT_ZOOM_RESET},
            {Command::PanLeft,       ACT_PAN_LEFT},
            {Command::PanRight,      ACT_PAN_RIGHT},
            {Command::PanUp,         ACT_PAN_UP},
            {Command::PanDown,       ACT_PAN_DOWN},
            {Command::RotateCW,      ACT_ROTATE_CW},
            {Command::RotateCCW,     ACT_ROTATE_CCW},
            {Command::FlipH,         ACT_FLIP_H},
            {Command::FlipV,         ACT_FLIP_V},
        };

        bool ActionFor(Command cmd, WPARAM &out) {
            for (const Mapping &m : MAP)
                if (m.cmd == cmd) { out = m.action; return true; }
            return false;
        }

        bool CommandFor(WPARAM action, Command &out) {
            for (const Mapping &m : MAP)
                if (m.action == action) { out = m.cmd; return true; }
            return false;
        }

        // UI thread only — the window procedure and ExecuteCommand run there.
        bool s_inbound = false;

        void PostToPeers(HWND self, WPARAM action) {
            const UINT msg = Message();
            if (msg == 0) return;
            for (HWND h : WindowArrange::FindInstances())
                if (h != self) PostMessageW(h, msg, action, 0);
        }

        void Announce(HWND hWnd) {
            std::wstring text = app.syncInstances ? Constants::Messages::SYNC_INSTANCES_ON
                                                  : Constants::Messages::SYNC_INSTANCES_OFF;
            if (app.syncInstances) {
                const size_t n = WindowArrange::FindInstances().size();
                text += L" (" + std::to_wstring(n) + (n == 1 ? L" window)" : L" windows)");
            }
            g_overlayManager.PostCenterMessage(hWnd, text);
            g_overlayManager.RefreshSyncIndicator(hWnd); // the gold SYNC marker
        }
    }

    UINT Message() {
        static const UINT msg = RegisterWindowMessageW(L"QuickImageViewer.SyncInstances.v1");
        return msg;
    }

    bool InboundActive() { return s_inbound; }

    void Broadcast(HWND self, Command cmd) {
        if (!app.syncInstances || s_inbound) return;
        WPARAM action = 0;
        if (!ActionFor(cmd, action)) return;
        PostToPeers(self, action);
    }

    void SetEnabled(HWND self, bool on) {
        app.syncInstances = on;
        PostToPeers(self, on ? ACT_SET_ON : ACT_SET_OFF);
        Announce(self);
    }

    void HandleMessage(HWND hWnd, WPARAM wParam) {
        // A kiosk-locked viewer takes no input, from a peer or anyone else.
        if (app.isLocked) return;

        if (wParam == ACT_SET_ON || wParam == ACT_SET_OFF) {
            const bool on = (wParam == ACT_SET_ON);
            if (app.syncInstances == on) return; // already agrees — nothing to say
            app.syncInstances = on;
            Announce(hWnd);
            return;
        }

        if (!app.syncInstances) return;
        Command cmd = Command::None;
        if (!CommandFor(wParam, cmd)) return; // unknown code: a newer build's action

        s_inbound = true;
        InputManager::ExecuteCommand(hWnd, cmd);
        s_inbound = false;
    }
}
