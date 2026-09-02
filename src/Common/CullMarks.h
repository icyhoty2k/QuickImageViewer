// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#pragma once
#include <string>
#include <unordered_map>
#include <vector>

// KEEP / REJECT MARKS, WHILE GOING THROUGH A FOLDER.
//
// The job a fast viewer is actually used for: page through a shoot, say yes or
// no to each frame, then act on the noes in one go. Everything here is the
// bookkeeping for that, and nothing here touches a file.
//
// ⚠ MARKS LIVE IN MEMORY AND DIE WITH THE SESSION, deliberately. Writing them
// to disk means a sidecar file per picture or a database, and both make a
// viewer that promised to leave nothing behind start leaving things behind.
// The marks exist to be resolved in the same sitting; a mark you cannot
// remember making is not one you should act on.
//
// KEYED BY PATH, not by playlist index. Sorting the folder, opening another
// one and coming back, or a file appearing mid-session all renumber the
// playlist - and a mark that moved to a different picture is how the wrong
// file gets deleted.
namespace Common::CullMarks {

    enum class Mark { None, Keep, Reject };

    class Set {
        public:
            // Cycles rather than sets: pressing the same key twice undoes it,
            // which is what a hand moving quickly expects. Marking Keep on
            // something already Rejected switches it outright - the user has
            // changed their mind, not asked for a third state.
            void Toggle(const std::wstring &path, Mark to) {
                if (to == Mark::None) { m_marks.erase(path); return; }
                const auto it = m_marks.find(path);
                if (it != m_marks.end() && it->second == to) {
                    m_marks.erase(it);      // pressed twice: undo
                    return;
                }
                m_marks[path] = to;
            }

            // Assigns, with no undo-on-repeat. The menu form.
            //
            // Named Assign rather than Set because the CLASS is Set - a member
            // function of that name is a constructor, which the compiler says
            // plainly and only once you have written it.
            //
            // ⚠ NOT A CONVENIENCE WRAPPER AROUND Toggle - it is the other
            // operation, and the difference only shows on a SET OF FILES.
            // Toggle is right for a key pressed on one picture: press twice,
            // undo. Run it over a selection of forty where half are already
            // Keep and those forty split - twenty flip to unmarked while twenty
            // become Keep - so a menu item saying "Mark Keep" leaves half the
            // selection unmarked. A menu item has to mean its own label.
            void Assign(const std::wstring &path, Mark to) {
                if (to == Mark::None) { m_marks.erase(path); return; }
                m_marks[path] = to;
            }

            [[nodiscard]] Mark Of(const std::wstring &path) const {
                const auto it = m_marks.find(path);
                return it == m_marks.end() ? Mark::None : it->second;
            }

            [[nodiscard]] int Count(Mark m) const {
                int n = 0;
                for (const auto &kv : m_marks) if (kv.second == m) ++n;
                return n;
            }

            [[nodiscard]] bool Empty() const { return m_marks.empty(); }

            // The rejected paths, in the order given rather than in hash order.
            //
            // ⚠ THE ORDER MATTERS BECAUSE A HUMAN READS IT. The resolve step
            // lists what it is about to move, and a list that reshuffles between
            // one look and the next is one nobody can check. Passing the
            // playlist in is what makes it the folder's own order.
            [[nodiscard]] std::vector<std::wstring>
            Rejected(const std::vector<std::wstring> &order) const {
                std::vector<std::wstring> out;
                for (const std::wstring &p : order)
                    if (Of(p) == Mark::Reject) out.push_back(p);
                return out;
            }

            // Forgets one path. Used after a file has actually been moved, so a
            // second resolve does not try again on something already gone.
            void Forget(const std::wstring &path) { m_marks.erase(path); }

            void Clear() { m_marks.clear(); }

        private:
            std::unordered_map<std::wstring, Mark> m_marks;
    };

} // namespace Common::CullMarks
