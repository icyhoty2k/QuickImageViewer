// SPDX-License-Identifier: AGPL-3.0-or-later
#include "FolderIndex.h"

#include "Constants.h"
#include "FileHandler.h" // is_image_ext - ONE definition of "an image"

#include <filesystem>
#include <mutex>

namespace fs = std::filesystem;

namespace Platform::FolderIndex {

    namespace {
        std::vector<Entry> g_entries;
        std::mutex         g_mutex;
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

            for (const fs::directory_entry &de : it) {
                if (built.size() >= Constants::FOLDER_INDEX_MAX_FILES) break;

                std::error_code fe;
                if (!de.is_regular_file(fe) || fe) continue;

                const fs::path &p = de.path();
                if (!is_image_ext(p.extension().wstring())) continue;

                Entry e;

                // From the directory entry, not a separate stat: the walk
                // already has it. A failure leaves 0, which means "unknown".
                std::error_code se;
                const auto sz = de.file_size(se);
                if (!se) e.size = static_cast<unsigned long long>(sz);

                e.path = p.wstring();
                const size_t slash = e.path.find_last_of(L"\/");
                e.nameOffset = (slash == std::wstring::npos)
                                   ? 0
                                   : static_cast<int>(slash + 1);
                built.push_back(std::move(e));
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
