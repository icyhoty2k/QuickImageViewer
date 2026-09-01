// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// =============================================================================
//  DuplicateFinder — which of these pictures are the same picture
//
//  PURE, AND THAT IS THE POINT. Deciding what counts as a duplicate is the half
//  worth testing, and it does not need the filesystem: the caller supplies size
//  and content digests, this decides the grouping. The reading lives in the
//  caller (Platform/DuplicateScan), the rule lives here, and the suite can
//  exercise the rule without a disk.
//
//  Same split, and the same reason, as HistoryPath and Remote::AuthPolicy.
//
//  THE RULE, AND WHY IT IS IN THIS ORDER.
//
//  1. GROUP BY SIZE. Two files of different byte counts cannot be the same
//     bytes. Size comes free from the directory walk, so this eliminates almost
//     everything before anything is read. A folder of ten thousand holiday
//     photographs usually leaves a few dozen candidates.
//
//  2. WITHIN A SIZE GROUP, COMPARE DIGESTS. Only files that survived step 1 are
//     ever read. A group of one is not a group and is dropped without reading
//     anything at all.
//
//  ⚠ SAME BYTES, NOT SAME PICTURE. This finds byte-identical files. The same
//  photograph saved twice at different quality, or rotated, or re-encoded, is a
//  different file and is deliberately NOT reported: claiming those are
//  duplicates would invite somebody to delete the better copy. A viewer that is
//  wrong about this costs people their photographs.
//
//  ⚠ AND IT ONLY EVER REPORTS. Nothing here deletes, moves or rewrites. What to
//  do about a duplicate is the user's decision, taken in the shell where it can
//  be undone.
// =============================================================================

namespace Common::DuplicateFinder {

    // One file, as much as the rule needs to know about it.
    struct Candidate {
        std::wstring       path;
        unsigned long long size   = 0;  // 0 = unknown, never grouped
        std::uint64_t      digest = 0;  // content hash; only meaningful within a size group
        bool               hashed = false; // false = digest not computed, so not comparable
    };

    // Files that are byte-identical to each other. Two or more paths, always -
    // a "group" of one is not reported.
    struct Group {
        unsigned long long        size = 0;
        std::vector<std::wstring> paths;
    };

    // The sizes worth reading, given everything known so far.
    //
    // Returns the indices of candidates that share their size with at least one
    // other candidate. Anything else cannot have a duplicate and must never be
    // opened - that is where the speed comes from.
    //
    // A size of 0 is "unknown" and is excluded: grouping unknowns together would
    // read every file whose size could not be read, to compare them against each
    // other for no reason.
    std::vector<size_t> NeedHashing(const std::vector<Candidate> &candidates);

    // The duplicate groups, from candidates whose digests have been filled in.
    //
    // Candidates with hashed == false are ignored rather than assumed equal: a
    // file that could not be read is not evidence of anything, and treating it
    // as matching would be the one failure that costs somebody a photograph.
    //
    // Groups come back largest first, and paths within a group in the order they
    // were given, so the caller's own ordering decides which copy reads as the
    // original.
    std::vector<Group> FindGroups(const std::vector<Candidate> &candidates);

    // Splits one group into sets that are ACTUALLY identical, using a caller
    // supplied byte comparison.
    //
    // WHY THIS EXISTS AT ALL. Same size plus same 64-bit digest is enormously
    // strong evidence, and it is still evidence rather than proof: a hash maps
    // many inputs onto one value, so two different pictures CAN collide. The
    // odds are astronomical and the consequence is somebody deleting a
    // photograph they wanted, which is exactly the trade where astronomical is
    // not good enough.
    //
    // (A CRC32 would be far weaker, not stronger: 32 bits collide by accident
    // within tens of thousands of files, and it is an error-detection code, not
    // a fingerprint. The answer to "be certain" is not a different hash - it is
    // to stop hashing and compare the bytes.)
    //
    // It is affordable BECAUSE it runs last. Size eliminated almost everything,
    // the digest eliminated the rest, and what reaches here is a handful of
    // files that really are expected to match - so the comparison confirms, and
    // reads each file at most once more.
    //
    // `equal(a, b)` returns true when the two files hold identical bytes. It may
    // return false for a file it could not read, and that is the safe direction:
    // an unreadable file ends up in a set of its own and is never reported as a
    // duplicate of anything.
    //
    // Returns one vector per distinct content. Sets of a single path are kept -
    // the caller decides that a set of one is not a duplicate, the same rule
    // FindGroups already applies.
    std::vector<std::vector<std::wstring>> Partition(
        const std::vector<std::wstring> &paths,
        const std::function<bool(const std::wstring &, const std::wstring &)> &equal);

} // namespace Common::DuplicateFinder
