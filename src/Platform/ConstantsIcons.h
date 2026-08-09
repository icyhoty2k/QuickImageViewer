// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#pragma once

// ConstantsIcons.h
// THE one place every icon glyph in the app is written down. A panel that wants
// a warning triangle names Constants::Icon::WARNING, and swapping the glyph is
// a one-line edit here rather than a hunt through fifty files.
//
// WHAT COUNTS AS AN ICON: a glyph carrying meaning on its own — a status badge,
// a hand-drawn tick, a direction marker, a separator between fields. Ordinary
// typography inside a sentence is NOT an icon and stays in the sentence: the
// dashes in "Grid — Stacked", the arrows in "None → Mica → Acrylic", the × in
// "1920 × 1080", the minus in "Num −". Those are words, and pulling them out
// would make the text unreadable to gain nothing. The exception is help text
// that DESCRIBES one of these icons ("Ctrl+F11's ◉") — that must track the
// glyph, so it names the constant.
//
// TWO FORMS OF EVERY ICON, from ONE definition:
//
//   QIV_ICON_<NAME>       a macro holding the raw wide literal
//   Constants::Icon::NAME a constexpr pointer initialised from that macro
//
// Runtime code uses the constexpr name. The macro exists because C++ cannot
// paste a constexpr pointer into a string literal, and ConstantsStrings.h needs
// exactly that — "⚠ Folder not found" is one compile-time literal, not a
// runtime concatenation. Adjacent-literal pasting gives it the icon without
// either a second copy of the codepoint or a std::wstring build at run time.
//
// So: use the macro ONLY where a literal is being assembled at compile time.
// Everywhere else use Constants::Icon::NAME.
//
// Written as \x / \U escapes rather than pasted glyphs. A variation selector is
// invisible in an editor, and one stripped by a save or a merge would change
// what renders with nothing in the diff to explain it.

// ─────────────────────────────────────────────────────────────────────────────
// Status / indicators
// ─────────────────────────────────────────────────────────────────────────────
#define QIV_ICON_FAVORITES_MARK  L"\x2605"        // ★
#define QIV_ICON_SYMLINK_MARK    L"\U0001F517"    // 🔗
#define QIV_ICON_WARNING         L"\x26A0"        // ⚠

// NO variation selector, unlike SECTION_INFO below.
//
// U+2139 defaults to TEXT presentation; adding U+FE0F forces the emoji one.
// In emoji presentation Segoe UI Emoji gives it a full-square advance with
// the ink sitting right of centre inside it, so a centred line that opens
// with it reads as nudged right — DWrite centres the advance, not the ink.
// As a text glyph it is proportioned like the letters beside it and takes
// the line's own colour.
#define QIV_ICON_INFO            L"\x2139"        // ℹ
#define QIV_ICON_EMPTY           L"\x2205"        // ∅
#define QIV_ICON_CLOSE           L"\x2715"        // ✕
#define QIV_ICON_CHECK           L"\x2714"        // ✔
#define QIV_ICON_FOLDER_ARROW    L"\x25B8"        // ▸

// Shown when a row has MORE than one badge and only one slot to show them in;
// hovering it lists them all. Two joined squares — the layered look says
// "several things stacked here" without borrowing any badge's own meaning.
#define QIV_ICON_BADGE_STACK     L"\x29C9"        // ⧉

// ─────────────────────────────────────────────────────────────────────────────
// Separators — the two dots used to break a footer or status line into parts
// ─────────────────────────────────────────────────────────────────────────────
#define QIV_ICON_BULLET          L"\x2022"        // •
#define QIV_ICON_MIDDLE_DOT      L"\x00B7"        // ·

// ─────────────────────────────────────────────────────────────────────────────
// Directional arrows
// ─────────────────────────────────────────────────────────────────────────────
#define QIV_ICON_ARROW_RIGHT     L"\x2192"        // →
#define QIV_ICON_ARROW_LEFT      L"\x2190"        // ←
#define QIV_ICON_ARROW_UP        L"\x2191"        // ↑
#define QIV_ICON_ARROW_DOWN      L"\x2193"        // ↓
#define QIV_ICON_ARROWS_UP_DOWN  L"\x2191\x2193"  // ↑↓

// ─────────────────────────────────────────────────────────────────────────────
// Media playback
// ─────────────────────────────────────────────────────────────────────────────
#define QIV_ICON_PLAY            L"\x25B6"        // ▶
#define QIV_ICON_PAUSE           L"\x23F8"        // ⏸
#define QIV_ICON_STOP            L"\x25A0"        // ■

// ─────────────────────────────────────────────────────────────────────────────
// Wrap navigation
// ─────────────────────────────────────────────────────────────────────────────
#define QIV_ICON_WRAP_START      L"\x21A9"        // ↩
#define QIV_ICON_WRAP_END        L"\x21AA"        // ↪

// ─────────────────────────────────────────────────────────────────────────────
// Sort markers and paging triangles
// ─────────────────────────────────────────────────────────────────────────────
#define QIV_ICON_SORT_ASCENDING  L"\x25B2"        // ▲
#define QIV_ICON_SORT_DESCENDING L"\x25BC"        // ▼
#define QIV_ICON_PAGE_PREV       L"\x25C0"        // ◀
// Same triangle as PLAY. Named separately because a Next button and a play
// button are different things to whoever reads the code, and one of the two
// could want a different glyph later without dragging the other along.
#define QIV_ICON_PAGE_NEXT       L"\x25B6"        // ▶

// ─────────────────────────────────────────────────────────────────────────────
// Hand-drawn toggles — the remote panels paint their own rows, so a tick or a
// radio is a glyph rather than a control.
// ─────────────────────────────────────────────────────────────────────────────
#define QIV_ICON_CHECKBOX_ON     L"\x2611"        // ☑
#define QIV_ICON_CHECKBOX_OFF    L"\x2610"        // ☐
#define QIV_ICON_RADIO_ON        L"\x25C9"        // ◉
#define QIV_ICON_RADIO_OFF       L"\x25CB"        // ○
// The clickable run/stop dot in the Servers panel.
#define QIV_ICON_DOT_FILLED      L"\x25CF"        // ●

// ─────────────────────────────────────────────────────────────────────────────
// Objects / folders
// ─────────────────────────────────────────────────────────────────────────────
#define QIV_ICON_FOLDER          L"\U0001F4C1"    // 📁
#define QIV_ICON_FOLDER_OPEN     L"\U0001F4C2"    // 📂
// "You are here" rather than a second folder: the line it marks names the
// folder you are ALREADY in, and two near-identical folder glyphs stacked
// above each other read as one repeated thing rather than two different ones.
#define QIV_ICON_LOCATION        L"\U0001F4CD"    // 📍

// ─────────────────────────────────────────────────────────────────────────────
// Folder-tree walk (Alt+Up / Down / Left / Right)
//
// THE BORDERED RECTANGLE COMES FROM SEGOE UI EMOJI, and from nothing else.
// Rendering all four codepoints across the candidate faces settles it:
//
//   Segoe UI Emoji    ⬆ ⬇ ⬅ ➡   all four, arrow inside a rounded border
//   Segoe UI Symbol   ⬆ ⬇ ⬅ ➡   all four, plain arrows, no border
//   Segoe UI          ⬆ ⬇ ⬅ ➡   all four, plain arrows, no border
//
// So the odd one out was never the glyph — it was the FONT FALLBACK. A bare
// U+27A1 happens to reach Segoe UI Emoji, while bare U+2B05-2B07 land in
// Segoe UI, which is why one arrow had a border and three did not.
//
// U+FE0F on every one asks for the emoji form explicitly, so all four resolve
// to the same face and get the same border. It is on the right arrow too:
// that is the presentation it was already getting by luck, and pinning it
// means a font-fallback change on some other machine cannot silently take the
// border away again.
//
// THIS MUST STAY PAIRED WITH D2D1_DRAW_TEXT_OPTIONS_NONE at the centre-message
// draw call. The outline form is what carries the border and takes the
// message brush; ENABLE_COLOR_FONT swaps it for a flat filled blue tile and
// loses both. See OverlayManager's centre-slot branch.
// ─────────────────────────────────────────────────────────────────────────────
#define QIV_ICON_WALK_UP         L"\x2B06\xFE0F"  // ⬆️
#define QIV_ICON_WALK_DOWN       L"\x2B07\xFE0F"  // ⬇️
#define QIV_ICON_WALK_PREV       L"\x2B05\xFE0F"  // ⬅️
#define QIV_ICON_WALK_NEXT       L"\x27A1\xFE0F"  // ➡️

// ─────────────────────────────────────────────────────────────────────────────
// Remote control over TCP/IP — where the other end lives, and what it is
// ─────────────────────────────────────────────────────────────────────────────
#define QIV_ICON_HOME            L"\U0001F3E0"    // 🏠 this machine
#define QIV_ICON_LAN             L"\U0001F5A7"    // 🖧 same network
#define QIV_ICON_GLOBE           L"\U0001F310"    // 🌐 public — leaves the network
#define QIV_ICON_CLIENT          L"\U0001F64B"    // 🙋 something using our listener
#define QIV_ICON_ANTENNA         L"\U0001F4E1"    // 📡 a listener we drive

// ─────────────────────────────────────────────────────────────────────────────
// Coloured status dots (overlay top-right server indicator)
//
// COLOUR emoji glyphs, so they keep their own colour while the rest of the
// slot is drawn in the user's chosen overlay colour. All the same emoji class,
// so every one measures the same width and the count beside them never jumps.
// ─────────────────────────────────────────────────────────────────────────────
#define QIV_ICON_DOT_GREEN       L"\U0001F7E2"    // 🟢
#define QIV_ICON_DOT_ORANGE      L"\U0001F7E0"    // 🟠
#define QIV_ICON_DOT_RED         L"\U0001F534"    // 🔴
#define QIV_ICON_DOT_YELLOW      L"\U0001F7E1"    // 🟡
#define QIV_ICON_DOT_WHITE       L"\U000026AA"    // ⚪
#define QIV_ICON_DOT_BLACK       L"\U000026AB"    // ⚫

// ─────────────────────────────────────────────────────────────────────────────
// HelpWnd section headers
//
// These carry U+FE0F where the codepoint would otherwise default to text
// presentation — unlike QIV_ICON_INFO above. They sit in a column of other
// emoji, where a text glyph would be the odd one out.
// ─────────────────────────────────────────────────────────────────────────────
#define QIV_ICON_SECTION_COMPASS   L"\U0001F9ED"        // 🧭
#define QIV_ICON_SECTION_MAGNIFIER L"\U0001F50D"        // 🔍
#define QIV_ICON_SECTION_MOUSE     L"\U0001F5B1\xFE0F"  // 🖱️
#define QIV_ICON_SECTION_WINDOW    L"\U0001FA9F"        // 🪟
#define QIV_ICON_SECTION_TOOLBOX   L"\U0001F9F0"        // 🧰
#define QIV_ICON_SECTION_PICTURE   L"\U0001F5BC\xFE0F"  // 🖼️
#define QIV_ICON_SECTION_SCROLL    L"\U0001F4DC"        // 📜
#define QIV_ICON_SECTION_PLAY      L"\x25B6\xFE0F"      // ▶️
#define QIV_ICON_SECTION_INFO      L"\x2139\xFE0F"      // ℹ️
#define QIV_ICON_SECTION_PALETTE   L"\U0001F3A8"        // 🎨
#define QIV_ICON_SECTION_FLOPPY    L"\U0001F4BE"        // 💾
#define QIV_ICON_SECTION_GEAR      L"\x2699\xFE0F"      // ⚙️
#define QIV_ICON_SECTION_BELL      L"\U0001F514"        // 🔔
#define QIV_ICON_SECTION_DESKTOP   L"\U0001F5A5\xFE0F"  // 🖥️
#define QIV_ICON_SECTION_KEYBOARD  L"\x2328\xFE0F"      // ⌨️
// 📡 is emoji-by-default, so the antenna needs no variation selector and the
// section header is the same literal the remote panels use.
#define QIV_ICON_SECTION_ANTENNA   QIV_ICON_ANTENNA     // 📡

namespace Constants::Icon {
    // ── Status / indicators ──────────────────────────────────────────────────
    constexpr const wchar_t *FAVORITES_MARK  = QIV_ICON_FAVORITES_MARK;
    constexpr const wchar_t *SYMLINK_MARK    = QIV_ICON_SYMLINK_MARK;
    constexpr const wchar_t *WARNING         = QIV_ICON_WARNING;
    constexpr const wchar_t *INFO            = QIV_ICON_INFO;
    constexpr const wchar_t *EMPTY           = QIV_ICON_EMPTY;
    constexpr const wchar_t *CLOSE           = QIV_ICON_CLOSE;
    constexpr const wchar_t *CHECK           = QIV_ICON_CHECK;
    constexpr const wchar_t *FOLDER_ARROW    = QIV_ICON_FOLDER_ARROW;
    constexpr const wchar_t *BADGE_STACK     = QIV_ICON_BADGE_STACK;

    // ── Separators ───────────────────────────────────────────────────────────
    constexpr const wchar_t *BULLET          = QIV_ICON_BULLET;
    constexpr const wchar_t *MIDDLE_DOT      = QIV_ICON_MIDDLE_DOT;

    // ── Directional arrows ───────────────────────────────────────────────────
    constexpr const wchar_t *ARROW_RIGHT     = QIV_ICON_ARROW_RIGHT;
    constexpr const wchar_t *ARROW_LEFT      = QIV_ICON_ARROW_LEFT;
    constexpr const wchar_t *ARROW_UP        = QIV_ICON_ARROW_UP;
    constexpr const wchar_t *ARROW_DOWN      = QIV_ICON_ARROW_DOWN;
    constexpr const wchar_t *ARROWS_UP_DOWN  = QIV_ICON_ARROWS_UP_DOWN;

    // ── Media playback ───────────────────────────────────────────────────────
    constexpr const wchar_t *PLAY            = QIV_ICON_PLAY;
    constexpr const wchar_t *PAUSE           = QIV_ICON_PAUSE;
    constexpr const wchar_t *STOP            = QIV_ICON_STOP;

    // ── Wrap navigation ──────────────────────────────────────────────────────
    constexpr const wchar_t *WRAP_START      = QIV_ICON_WRAP_START;
    constexpr const wchar_t *WRAP_END        = QIV_ICON_WRAP_END;

    // ── Sort markers and paging triangles ────────────────────────────────────
    constexpr const wchar_t *SORT_ASCENDING  = QIV_ICON_SORT_ASCENDING;
    constexpr const wchar_t *SORT_DESCENDING = QIV_ICON_SORT_DESCENDING;
    constexpr const wchar_t *PAGE_PREV       = QIV_ICON_PAGE_PREV;
    constexpr const wchar_t *PAGE_NEXT       = QIV_ICON_PAGE_NEXT;

    // ── Hand-drawn toggles ───────────────────────────────────────────────────
    constexpr const wchar_t *CHECKBOX_ON     = QIV_ICON_CHECKBOX_ON;
    constexpr const wchar_t *CHECKBOX_OFF    = QIV_ICON_CHECKBOX_OFF;
    constexpr const wchar_t *RADIO_ON        = QIV_ICON_RADIO_ON;
    constexpr const wchar_t *RADIO_OFF       = QIV_ICON_RADIO_OFF;
    constexpr const wchar_t *DOT_FILLED      = QIV_ICON_DOT_FILLED;

    // ── Objects / folders ────────────────────────────────────────────────────
    constexpr const wchar_t *FOLDER          = QIV_ICON_FOLDER;
    constexpr const wchar_t *FOLDER_OPEN     = QIV_ICON_FOLDER_OPEN;
    constexpr const wchar_t *LOCATION        = QIV_ICON_LOCATION;

    // HISTORY — the folders already visited, as opposed to a folder as a thing
    // on disk. The distinction is the whole point of it existing: the history
    // walk announced itself with 📁, which is what the FOLDER-TREE walk is
    // about, so the two features that move between folders were marked with the
    // same glyph while doing different things.
    //
    // AN ALIAS OF THE SECTION ICON, not a second literal. HelpWnd's HISTORY
    // PANEL section already used 📜, so this is the app's existing mark for the
    // idea and the panel, the help section and the walk overlay now all lead
    // with one glyph — which only stays true while there is one definition.
    constexpr const wchar_t *HISTORY         = QIV_ICON_SECTION_SCROLL;

    // ── Folder-tree walk ─────────────────────────────────────────────────────
    constexpr const wchar_t *WALK_UP         = QIV_ICON_WALK_UP;
    constexpr const wchar_t *WALK_DOWN       = QIV_ICON_WALK_DOWN;
    constexpr const wchar_t *WALK_PREV       = QIV_ICON_WALK_PREV;
    constexpr const wchar_t *WALK_NEXT       = QIV_ICON_WALK_NEXT;

    // ── Remote control over TCP/IP ───────────────────────────────────────────
    constexpr const wchar_t *HOME            = QIV_ICON_HOME;
    constexpr const wchar_t *LAN             = QIV_ICON_LAN;
    constexpr const wchar_t *GLOBE           = QIV_ICON_GLOBE;
    constexpr const wchar_t *CLIENT          = QIV_ICON_CLIENT;
    constexpr const wchar_t *ANTENNA         = QIV_ICON_ANTENNA;

    // ── Coloured status dots ─────────────────────────────────────────────────
    constexpr const wchar_t *DOT_GREEN       = QIV_ICON_DOT_GREEN;
    constexpr const wchar_t *DOT_ORANGE      = QIV_ICON_DOT_ORANGE;
    constexpr const wchar_t *DOT_RED         = QIV_ICON_DOT_RED;
    constexpr const wchar_t *DOT_YELLOW      = QIV_ICON_DOT_YELLOW;
    constexpr const wchar_t *DOT_WHITE       = QIV_ICON_DOT_WHITE;
    constexpr const wchar_t *DOT_BLACK       = QIV_ICON_DOT_BLACK;

    // ── HelpWnd section headers ──────────────────────────────────────────────
    constexpr const wchar_t *SECTION_COMPASS   = QIV_ICON_SECTION_COMPASS;
    constexpr const wchar_t *SECTION_MAGNIFIER = QIV_ICON_SECTION_MAGNIFIER;
    constexpr const wchar_t *SECTION_MOUSE     = QIV_ICON_SECTION_MOUSE;
    constexpr const wchar_t *SECTION_WINDOW    = QIV_ICON_SECTION_WINDOW;
    constexpr const wchar_t *SECTION_TOOLBOX   = QIV_ICON_SECTION_TOOLBOX;
    constexpr const wchar_t *SECTION_PICTURE   = QIV_ICON_SECTION_PICTURE;
    constexpr const wchar_t *SECTION_SCROLL    = QIV_ICON_SECTION_SCROLL;
    constexpr const wchar_t *SECTION_PLAY      = QIV_ICON_SECTION_PLAY;
    constexpr const wchar_t *SECTION_INFO      = QIV_ICON_SECTION_INFO;
    constexpr const wchar_t *SECTION_PALETTE   = QIV_ICON_SECTION_PALETTE;
    constexpr const wchar_t *SECTION_FLOPPY    = QIV_ICON_SECTION_FLOPPY;
    constexpr const wchar_t *SECTION_GEAR      = QIV_ICON_SECTION_GEAR;
    constexpr const wchar_t *SECTION_BELL      = QIV_ICON_SECTION_BELL;
    constexpr const wchar_t *SECTION_DESKTOP   = QIV_ICON_SECTION_DESKTOP;
    constexpr const wchar_t *SECTION_KEYBOARD  = QIV_ICON_SECTION_KEYBOARD;
    constexpr const wchar_t *SECTION_ANTENNA   = QIV_ICON_SECTION_ANTENNA;
}
