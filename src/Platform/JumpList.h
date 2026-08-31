// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// =============================================================================
//  JumpList — the folders qIV knows about, on the taskbar icon
//
//  Right-click the taskbar button (or the Start-menu entry) and Windows shows a
//  Jump List. This fills it with the folders the viewer already tracks:
//  favourites first, then recent ones, each opening that folder in a new qIV.
//
//  WHY IT IS WORTH HAVING. The history is already there — the panel, the Tab
//  walk and the phone all read the same list — but only from inside a running
//  qIV. The Jump List is the one place it is useful BEFORE the app is open,
//  which is exactly when somebody is deciding what to look at.
//
//  IT COSTS NOTHING WHEN NOBODY LOOKS. Building the list is a few COM calls over
//  a list capped at JUMP_LIST_MAX entries; it runs at startup and whenever the
//  folder list changes, never per frame.
//
//  ⚠ THE SHELL, NOT THE APP, OWNS WHAT IS SHOWN. Windows keeps its own copy
//  keyed by the exe path, so entries survive a restart and a list that is not
//  rebuilt simply stays as it was. Deleting an entry is the user's to do,
//  through the shell's own "Remove from this list" — which is why Refresh
//  rebuilds the whole list rather than appending.
// =============================================================================

namespace Platform::JumpList {

    // Rebuilds the list from the live folder history.
    //
    // UI THREAD ONLY: it reads the same vectors the history panel mutates, via
    // UI::SnapshotHistoryForRemote().
    //
    // Silent on every failure. A Jump List that cannot be built is a cosmetic
    // loss on a machine that may simply have them switched off in Settings, and
    // there is nothing the user could do with the error.
    void Refresh();

    // Empties it. Used when the user clears the history, so the taskbar does not
    // go on offering folders the app has just forgotten.
    void Clear();

} // namespace Platform::JumpList
