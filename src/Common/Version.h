// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#pragma once
//
// Version.h — every piece of version / product identity in one place.
//
// *** To bump the version, edit the three numbers below. Nothing else. ***
//
// The fourth component, VER_BUILD, is NOT here: it is auto-incremented on every
// build into a generated BuildNumber.h (see IncreaseQIVBuildNumberOnBuild.cmake).
//
// ---------------------------------------------------------------------------
// WHY THIS HEADER DOES NOT INCLUDE BuildNumber.h
//
// Constants.h includes this file for FILE_DESC / COPYRIGHT, and Constants.h is
// included by nearly every translation unit. If the ever-changing build number
// were reachable from here, every build would invalidate all ~47 objects — a
// full rebuild, permanently.
//
// So the two macros that need it, VER_NUMERIC and VER_STR, are defined here but
// left to expand later. A macro is only evaluated where it is USED, so a TU that
// never mentions them never needs VER_BUILD.
//
// THE TWO PLACES THAT USE THEM MUST INCLUDE BuildNumber.h FIRST:
//     resources/resource.rc
//     src/Common/Version.cpp
// Anywhere else, using VER_STR is a compile error naming VER_BUILD — which is
// the intended outcome, not a trap to work around.
// ---------------------------------------------------------------------------

// =========================================================================
// RC COMPATIBLE DEFINITIONS
// Used by the Resource Compiler for version metadata.
// =========================================================================
#define VER_MAJOR 2
#define VER_MINOR 180
#define VER_PATCH 0

#define FILE_DESC     "qIV"
#define ORIG_FILENAME "QuickImageViewer.exe"
#define PROD_NAME     "Quick Image Viewer"
#define COPYRIGHT     "Copyright \xA9 2026 All rights reserved, Ivan Hristov Yanev"

// ---- Require BuildNumber.h before use — see the note above ----------------

// Comma form — FILEVERSION / PRODUCTVERSION in .rc  (e.g. 2,80,0,123)
#define VER_NUMERIC VER_MAJOR, VER_MINOR, VER_PATCH, VER_BUILD

// String form — derived via C preprocessor stringification.
// rc.exe (Windows SDK 10) runs a full C-preprocessor pass, so # works here.
//   In C++: L"" VER_STR  →  L"2.80.0.123"
//   In RC:  VALUE "FileVersion", VER_STR  →  "2.80.0.123"
#define _QIV_S(x)   #x
#define _QIV_STR(x) _QIV_S(x)
#define VER_STR \
    _QIV_STR(VER_MAJOR) "." _QIV_STR(VER_MINOR) "." _QIV_STR(VER_PATCH) "." _QIV_STR(VER_BUILD)
