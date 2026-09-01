// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DuplicateScan.h"

#include "Common/DuplicateFinder.h"
#include "Common/Utf8.h"
#include "Constants.h"
#include "FolderIndex.h"
#include "IniFile.h"      // PathBesideExe - the report sits with the other qIV files
#include "WorkerThread.h"

#include <atomic>
#include <cstdio>
#include <cstring> // memcmp - the byte-for-byte confirmation
#include <fstream>
#include <memory>  // unique_ptr - the result frees itself if the post fails
#include <vector>

extern IoThreadPool g_ioWorker;

namespace Platform::DuplicateScan {

    namespace {
        std::atomic<bool> g_running{false};

        // FNV-1a over the whole file.
        //
        // WHOLE FILE, NOT A SAMPLE. Hashing only the head and tail would be
        // faster and would be wrong in the one case that matters: two large
        // pictures that share a header and a trailer differ somewhere in the
        // middle, and reporting them as identical is how somebody deletes the
        // one they wanted. Only files that already share a size are read at all,
        // so the total read is small - paying full price on a short list beats
        // guessing on a long one.
        //
        // Not a cryptographic hash: nothing here defends against a crafted
        // collision, only against accident, and 64 bits of FNV over identical
        // sizes is far past the point where accident matters.
        bool HashFile(const std::wstring &path, std::uint64_t &out) {
            std::ifstream f(path, std::ios::in | std::ios::binary);
            if (!f.is_open()) return false;

            std::uint64_t h = 1469598103934665603ULL; // FNV offset basis
            char buf[64 * 1024];
            while (f.read(buf, sizeof buf) || f.gcount() > 0) {
                const std::streamsize n = f.gcount();
                for (std::streamsize i = 0; i < n; ++i) {
                    h ^= static_cast<unsigned char>(buf[i]);
                    h *= 1099511628211ULL;            // FNV prime
                }
            }
            out = h;
            return true;
        }

        // Byte-for-byte. The last word on whether two files are the same file.
        //
        // Reads both in lockstep and stops at the first difference, so a pair
        // that is NOT identical usually costs a single block rather than two
        // whole files. Only pairs that already agree on size and on a 64-bit
        // digest ever get here, so in practice it confirms rather than refutes.
        //
        // Any read failure answers FALSE - not "assume equal". An unreadable
        // file then lands in a set of its own and is never reported as somebody
        // else's duplicate, which is the only safe direction for a tool whose
        // output people delete things from.
        bool SameBytes(const std::wstring &a, const std::wstring &b) {
            if (a == b) return true; // the same file, listed once

            std::ifstream fa(a, std::ios::in | std::ios::binary);
            std::ifstream fb(b, std::ios::in | std::ios::binary);
            if (!fa.is_open() || !fb.is_open()) return false;

            char bufA[64 * 1024];
            char bufB[64 * 1024];
            for (;;) {
                fa.read(bufA, sizeof bufA);
                fb.read(bufB, sizeof bufB);
                const std::streamsize na = fa.gcount();
                const std::streamsize nb = fb.gcount();
                if (na != nb) return false;                 // different lengths
                if (na == 0) return fa.eof() && fb.eof();   // both ended together
                if (std::memcmp(bufA, bufB, static_cast<size_t>(na)) != 0) return false;
            }
        }

        std::wstring ReportPath() {
            return Persistence::Ini::PathBesideExe(Constants::History::DUPLICATES_REPORT_NAME);
        }

        // The listing, as UTF-8 with CRLF - the same shape as every other text
        // file this program writes, and readable in Notepad without ceremony.
        bool WriteReport(const std::wstring &path,
                         const std::vector<Common::DuplicateFinder::Group> &groups,
                         unsigned long long reclaimable) {
            std::wstring text;
            text += L"; QuickImageViewer - duplicate pictures\n";
            text += L"; Same BYTES, not merely the same photograph: a re-encoded or\n";
            text += L"; rotated copy is a different file and is NOT listed here.\n";
            text += L"; Nothing has been deleted. This is a listing only.\n";
            text += L";\n";

            wchar_t line[512];
            swprintf_s(line, L"; %zu group(s), %.1f MB held by the extra copies\n",
                       groups.size(), static_cast<double>(reclaimable) / (1024.0 * 1024.0));
            text += line;
            text += L"\n";

            for (const auto &g : groups) {
                swprintf_s(line, L"%zu copies, %llu bytes each\n", g.paths.size(), g.size);
                text += line;
                for (const std::wstring &p : g.paths) {
                    text += L"    ";
                    text += p;
                    text += L"\n";
                }
                text += L"\n";
            }

            std::ofstream f(path, std::ios::out | std::ios::binary | std::ios::trunc);
            if (!f.is_open()) return false;
            const std::string utf8 = Common::Utf8::Encode(Common::Utf8::ToCrlf(text));
            if (!utf8.empty()) f.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
            return static_cast<bool>(f);
        }
    } // namespace

    bool IsRunning() { return g_running.load(std::memory_order_relaxed); }

    void StartAsync(HWND hWnd) {
        bool expected = false;
        if (!g_running.compare_exchange_strong(expected, true)) return; // already scanning

        if (!g_ioWorker.PushTask([hWnd]() {
                // ⚠ BOTH OF THESE RELEASE THEMSELVES ON THE WAY OUT, and the
                // reason is that the thread pool CATCHES AND DISCARDS whatever a
                // task throws. A bad_alloc partway through - a plausible end for
                // an index of two hundred thousand paths - would otherwise leave
                // g_running stuck true, and Ctrl+D dead for the rest of the
                // session with no error anywhere saying why. The result would
                // leak with it.
                struct FlagGuard {
                    ~FlagGuard() { g_running.store(false, std::memory_order_relaxed); }
                } flagGuard;

                auto result = std::make_unique<Result>();

                std::vector<Common::DuplicateFinder::Candidate> candidates;
                for (const auto &e : FolderIndex::Snapshot()) {
                    Common::DuplicateFinder::Candidate c;
                    c.path = e.path;
                    c.size = e.size;
                    candidates.push_back(std::move(c));
                }
                result->indexed = static_cast<int>(candidates.size());

                // Only the files that share a size with another are opened.
                for (const size_t i : Common::DuplicateFinder::NeedHashing(candidates)) {
                    std::uint64_t h = 0;
                    if (HashFile(candidates[i].path, h)) {
                        candidates[i].digest = h;
                        candidates[i].hashed = true;
                        ++result->read;
                    }
                }

                // PROVEN, not merely hashed. Same size and same 64-bit digest is
                // enormously strong evidence and still evidence: a hash maps many
                // inputs onto one value. The consequence of being wrong is a
                // deleted photograph, so each group is confirmed byte for byte
                // before it is reported.
                //
                // Affordable because it runs LAST: what reaches here is a handful
                // of files that already agree twice over, so this confirms rather
                // than searches. A group that turns out to hold two different
                // pictures is split, and any set left with one member is dropped
                // by the same rule FindGroups uses.
                std::vector<Common::DuplicateFinder::Group> groups;
                for (const auto &g : Common::DuplicateFinder::FindGroups(candidates)) {
                    for (auto &set : Common::DuplicateFinder::Partition(g.paths, SameBytes)) {
                        if (set.size() < 2) continue;
                        Common::DuplicateFinder::Group confirmed;
                        confirmed.size  = g.size;
                        confirmed.paths = std::move(set);
                        groups.push_back(std::move(confirmed));
                    }
                }
                result->groups = static_cast<int>(groups.size());
                int groupId = 0;
                for (const auto &g : groups) {
                    result->files += static_cast<int>(g.paths.size());
                    for (const std::wstring &p : g.paths) {
                        result->paths.push_back(p);
                        result->groupIds.push_back(groupId);
                    }
                    ++groupId;
                    // What is RECLAIMABLE is every copy but one - the point is
                    // to keep a copy, so counting the whole group would promise
                    // space that only deleting the picture entirely would free.
                    result->reclaimable += g.size * (g.paths.size() - 1);
                }

                result->reportPath = ReportPath();
                result->reportWritten = !groups.empty() &&
                                        WriteReport(result->reportPath, groups, result->reclaimable);

                // Ownership moves to the window procedure only if the post
                // succeeds - the same rule every other posted payload here
                // follows. release() is called only after PostMessageW has said
                // yes, so a failed post frees it here rather than leaking.
                if (PostMessageW(hWnd, Constants::WM_QIV_DUPLICATES_READY, 0,
                                 reinterpret_cast<LPARAM>(result.get())))
                    (void)result.release();
            })) {
            // The pool refused the task, so nothing will ever clear the flag.
            g_running.store(false, std::memory_order_relaxed);
        }
    }

} // namespace Platform::DuplicateScan
