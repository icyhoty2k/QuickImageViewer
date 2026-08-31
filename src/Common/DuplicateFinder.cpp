// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DuplicateFinder.h"

#include <algorithm>
#include <unordered_map>

namespace Common::DuplicateFinder {

    std::vector<size_t> NeedHashing(const std::vector<Candidate> &candidates) {
        // Count per size first, then collect. One pass to learn which sizes are
        // shared, one to gather - rather than an inner loop per candidate, which
        // is quadratic on the folder sizes this actually runs against.
        std::unordered_map<unsigned long long, int> perSize;
        for (const Candidate &c : candidates) {
            if (c.size == 0) continue; // unknown - see the header
            ++perSize[c.size];
        }

        std::vector<size_t> out;
        for (size_t i = 0; i < candidates.size(); ++i) {
            const Candidate &c = candidates[i];
            if (c.size == 0) continue;
            const auto it = perSize.find(c.size);
            if (it != perSize.end() && it->second > 1) out.push_back(i);
        }
        return out;
    }

    std::vector<Group> FindGroups(const std::vector<Candidate> &candidates) {
        // Keyed by size AND digest. Digest alone would be enough for a good
        // hash, but size is already known and free to compare, and a collision
        // that survives both is far less likely than one that survives one.
        // Cheap insurance on the operation where being wrong is worst.
        std::unordered_map<unsigned long long,
                           std::unordered_map<std::uint64_t, std::vector<std::wstring>>> buckets;

        for (const Candidate &c : candidates) {
            if (c.size == 0 || !c.hashed) continue; // unreadable proves nothing
            buckets[c.size][c.digest].push_back(c.path);
        }

        std::vector<Group> out;
        for (auto &[size, byDigest] : buckets) {
            for (auto &[digest, paths] : byDigest) {
                if (paths.size() < 2) continue; // one copy is not a duplicate
                Group g;
                g.size  = size;
                g.paths = std::move(paths);
                out.push_back(std::move(g));
            }
        }

        // Biggest waste first: the group of six 40 MB scans matters more than
        // two 4 KB icons, and that is the order somebody reclaiming space wants
        // to read. Ties break on size, then on the first path, so the order is
        // STABLE - an unordered_map's iteration order is not, and a list that
        // reshuffled between runs would be unusable.
        std::sort(out.begin(), out.end(), [](const Group &a, const Group &b) {
            if (a.paths.size() != b.paths.size()) return a.paths.size() > b.paths.size();
            if (a.size != b.size)                 return a.size > b.size;
            return a.paths.front() < b.paths.front();
        });
        return out;
    }

} // namespace Common::DuplicateFinder
