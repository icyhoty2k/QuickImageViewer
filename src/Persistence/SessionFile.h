// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#pragma once
#include <string>

// =============================================================================
// SessionFile — what this copy was doing, as opposed to how it is configured.
//
// One value so far: the image on screen at the last exit, reopened on the next
// launch so qIV resumes where it was left instead of prompting.
//
// WHY IT IS NOT A SETTING. It changes at every close, it means nothing on
// another machine, and it is not something anyone would export, import or
// restore to defaults. Held in the settings store it was also expensive out of
// all proportion to its worth: an .ini write rewrites the WHOLE file, so one
// line of session state made every close rewrite every setting the application
// has. Its own tiny file costs one small write instead.
//
// Nothing here is worth preserving. Deleting qivSession.ini simply means the
// next launch opens from history instead.
//
// Always a file, in registry mode as well — resume position is per-copy state
// like qivHistory.txt and qivFavorites.txt beside it, not a user preference,
// and routing it through the settings store is what tied it to the settings
// file in the first place.
// =============================================================================

namespace Persistence::Session {

    // Full path of the image to reopen, or empty when there is none. The caller
    // still has to check the file exists — a remembered path may name a file on
    // a drive that is no longer attached.
    std::wstring LoadLastImage();

    // Records the image on screen. An empty value clears the key rather than
    // writing a blank, so a cleared session leaves nothing behind to puzzle over.
    void SaveLastImage(const std::wstring &fullPath);

} // namespace Persistence::Session
