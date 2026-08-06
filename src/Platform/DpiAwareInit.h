// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#pragma once
#include <windows.h>
#include <string>

HWND CreateViewerWindow(HINSTANCE hInstance, const wchar_t* className);

// =============================================================================
// IsUsableWindowRect — would this placement leave the user a window they can
// actually reach?
//
// The remembered placement is the one piece of window geometry that comes from
// OUTSIDE the running program: a file the user is invited to open and edit, or
// a registry value, written by a possibly older build on a possibly different
// display arrangement. Everything it says has to be treated as a claim rather
// than a fact.
//
// A rect fails when it is:
//   * not a size the app accepts at all  (WINDOW_SIZE_MIN..WINDOW_SIZE_MAX) —
//     a two-pixel window cannot be grabbed to fix itself, and a 16000-pixel one
//     puts every control off screen
//   * larger than the whole virtual desktop
//   * on no monitor, or leaving less than WINDOW_MIN_VISIBLE_W x _H inside a
//     monitor's WORK AREA — work area, not monitor bounds, so a window hidden
//     behind the taskbar counts as unreachable
//
// Used on the way IN and on the way OUT: a placement that fails is neither
// restored nor saved, so one bad exit cannot poison every later launch.
// =============================================================================
bool IsUsableWindowRect(int x, int y, int width, int height);

// Is the display device named by `deviceName` (e.g. "\\.\DISPLAY2") still
// attached, and if so where is its work area NOW? Returns false when that
// screen is gone, which is the caller's cue to fall back to its default
// placement rather than guess.
//
// Looked up by enumeration every time. An HMONITOR does not survive the process
// that obtained it, and does not reliably survive a display change within one.
bool FindMonitorWorkArea(const std::wstring &deviceName, RECT &outWork);
