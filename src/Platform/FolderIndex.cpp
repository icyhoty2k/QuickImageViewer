// SPDX-License-Identifier: AGPL-3.0-or-later
#include "FolderIndex.h"

#include "Constants.h"
#include "FileHandler.h" // is_image_ext - ONE definition of "an image"

#include <chrono>
#include <filesystem>
#include <mutex>

namespace fs = std::filesystem;

namespace Platform::FolderIndex {

    namespace {
        std::vector<Entry> g_entries;
        std::mutex         g_mutex;
    }

    // A file time as days since 1970-01-01.
    //
    // ⚠ NOT A CLOCK CALL. file_clock and system_clock have different epochs, so
    // subtracting file_clock::now() would make "how old is this" depend on when
    // the index was built. clock_cast converts the value itself, which is what
    // makes a stored day mean the same thing tomorrow.
    static long long FileTimeToDay(fs::file_time_type t) {
        const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(t);
        return std::chrono::duration_cast<std::chrono::hours>(
                   sys.time_since_epoch()).count() / 24;
    }

    void Build(const std::vector<std::wstring> &folders) {
        // Built into a LOCAL vector and swapped in at the end. The lock is held
        // only for the swap, so a search on the UI thread never waits for the
        // filesystem - and never sees a half-built list.
        std::vector<Entry> built;

        for (const std::wstring &folder : folders) {
            if (built.size() >= Constants::FOLDER_INDEX_MAX_FILES) break;
            if (folder.empty()) continue;

            std::error_code ec;
            // NON-recursive, deliberately. qIV's idea of a folder has never
            // included its subfolders, and a result the viewer would refuse to
            // open is worse than no result.
            //
            // skip_permission_denied because a remembered folder may now be on a
            // drive this user cannot read; that is a reason to skip it, not to
            // abandon the whole index.
            fs::directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec);
            if (ec) continue;

            // ⚠ INCREMENTED BY HAND, with an error_code.
            //
            // A range-for over a directory_iterator uses the THROWING operator++,
            // and the error_code above only covered construction. A folder that
            // disappears mid-walk - a network share dropping, a USB stick pulled,
            // a directory deleted by something else - therefore threw out of
            // Build().
            //
            // That failed SILENTLY and in the worst possible way: the thread pool
            // catches and discards whatever a task throws, so the exception went
            // nowhere, `built` was abandoned before the swap, and the index kept
            // its PREVIOUS contents. Cross-folder Find and Ctrl+D then went on
            // answering from a stale index with nothing anywhere saying so.
            //
            // Stopping this folder and keeping what was collected is the right
            // answer: the other folders still index, and a partial index is
            // honest where a stale one is not.
            for (auto de = it; de != fs::directory_iterator(); ) {
                if (built.size() >= Constants::FOLDER_INDEX_MAX_FILES) break;

                // ⚠ EVERY SKIP MUST STILL ADVANCE. With a hand-rolled loop a
                // bare `continue` is an infinite loop on the first non-image
                // file, so the two skips below advance first and then continue.
                std::error_code fe;
                const bool regular = de->is_regular_file(fe) && !fe;
                const fs::path p = de->path();
                const bool wanted = regular && is_image_ext(p.extension().wstring());
                if (!wanted) {
                    std::error_code se2;
                    de.increment(se2);
                    if (se2) break;
                    continue;
                }

                Entry e;

                // From the directory entry, not a separate stat: the walk
                // already has it. A failure leaves 0, which means "unknown".
                std::error_code se;
                const auto sz = de->file_size(se);
                if (!se) e.size = static_cast<unsigned long long>(sz);

                // The timestamp comes off the same entry, for the same reason,
                // and is reduced to a day here so the search loop never has to.
                std::error_code te;
                const auto tm = de->last_write_time(te);
                if (!te) e.day = FileTimeToDay(tm);

                e.path = p.wstring();
                e.nameOffset = NameOffsetOf(e.path);
                built.push_back(std::move(e));

                std::error_code ie;
                de.increment(ie);
                if (ie) break; // this folder became unreadable; keep the rest
            }
        }

        std::lock_guard<std::mutex> lk(g_mutex);
        g_entries.swap(built);
    }

    std::vector<Entry> Snapshot() {
        std::lock_guard<std::mutex> lk(g_mutex);
        return g_entries;
    }

    size_t Count() {
        std::lock_guard<std::mutex> lk(g_mutex);
        return g_entries.size();
    }

    void Clear() {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_entries.clear();
        g_entries.shrink_to_fit();
    }

} // namespace Platform::FolderIndex
