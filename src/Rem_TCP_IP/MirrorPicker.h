#pragma once
#include <windows.h>

// =============================================================================
// MirrorPicker — "which of them am I driving?", asked at the moment F11 goes on.
//
// With one instance connected the question does not exist and nothing is shown.
// With several it always did exist, and the answer used to be "all of them"
// with no way to say otherwise: a wall of screens is the case mirroring was
// built for, but so is a desk with a second viewer that should stay where it is
// while the first follows along.
//
// A CHECKABLE POPUP, not another panel. The choice is made in the second
// between pressing F11 and starting to navigate, and a floating window would
// have to be opened, aimed at and closed again. The F10 console remains the
// place where targets are configured; this only narrows what the next keystroke
// reaches.
//
// Win32 closes a menu on every pick, so a multi-select popup has to be put back
// up after each tick — the same loop the transition list uses (AppMenu.cpp).
// Dismissing it (Esc, or a click outside) keeps whatever is ticked at that
// moment: there is no cancel, because every tick has already taken effect on
// the target list and pretending otherwise would need a copy to roll back to.
//
// The selection lives in Remote::Mirror (session state, not persisted).
// =============================================================================

namespace Remote::Mirror {

    // Puts the picker up if it is worth asking — two or more CONNECTED targets.
    // Returns how many are selected when it closes, which is what the caller
    // reports on screen; 0 means the user unticked everything, and the caller
    // treats that as "do not switch mirroring on".
    //
    // With fewer than two connected targets nothing is shown and this is just
    // MirroredLiveCount(): one screen needs no disambiguating, and a popup that
    // appeared with a single row in it would be a keypress the user has to
    // dismiss for no gain.
    // `force` shows it whenever anything is connected, single target included —
    // Shift+F11, which is a deliberate "let me see and change this", not a side
    // effect of switching mirroring on. It is also the only way to reach Sync
    // now from the keyboard when just one instance is joined.
    int PickTargets(HWND hOwner, bool force = false);

} // namespace Remote::Mirror
