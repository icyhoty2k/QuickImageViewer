// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

// The ONLY translation unit that sees the auto-incremented build number.
//
// Constants::APP_VERSION is declared extern in Constants.h and defined here, so
// bumping the build number recompiles this one file instead of every TU that
// includes Constants.h. See the note at the top of Version.h.
//
// BuildNumber.h MUST come first: Version.h defines VER_STR in terms of
// VER_BUILD without providing it.

#include "BuildNumber.h" // generated into the build tree, rewritten every build
#include "Common/Version.h"

namespace Constants {
    // major.minor.patch.build — e.g. 2.80.0.123
    //
    // `extern` is REQUIRED on the definition, not decoration: at namespace scope a
    // const object has INTERNAL linkage by default, so without it this compiles
    // cleanly and emits no symbol, and the link fails with an unresolved external
    // that Constants.h plainly declares.
    //
    // Constants.h is deliberately not included here — this TU recompiles on every
    // build, so it stays as small as possible. The linker is what verifies the two
    // declarations agree.
    extern const wchar_t *const APP_VERSION = L"" VER_STR;
}
