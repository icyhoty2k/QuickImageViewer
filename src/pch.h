// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#pragma once
//
// Precompiled header. Force-included into every C++ TU by MSVC (/FI), so no
// source file needs to #include it — do not add it by hand anywhere.
//
// WHAT BELONGS HERE: system headers that are (a) expensive to parse and (b)
// never change. windows.h alone was being parsed 65 times per full build.
//
// WHAT DOES NOT BELONG HERE: project headers, and above all AppState.h /
// Platform/Constants.h. Those are edited constantly; putting them in the PCH
// would invalidate the PCH on every edit, forcing a full rebuild AND a PCH
// regeneration — strictly worse than today.
//
// ORDER IS LOAD-BEARING: winsock2.h must be seen before windows.h, otherwise
// windows.h drags in the original winsock.h and every socket symbol in
// src/Rem_TCP_IP/ collides. The three files that already include winsock2.h
// themselves (RemoteServer/RemoteClient/RemoteMirror.cpp) keep their includes —
// they are no-ops behind the include guards, and they document the constraint
// at the point where it matters.
//
// NOMINMAX / UNICODE / _UNICODE come from target_compile_definitions in
// CMakeLists.txt, so they are already in effect when windows.h is parsed here.

// --- Sockets first. See note above. ---
#include <winsock2.h>
#include <ws2tcpip.h>

// --- Core Win32 ---
#include <windows.h>
#include <windowsx.h>

// --- Shell / COM ---
#include <objbase.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <commctrl.h>
#include <commdlg.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <wrl/client.h>

// --- Graphics / imaging. Highest version pulls in the lower ones. ---
#include <d2d1_3.h>
#include <dwrite_3.h>
#include <wincodec.h>

// --- STL ---
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <numeric>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
