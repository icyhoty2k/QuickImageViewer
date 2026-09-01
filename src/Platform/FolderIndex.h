// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

// =============================================================================
//  FolderIndex — every picture qIV knows about, ready to search
//
//  WHAT IT IS FOR. "Go to name" is one of this viewer's best features and it has
//  always been scoped to the folder you are standing in. This is the same idea
//  across every folder the app already remembers — history and favourites — so
//  typing part of a name finds the picture wherever it lives, without opening
//  that folder first.
//
//  WHY IT CAN BE CHEAP. The folder list is small (history is capped) and only
//  file NAMES are held — no decoding, no thumbnails, no attributes. A folder of
//  10,000 pictures costs about a megabyte of strings, and the walk is one
//  directory_iterator per folder with no recursion: qIV's own idea of a folder
//  has never been recursive, and search results that came from somewhere the
//  viewer would not open are worse than no results.
//
//  ⚠ IT IS A CACHE OF NAMES, NOT A TRUTH. Files appear and vanish behind the
//  viewer's back, so an entry may name something that is gone. Opening one is
//  the same call the playlist uses and fails the same visible way; the index is
//  refreshed rather than trusted to stay right.
//
//  THREADING. Build() runs on a worker; Snapshot() hands back a copy under a
//  lock, so the UI thread searching it can never see a half-built index.
// =============================================================================

namespace Platform::FolderIndex {

    // WHERE THE FILE NAME STARTS INSIDE A PATH.
    //
    // Pulled out as a pure function for two reasons. It is the whole of a
    // decision that was silently wrong - the separator set was written as
    // L"\/" in the .cpp, which is not a valid escape: MSVC folds it to "/"
    // alone, so on a Windows path nothing matched and every offset came back 0.
    // And a three-line computation inside a directory walk is unreachable from
    // a test, which is why nobody noticed for a release.
    //
    // BOTH SEPARATORS, deliberately. A path handed to qIV can carry either -
    // a command line, a drag-drop from a shell that normalises differently, or
    // a hand-edited history line.
    inline int NameOffsetOf(const std::wstring &path) {
        const size_t slash = path.find_last_of(L"\\/");
        return (slash == std::wstring::npos) ? 0 : static_cast<int>(slash + 1);
    }

    // One picture: the full path, and where its file name starts inside it.
    // The offset is kept so a search does not have to find the last separator
    // again for every candidate on every keystroke.
    struct Entry {
        std::wstring path;
        int          nameOffset = 0;

        // Size in bytes, taken from the directory entry during the walk.
        //
        // FREE HERE, EXPENSIVE LATER. The iterator already holds it, so reading
        // it costs nothing extra; asking for it afterwards is a stat() per file.
        // It is what makes duplicate detection cheap - two pictures of different
        // sizes cannot be the same picture, so most of the work never happens.
        //
        // 0 when the size could not be read, which is treated as "unknown" and
        // excluded from duplicate grouping rather than matched against other
        // unknowns.
        unsigned long long size = 0;
    };

    // Walks the folders qIV remembers and replaces the index with what it finds.
    // Safe to call repeatedly; the previous index stays searchable until the new
    // one is complete, so a rebuild never blanks the results under the user.
    //
    // WORKER THREAD ONLY - it touches the filesystem.
    void Build(const std::vector<std::wstring> &folders);

    // A copy of the current index. Empty until the first Build finishes, which
    // is a normal state and not an error - the caller simply has nothing extra
    // to offer yet.
    std::vector<Entry> Snapshot();

    // How many pictures are indexed, without copying anything. For the
    // Statistics panel.
    size_t Count();

    // Drops everything. Called when the history is cleared, so a search cannot
    // go on offering pictures from folders the app has just forgotten.
    void Clear();

} // namespace Platform::FolderIndex
