#pragma once

#include "ConstantsTheme.h"

// ConstantsStrings.h
// Central repository for all user-visible text used in QIV overlays.
// Keep strings here so they have one place to change for localization.

// Internal compile-time string fragments
// Do not use directly outside this header.
#define STR_THUMBNAIL_STRIP         L"Thumbnail strip"
#define STR_CACHE_WINDOW            L"VRAM strip"
#define STR_SEPARATOR               L": "
#define STR_STATE_ON STR_SEPARATOR  L"ON"
#define STR_STATE_OFF STR_SEPARATOR L"OFF"


namespace Constants::Messages {
    // ── MID_CENTER: state-change notifications ──────────────────────────────
    // These are posted via PostCenterMessage and auto-hide after the timer.
    //Inform user has jumped to firs last image in current folder
    constexpr const wchar_t *TOGGLE_FIRST_IMAGE_IN_FOLDER = L"First image: ";
    constexpr const wchar_t *TOGGLE_LAST_IMAGE_IN_FOLDER = L"Last image: ";
    constexpr const wchar_t *GO_TO_LAST_IMAGE_BEFORE_TOGGLE = L"Previous image: ";
    constexpr const wchar_t *CACHE_WINDOW_VISIBLE_MSG = STR_CACHE_WINDOW  STR_STATE_ON;
    constexpr const wchar_t *CACHE_WINDOW_HIDDEN_MSG = STR_CACHE_WINDOW  STR_STATE_OFF;
    constexpr const wchar_t *CACHE_WINDOW_CLEAR_CACHE_MSG = L"Cache cleared!";
    constexpr const wchar_t *DIR_WINDOW_VISIBLE_MSG = STR_THUMBNAIL_STRIP  STR_STATE_ON;
    constexpr const wchar_t *DIR_WINDOW_HIDDEN_MSG = STR_THUMBNAIL_STRIP  STR_STATE_OFF;


    // Overlay master toggle (N / I / Ctrl+0)
    constexpr const wchar_t *INFO_PANELS_ON = L"Info Panels" STR_STATE_ON;
    constexpr const wchar_t *INFO_PANELS_OFF = L"Info Panels" STR_STATE_OFF;

    // Overlay background toggle (P)
    constexpr const wchar_t *OVERLAY_BG_ON = L"Overlay BG" STR_STATE_ON;
    constexpr const wchar_t *OVERLAY_BG_OFF = L"Overlay BG" STR_STATE_OFF;

    // Overlay layout cycle (O)
    constexpr const wchar_t *LAYOUT_GRID = L"Layout: Grid";
    constexpr const wchar_t *LAYOUT_STACKED = L"Layout: Stacked";
    constexpr const wchar_t *LAYOUT_SUMMARY = L"Layout: Summary";

    // Reset / effects
    constexpr const wchar_t *RESET_TO_DEFAULTS = L"Reset to Defaults";
    constexpr const wchar_t *ALL_EFFECTS_RESET = L"All Effects Reset";
    // F5 Refresh/Reload current dir
    constexpr const wchar_t *RELOAD_CURRENT_DIR_MSG = L"Refreshed";
    // F5 inside the History panel — re-reads both .txt files and re-scans every
    // folder. Says how many, because the scan is asynchronous and the row markers
    // land a moment later; without it the key looks like it did nothing.
    constexpr const wchar_t *HISTORY_REFRESHED_MSG = L"History refreshed — rescanning ";
    // Empty-dir placeholder shown in DirWnd / SpawnedDirWnd when the folder has no images
    constexpr const wchar_t *EMPTY_DIR_NO_IMAGES = L"No Images:";
    // Placeholder shown when the directory itself has been deleted
    constexpr const wchar_t *EMPTY_DIR_MISSING = L"⚠  Directory Missing";
    // Placeholder shown in CacheWnd when the VRAM thumbnail cache is empty
    constexpr const wchar_t *EMPTY_CACHE = L"Thumbnail Cache Empty";

    // Q — toggle last/current dir
    constexpr const wchar_t *TOGGLE_DIR_NO_PREV = L"No previous folder";
    constexpr const wchar_t *TOGGLE_DIR_CHANGED = L"→ "; // prefix — append folder name
    constexpr const wchar_t *TOGGLE_DIR_MISSING = L"⚠ Previous folder no longer exists";

    // E — toggle last/current image
    constexpr const wchar_t *TOGGLE_IMAGE_NO_PREV = L"No previous image";
    constexpr const wchar_t *TOGGLE_IMAGE_CHANGED = L"→ "; // prefix — append filename
    constexpr const wchar_t *TOGGLE_IMAGE_MISSING = L"⚠ Previous image no longer exists";

    // Runtime theme factor  (Ctrl+Alt+Shift+Numpad+/-/0)
    constexpr const wchar_t *THEME_FACTOR_PREFIX = L"Theme: ";
    constexpr const wchar_t *THEME_FACTOR_RESET_MSG = L"Theme: Reset";

    // Window chrome toggles  (Ctrl+Shift+Numpad*)
    constexpr const wchar_t *CORNER_ROUND = L"Corners: Round";
    constexpr const wchar_t *CORNER_SQUARE = L"Corners: Square";

    // Backdrop cycle  (Ctrl+Shift+Numpad/)
    constexpr const wchar_t *BACKDROP_NONE = L"Backdrop: None";
    constexpr const wchar_t *BACKDROP_MICA = L"Backdrop: Mica";
    constexpr const wchar_t *BACKDROP_ACRYLIC = L"Backdrop: Acrylic";
    constexpr const wchar_t *BACKDROP_MICA_ALT = L"Backdrop: MicaAlt";

    // Ctrl+F1 / Space / R / S — Slideshow
    constexpr const wchar_t *SLIDESHOW_PLAYING = L"▶ Slideshow"; // prefix; interval/loop/shuffle appended dynamically
    constexpr const wchar_t *SLIDESHOW_PAUSED = L"⏸ Slideshow Paused";
    constexpr const wchar_t *SLIDESHOW_STOPPED = L"■ Slideshow Stopped";
    constexpr const wchar_t *SLIDESHOW_LOOP_ON = L"Loop" STR_STATE_ON;
    constexpr const wchar_t *SLIDESHOW_LOOP_OFF = L"Loop" STR_STATE_OFF;
    constexpr const wchar_t *SLIDESHOW_SHUFFLE_ON = L"Shuffle" STR_STATE_ON;
    constexpr const wchar_t *SLIDESHOW_SHUFFLE_OFF = L"Shuffle" STR_STATE_OFF;
    // Indexed by TransitionType — single source for the tray submenu, the context
    // menu submenu and the overlay message. Order MUST match the enum in
    // SlideshowTransitions.h; size MUST match Constants::Slideshow::TRANSITION_COUNT.
    constexpr const wchar_t *TRANSITION_NAMES[] = {
        L"Cut", L"Fade", L"Dissolve", L"Ripple",
        L"Slide Left", L"Zoom Out", L"Slide Up", L"Zoom In",
        L"Slide Right", L"Slide Down", L"Soft Zoom", L"Spin",
        L"Spin Zoom", L"Drift Left", L"Drift Up", L"Flicker",
        L"Bounce", L"Swing", L"Slam", L"Iris", L"Slide Diagonal"
    };
    constexpr const wchar_t *TRANSITION_PREFIX = L"Transition: "; // append a NAMES entry
    // Indexed by Constants::Slideshow::TransitionSource::NONE..LIST.
    constexpr const wchar_t *TRANSITION_SOURCE_NAMES[] = {
        L"None (use selected)", L"All", L"List (ticked only)"
    };
    // Indexed by Constants::Slideshow::TransitionOrder::SEQUENTIAL..RANDOM.
    constexpr const wchar_t *TRANSITION_ORDER_NAMES[] = { L"Sequential", L"Random" };
    constexpr const wchar_t *TRANSITION_SOURCE_PREFIX = L"Transitions: ";
    constexpr const wchar_t *TRANSITION_ORDER_PREFIX  = L"Transition Order: ";
    constexpr const wchar_t *TRANSITION_LIST_EMPTY    = L"⚠ Transition list is empty";
    // Runtime-appendable forms of the STR_STATE_* macros, for messages whose
    // subject is only known at run time.
    constexpr const wchar_t *STATE_ON_SUFFIX  = STR_STATE_ON;
    constexpr const wchar_t *STATE_OFF_SUFFIX = STR_STATE_OFF;
    constexpr const wchar_t *SLIDESHOW_INTERVAL_PREFIX = L"Interval: "; // append "<n> ms"

    // Ctrl+T — always on top
    constexpr const wchar_t *ALWAYS_ON_TOP_ON = L"Always on Top" STR_STATE_ON;
    constexpr const wchar_t *ALWAYS_ON_TOP_OFF = L"Always on Top" STR_STATE_OFF;

    // F11 / F12 — mirroring to other instances. The ON message names the target
    // count: "Mirroring: On" with nothing connected looks identical to a broken
    // feature, and the number is the fastest way to see which it is.
    constexpr const wchar_t *MIRROR_ON_PREFIX  = L"Mirroring" STR_STATE_ON L" → "; // append "<n> target(s)"
    constexpr const wchar_t *MIRROR_OFF        = L"Mirroring" STR_STATE_OFF;
    constexpr const wchar_t *MIRROR_NO_TARGETS = L"Mirroring" STR_STATE_ON L" — no targets (F10 to connect)";
    // The picker (two or more instances connected) closed with nothing ticked.
    // Mirroring to no one is mirroring off, so say that rather than claim it is
    // on and then forward nothing.
    constexpr const wchar_t *MIRROR_NONE_PICKED = L"Mirroring" STR_STATE_OFF L" — no instance chosen";
    // F12 is meaningless while F11 is off, and a keypress that appears to do
    // nothing reads as a bug. Say why instead.
    constexpr const wchar_t *MIRROR_LOCAL_ON   = L"Mirror: execute here too" STR_STATE_ON;
    constexpr const wchar_t *MIRROR_LOCAL_OFF  = L"Mirror: remote only (this screen stays put)";
    constexpr const wchar_t *MIRROR_LOCAL_IDLE = L"Mirroring is off (F11 to start)";

    // Ctrl+Enter — pushing this viewer's picture at the instances under Control.
    // Every outcome says which it was: a push that reaches nothing must not look
    // the same as one that reached three screens.
    constexpr const wchar_t *PUSH_SENT_PREFIX = L"Pushed image → ";      // + count
    constexpr const wchar_t *PUSH_SENT_SUFFIX = L" instance(s)";
    constexpr const wchar_t *PUSH_NO_IMAGE    = L"Nothing to push — no image loaded";
    // Alt+Enter. Worded differently from the Ctrl+Enter line on purpose: the two
    // do visibly different things at the far end, and an identical message would
    // make a mis-pressed modifier impossible to notice.
    constexpr const wchar_t *PUSH_ONCE_PREFIX = L"Image streamed → ";  // + count
    // Ctrl+Alt+Enter — asking one instance for the picture it is displaying.
    constexpr const wchar_t *STREAM_IN_ASKING_PREFIX = L"Asking ";  // + target name
    constexpr const wchar_t *STREAM_IN_ASKING_SUFFIX = L" for its image…";
    constexpr const wchar_t *STREAM_IN_NO_TARGET =
        L"Nobody to ask — tick an instance under Control (Ctrl+F11)";
    // It answered, and it is showing nothing. An answer, not a failure.
    constexpr const wchar_t *STREAM_IN_EMPTY = L"That instance is showing no image";
    constexpr const wchar_t *STREAM_IN_FAILED = L"Could not fetch that instance's image";

    // Refusals from the transfer itself, shared by both directions so the two ends
    // describe the same fault with the same words.
    constexpr const wchar_t *STREAM_ERR_UNREADABLE = L"cannot read that image file";
    // The far end predates image streaming. Refused with a reason rather than tried:
    // its line buffer is 4 KB, so it answers a chunk by dropping the connection, and
    // "the link died" is a much worse explanation than this one.
    constexpr const wchar_t *STREAM_ERR_PEER_TOO_OLD =
        L"that instance is too old to receive an image — update it (protocol v2)";
    constexpr const wchar_t *STREAM_ERR_TOO_LARGE  = L"image is too large to stream";
    constexpr const wchar_t *STREAM_ERR_NO_TRANSFER =
        L"no transfer in progress — send StreamImageBegin first";
    constexpr const wchar_t *STREAM_ERR_TRUNCATED =
        L"transfer incomplete — fewer bytes arrived than were declared";
    constexpr const wchar_t *PUSH_NO_TARGETS  =
        L"Nothing to push to — tick an instance under Control (Ctrl+F11)";
    // Every ticked instance is on another machine, where a folder path and a
    // playlist index both mean nothing. Named rather than silently skipped.
    constexpr const wchar_t *PUSH_ONLY_REMOTE =
        L"Not pushed — the ticked instances are on another machine";
    constexpr const wchar_t *PUSH_SKIPPED_PREFIX = L" (";                // + count
    constexpr const wchar_t *PUSH_SKIPPED_SUFFIX = L" skipped: another machine)";

    // Shown when connecting forces the sort order off disk order — see
    // LeaveDiskOrderForSession.
    constexpr const wchar_t *REMOTE_SORT_LEFT_DISK_ORDER =
        L"Sort: by Name — disk order cannot be shared between instances";

    // A command refused because a connection is live. The reason is appended
    // from the SESSION_BLOCKED table, so this is only the opening.
    constexpr const wchar_t *REMOTE_BLOCKED_PREFIX = L"Blocked while connected: ";
    // Shown when the lock is turned ON so the operator knows the window went
    // deaf on purpose, and that the tray is the way back.
    constexpr const wchar_t *KIOSK_LOCK_ON  = L"Kiosk Lock" STR_STATE_ON L" — unlock from the tray icon";
    constexpr const wchar_t *KIOSK_LOCK_OFF = L"Kiosk Lock" STR_STATE_OFF;
    constexpr const wchar_t *KEEP_DISPLAY_AWAKE_ON  = L"Keep Display Awake" STR_STATE_ON;
    constexpr const wchar_t *KEEP_DISPLAY_AWAKE_OFF = L"Keep Display Awake" STR_STATE_OFF;

    // Alt+W/A/S/D — keyboard snap to screen half
    constexpr const wchar_t *SNAP_LEFT = L"Snap: Left Half";
    constexpr const wchar_t *SNAP_RIGHT = L"Snap: Right Half";
    constexpr const wchar_t *SNAP_TOP = L"Snap: Top Half";
    constexpr const wchar_t *SNAP_BOTTOM = L"Snap: Bottom Half";

    // Alt+Q/E/Z/C — keyboard snap to screen quarter
    constexpr const wchar_t *SNAP_TOP_LEFT = L"Snap: Top-Left Quarter";
    constexpr const wchar_t *SNAP_TOP_RIGHT = L"Snap: Top-Right Quarter";
    constexpr const wchar_t *SNAP_BOTTOM_LEFT = L"Snap: Bottom-Left Quarter";
    constexpr const wchar_t *SNAP_BOTTOM_RIGHT = L"Snap: Bottom-Right Quarter";

    // Ctrl+Space — fill available screen space (work area minus visible panels) / restore
    constexpr const wchar_t *AUTOSIZE_TO_WORK_AREA = L"Fit to Screen";
    constexpr const wchar_t *AUTOSIZE_RESTORE = L"Default Size";

    // Ctrl+M — move the window to the next monitor. The single-monitor case is
    // reported rather than ignored: a key that does nothing silently reads as
    // broken, and "there is nowhere to go" is the actual answer.
    constexpr const wchar_t *MONITOR_MOVED_PREFIX = L"Monitor ";
    constexpr const wchar_t *MONITOR_ONLY_ONE = L"Only One Monitor";

    // Sort order  (Ctrl+Alt+Shift+0/6/7/8/9)  — press once: ascending, press again: descending
    constexpr const wchar_t *SORT_BY_NAME = L"Sort: Name (A→Z)";
    constexpr const wchar_t *SORT_BY_NAME_REV = L"Sort: Name (Z→A)";
    constexpr const wchar_t *SORT_BY_DATE = L"Sort: Date (Newest)";
    constexpr const wchar_t *SORT_BY_DATE_REV = L"Sort: Date (Oldest)";
    constexpr const wchar_t *SORT_BY_SIZE = L"Sort: Size (Largest)";
    constexpr const wchar_t *SORT_BY_SIZE_REV = L"Sort: Size (Smallest)";
    constexpr const wchar_t *SORT_BY_TYPE = L"Sort: Extension (A→Z)";
    constexpr const wchar_t *SORT_BY_TYPE_REV = L"Sort: Extension (Z→A)";
    constexpr const wchar_t *SORT_BY_DISK = L"Sort: Disk Order";

    // Spawned DirWnd messages



    constexpr const wchar_t *SPAWN_DIR_TOP =  STR_THUMBNAIL_STRIP STR_SEPARATOR L"Top";
    constexpr const wchar_t *SPAWN_DIR_LEFT = STR_THUMBNAIL_STRIP STR_SEPARATOR L"Left";
    constexpr const wchar_t *SPAWN_DIR_RIGHT = STR_THUMBNAIL_STRIP STR_SEPARATOR L"Right";
    constexpr const wchar_t *SPAWN_DIR_BOTTOM = STR_THUMBNAIL_STRIP STR_SEPARATOR L"Bottom";
    constexpr const wchar_t *SPAWN_DIR_CLOSED = STR_THUMBNAIL_STRIP STR_SEPARATOR L"Closed";
    constexpr const wchar_t *SPAWN_DIR_NO_SPACE = L"No free positions for " STR_THUMBNAIL_STRIP;
    constexpr const wchar_t *COPIED_TO_CLIPBOARD = L"Copied to Clipboard";

    // Desktop wallpaper — NAMES is indexed by Constants::Wallpaper::FILL..SPAN and
    // is the single source for both the submenu labels and the overlay message.
    constexpr const wchar_t *WALLPAPER_NAMES[] = {
        L"Fill", L"Fit", L"Stretch", L"Tile", L"Center", L"Span"
    };
    constexpr const wchar_t *WALLPAPER_SET = L"Wallpaper: "; // prefix — append the style name
    constexpr const wchar_t *WALLPAPER_FAILED = L"⚠ Wallpaper could not be applied";
    // Folder walking — used by ALL three walkers (horizontal wheel, PageUp/Down,
    // Insert/Delete) so the centre message never depends on how you moved.
    // Format is "<prefix><n>/<total> <folder name>", where <n> is the row number
    // the History panel shows for that folder, so overlay and panel always agree.
    // The prefix is chosen from the row itself: starred rows get ★, the rest 📁.
    constexpr const wchar_t *WALK_HISTORY_FOLDER = Constants::ThemeIcons::ICON_FOLDER;
    constexpr const wchar_t *WALK_FAVORITE_FOLDER = Constants::ThemeIcons::ICON_FAVORITES_MARK;
    // Shown when a walk stepped over more than one dead folder to reach its
    // destination — append the count. A single skip names the folder instead.
    // Text only: ThemeIcons entries are constexpr pointers, not macros, so
    // ICON_WARNING cannot be concatenated into the literal here — the caller
    // prepends it at runtime.
    constexpr const wchar_t *WALK_SKIPPED = L" skipped ";
    // History panel footer total, and its hover popup.
    // Footer reads "<size>/<files>" plus "/<n>" only when folders were excluded.
    // The popup spells all three out, then names what was left out and why it
    // could be: an alias whose target is already counted elsewhere in the list.
    // Shown centred in the History panel while the background folder sweep is
    // still running. The panel stays fully usable — the scan does not block it,
    // and closing the panel does not stop the scan.
    constexpr const wchar_t *HISTORY_SCANNING = L"Loading ...";

    // The word for a folder, used everywhere in this popup so it can be changed
    // in one place (dirs / folders / directories).
    constexpr const wchar_t *WORD_DIRS = L"dirs";

    constexpr const wchar_t *TOTAL_HEADER = L"TOTAL:";
    constexpr const wchar_t *TOTAL_SIZE_LABEL = L"Size: ";
    constexpr const wchar_t *TOTAL_FILES_LABEL = L"Files: ";
    constexpr const wchar_t *TOTAL_DIRS_LABEL = L"Dirs: ";
    constexpr const wchar_t *TOTAL_SEPARATOR = L"===========";
    constexpr const wchar_t *TOTAL_EXCLUDED_SUFFIX = L" excluded:"; // "<n> dirs excluded:"
    constexpr const wchar_t *EXCLUDED_BULLET = L"* ";               // before each number
    // Group headings under the excluded count. Each names WHY those folders
    // contribute nothing, then lists them numbered from 1.
    constexpr const wchar_t *EXCLUDED_DUPLICATES = L"already counted (symlinks of a listed folder):";
    constexpr const wchar_t *EXCLUDED_MISSING = L"missing ";  // + WORD_DIRS + ":"
    constexpr const wchar_t *EXCLUDED_EMPTY = L"empty ";      // + WORD_DIRS + ":"

    // History panel — row badge labels. One line each in the hover popup that
    // appears when a row carries more than one badge and they have to share a slot.
    constexpr const wchar_t *BADGE_MISSING = L"Folder not found";
    constexpr const wchar_t *BADGE_EMPTY = L"No images in folder";
    constexpr const wchar_t *BADGE_FAVORITE = L"Favorite";

    // History panel — symlink glyph hover popup. Line 1 is the kind of link,
    // line 2 is the resolved destination (filled in at runtime).
    constexpr const wchar_t *LINK_KIND_JUNCTION = L"Directory junction  (mklink /J)";
    constexpr const wchar_t *LINK_KIND_SYMLINK = L"Directory symlink  (mklink /D)";
    // Path resolves elsewhere but no component is a reparse point — the drive
    // letter itself is redirected (subst, or a mapped network drive).
    constexpr const wchar_t *LINK_KIND_MAPPED = L"Mapped path  (subst / network drive)";
    constexpr const wchar_t *LINK_TARGET_UNKNOWN = L"target could not be resolved";

    constexpr const wchar_t *WALK_NO_HISTORY_FOLDERS = L"No other history folders";
    constexpr const wchar_t *WALK_NO_FAVORITE_FOLDERS = L"No favorite folders";
    constexpr const wchar_t *FOLDER_DEAD_MISSING = L"⚠ Folder not found";
    constexpr const wchar_t *FOLDER_DEAD_EMPTY = L"⚠ No images in folder";
    constexpr const wchar_t *FOLDER_DELETED_NOTIFY = L"⚠ Folder deleted";

    // Thumbnail strip wrap-around
    constexpr const wchar_t *THUMB_STRIP_WRAP_TO_START = L"↩ Start";
    constexpr const wchar_t *THUMB_STRIP_WRAP_TO_END = L"↪ End";
    constexpr const wchar_t *THUMB_STRIP_WRAP_ON = STR_THUMBNAIL_STRIP L" Wrap" STR_STATE_ON;
    constexpr const wchar_t *THUMB_STRIP_WRAP_OFF = STR_THUMBNAIL_STRIP L" Wrap" STR_STATE_OFF;

    // Remote control over TCP/IP (src/Rem_TCP_IP). Shown in the remote panel's
    // status line. An enabled server with an empty AllowList refuses every
    // connection — stating that is the difference between a configured refusal
    // and something that merely looks broken.
    // NOTE: there is no "server disabled" reason any more. Autostart describes
    // launch behaviour only, so there is no state in which the user asks the
    // server to start and is told it is not allowed to.
    constexpr const wchar_t *REMOTE_BLOCKED_NO_PORT        = L"No port configured";
    // A name identifies this instance to whoever drives it — see WhyCannotStart.
    constexpr const wchar_t *REMOTE_BLOCKED_NO_NAME        =
        L"No name set — a driving instance identifies this one by name";
    constexpr const wchar_t *REMOTE_WARN_EMPTY_ALLOWLIST   = L"AllowList empty — all connections denied";
    // A HARD STOP, not a warning, and the only rule here that refuses a
    // configuration the user explicitly asked for.
    //
    // An unauthenticated listener on loopback is a local convenience and stays
    // permitted. The same listener on a LAN or public interface is an open door
    // to the folder history, to opening any path, and to reading back whatever
    // is on screen — with no step at which anyone is asked who they are.
    //
    // Refusing to start is the only honest response: a warning on a panel nobody
    // has open does not defend anything, and this is precisely the mistake that
    // is invisible until it has already been exploited.
    constexpr const wchar_t *REMOTE_BLOCKED_NO_PASSWORD    =
        L"No password set — required unless bound to loopback (127.0.0.1)";
    // A stored value this build cannot parse — in practice one written before
    // the move to PBKDF2. It cannot be converted, because converting it needs
    // the plaintext and the stored form deliberately does not keep it.
    //
    // Refusing to start is the only safe reading. The alternative — treating an
    // unparseable password as no password — is how a listener ends up
    // unauthenticated because of a file format change, which is exactly the
    // class of accident that must not be possible.
    constexpr const wchar_t *REMOTE_BLOCKED_BAD_PASSWORD   =
        L"Stored password is in an old or damaged format — set it again";

    // Written into qivRemoteServerBlacklist.ini by the brute-force guard. The
    // count is spliced in from Constants::RemoteTcpIp::AUTH_MAX_FAILURES rather
    // than spelled out, so raising the threshold cannot leave the file claiming
    // a figure that is no longer true.
    //
    // Says what happened in plain words on purpose: this line is read months
    // later by somebody deciding whether it is safe to delete.
    constexpr const wchar_t *BLACKLIST_REASON_AUTH_PREFIX = L"blocked automatically: ";
    constexpr const wchar_t *BLACKLIST_REASON_AUTH_SUFFIX =
        L" failed authentications in 10 minutes";
    // Panel footers reporting WHERE the values on screen came from.
    //
    // Every panel backed by a file says this on open, because a populated panel
    // is otherwise ambiguous — identical whether it was loaded from disk or is
    // showing defaults — and that difference decides whether saving creates a
    // file or overwrites one.
    constexpr const wchar_t *REMOTE_PANEL_READ_FROM = L"Read from ";
    constexpr const wchar_t *REMOTE_PANEL_NO_INI =
        L"No settings file — these are defaults. Save to INI creates one.";
    constexpr const wchar_t *REMOTES_PANEL_NO_FILE =
        L"No qivRemoteServers.ini — added rows are saved to it automatically.";
    // The F8 Dedicated panel says the same thing in its own subtitle
    // ("Editing: <path>" / "New instance — not yet generated"), so it needs
    // nothing here.

    // Overlay top-right server indicator: "<dot> 2/4  86.0%".
    //
    // COLOUR emoji glyphs, so they keep their own colour while the rest of the
    // slot is drawn in the user's chosen overlay colour. The outer slots share
    // one brush; a per-range drawing effect would be the alternative and is far
    // more machinery for one dot.
    //
    // Shown only while the listener is running, so its absence is the "stopped"
    // state and there is nothing to grey out. The COLOUR then carries a second
    // fact that is otherwise invisible until something goes wrong:
    //
    //   GREEN  (U+1F7E2)  TLS — reachable from off this machine, encrypted
    //   ORANGE (U+1F7E0)  plaintext — loopback only, nothing to encrypt against
    //
    // Orange rather than red on purpose: a loopback listener is not a fault or a
    // misconfiguration, it is the normal local case. Red would cry wolf on the
    // setup most people run.
    constexpr const wchar_t *OVERLAY_SERVER_DOT_TLS   = L"\U0001F7E2";
    constexpr const wchar_t *OVERLAY_SERVER_DOT_PLAIN = L"\U0001F7E0";

    // F9 status line. The wording matches the overlay dot's two colours, so the
    // panel and the indicator cannot appear to disagree.
    constexpr const wchar_t *REMOTE_STATUS_TLS   = L"· TLS for remote clients";
    constexpr const wchar_t *REMOTE_STATUS_PLAIN = L"· loopback only — nothing to encrypt";
    constexpr const wchar_t *REMOTE_STATUS_FINGERPRINT = L"Fingerprint: ";
    constexpr const wchar_t *REMOTE_FINGERPRINT_COPIED = L"— copied";

    // SendDisplayedPreview failures. Separate reasons because the remedies
    // differ: no WIC is a broken install, a decode failure is a format this
    // build cannot read, and an encode failure is neither.
    constexpr const wchar_t *PREVIEW_NO_WIC =
        L"imaging component unavailable on this machine";
    constexpr const wchar_t *PREVIEW_CANNOT_DECODE =
        L"cannot decode that image for a preview — ask for the original instead";
    constexpr const wchar_t *PREVIEW_ENCODE_FAILED =
        L"could not encode the preview";

    // --- TLS (src/Rem_TCP_IP/RemoteTls.*) ---------------------------------
    constexpr const wchar_t *REMOTE_TLS_HANDSHAKE_FAILED =
        L"TLS handshake failed";
    // Refusing an unpinned server is the CORRECT outcome, so the text explains
    // rather than apologises: the first connection is the one an attacker has
    // to be present for, and trusting it silently is what pinning exists to
    // prevent.
    constexpr const wchar_t *REMOTE_TLS_NO_PIN =
        L"No fingerprint stored for this server. Compare the one shown against "
        L"the server's F9 panel, then save it to allow this connection.";
    constexpr const wchar_t *REMOTE_TLS_PIN_MISMATCH =
        L"CERTIFICATE MISMATCH — this is not the server you saved. Either its "
        L"certificate was regenerated, or something is impersonating it. "
        L"Connection refused.";
    // A listener that cannot obtain credentials must not run. Falling back to
    // plaintext on an address reachable from off the machine would be the exact
    // failure TLS was added to prevent, arrived at by accident.
    constexpr const wchar_t *REMOTE_BLOCKED_TLS_UNAVAILABLE =
        L"TLS could not be initialised — refusing to listen unencrypted: ";

    constexpr const wchar_t *BLACKLIST_REASON_CMDLINE = L"added with -remoteBlock";
    constexpr const wchar_t *BLACKLIST_REASON_PANEL   = L"added in the F9 panel";

    // Start-up failures. Each names the actual cause: "could not start" with no
    // reason is the least useful thing a status line can say.
    constexpr const wchar_t *REMOTE_ALREADY_RUNNING        = L"Server already running";
    constexpr const wchar_t *REMOTE_WSA_FAILED             = L"Winsock initialisation failed";
    constexpr const wchar_t *REMOTE_BAD_BIND_ADDRESS       = L"Bind address is not a valid IP literal";
    constexpr const wchar_t *REMOTE_SOCKET_FAILED          = L"Could not create socket";
    constexpr const wchar_t *REMOTE_PORT_IN_USE            = L"Port already in use";
    constexpr const wchar_t *REMOTE_BIND_FAILED            = L"Could not bind to that address and port";
    constexpr const wchar_t *REMOTE_LISTEN_FAILED          = L"Socket could not enter listening state";
    // Shown in the center overlay when an enabled listener fails at startup.
    // Deliberately non-fatal: a screen whose port is taken must still show
    // pictures rather than refuse to launch.
    constexpr const wchar_t *REMOTE_START_FAILED_PREFIX     = L"Remote control: ";

    // Client side — connecting to another instance.
    constexpr const wchar_t *REMOTE_CLIENT_NO_HOST           = L"No target address";
    constexpr const wchar_t *REMOTE_CLIENT_BAD_PORT          = L"Target port out of range";
    constexpr const wchar_t *REMOTE_CLIENT_RESOLVE_FAILED    = L"Could not resolve that host";
    constexpr const wchar_t *REMOTE_CLIENT_CONNECT_FAILED    = L"Could not connect";
    constexpr const wchar_t *REMOTE_CLIENT_NO_BANNER         = L"Peer did not identify itself";
    constexpr const wchar_t *REMOTE_CLIENT_PROTOCOL_ERROR    = L"Unexpected reply from peer";
    constexpr const wchar_t *REMOTE_CLIENT_PASSWORD_REQUIRED = L"Peer requires a password";
    constexpr const wchar_t *REMOTE_CLIENT_AUTH_FAILED       = L"Password rejected by peer";
    constexpr const wchar_t *REMOTE_CLIENT_NOT_CONNECTED     = L"Not connected";
    constexpr const wchar_t *REMOTE_CLIENT_SEND_FAILED       = L"Connection lost while sending";
    constexpr const wchar_t *REMOTE_CLIENT_NO_REPLY          = L"No reply from peer";
    // Imported-credential failures. Distinct from a wrong password, because the
    // remedy is different: re-import rather than retype.
    constexpr const wchar_t *REMOTE_CLIENT_BAD_SECRET        = L"Stored credentials are malformed";
    constexpr const wchar_t *REMOTE_CLIENT_SECRET_STALE      = L"Its password changed since this was imported";

    // Viewport lock (Y) — zoom + pan carried across image changes
    constexpr const wchar_t *VIEWPORT_LOCK_ON  = L"Viewport Lock" STR_STATE_ON;
    constexpr const wchar_t *VIEWPORT_LOCK_OFF = L"Viewport Lock" STR_STATE_OFF;

    // Thumbnail strip visual effects
    constexpr const wchar_t *THUMB_EFFECTS_ON  = L"Thumbnail Effects" STR_STATE_ON;
    constexpr const wchar_t *THUMB_EFFECTS_OFF = L"Thumbnail Effects" STR_STATE_OFF;

    // Overlay slot names, indexed by OverlayManager::Slot. Single source for
    // every centre message that names a slot, whatever triggered the change.
    constexpr const wchar_t *OVERLAY_SLOT_NAMES[] = {
        L"Top Left", L"Top Center", L"Top Right",
        L"Mid Left", L"Mid Center", L"Mid Right",
        L"Bot Left", L"Bot Center", L"Bot Right"
    };
    constexpr const wchar_t *OVERLAY_PREFIX        = L"Overlay ";
    constexpr const wchar_t *OVERLAY_FONT_PREFIX   = L"Overlay Font: "; // append the family name
    constexpr const wchar_t *OVERLAY_STATE_COMPACT = STR_SEPARATOR L"Compact";
    constexpr const wchar_t *OVERLAY_STATE_FULL    = STR_SEPARATOR L"Full";

    // BOT_LEFT readouts — independent toggles in the Overlays menu
    constexpr const wchar_t *OVERLAY_EFFECTS_LIST_ON  = L"Effects Overlay" STR_STATE_ON;
    constexpr const wchar_t *OVERLAY_EFFECTS_LIST_OFF = L"Effects Overlay" STR_STATE_OFF;
    constexpr const wchar_t *OVERLAY_DIR_NAME_ON      = L"Folder Name" STR_STATE_ON;
    constexpr const wchar_t *OVERLAY_DIR_NAME_OFF     = L"Folder Name" STR_STATE_OFF;

    // Settings-menu-only toggles (no keyboard shortcut counterpart)
    constexpr const wchar_t *KEEP_IN_BG_ON        = L"Keep in Background" STR_STATE_ON;
    constexpr const wchar_t *KEEP_IN_BG_OFF       = L"Keep in Background" STR_STATE_OFF;
    constexpr const wchar_t *RUN_ON_STARTUP_ON    = L"Run on Startup" STR_STATE_ON;
    constexpr const wchar_t *RUN_ON_STARTUP_OFF   = L"Run on Startup" STR_STATE_OFF;
    constexpr const wchar_t *HISTORY_FULL_ON      = L"History: Full List" STR_STATE_ON;
    constexpr const wchar_t *HISTORY_FULL_OFF     = L"History: Full List" STR_STATE_OFF;
    constexpr const wchar_t *OPEN_THUMB_START_ON  = L"Thumbnail Strip on Start" STR_STATE_ON;
    constexpr const wchar_t *OPEN_THUMB_START_OFF = L"Thumbnail Strip on Start" STR_STATE_OFF;
    constexpr const wchar_t *SWAP_MOUSE_ON        = L"Swap Mouse Buttons" STR_STATE_ON;
    constexpr const wchar_t *SWAP_MOUSE_OFF       = L"Swap Mouse Buttons" STR_STATE_OFF;
    constexpr const wchar_t *WHEEL_INVERT_ON      = L"Invert Scroll" STR_STATE_ON;
    constexpr const wchar_t *WHEEL_INVERT_OFF     = L"Invert Scroll" STR_STATE_OFF;
    constexpr const wchar_t *WHEEL_INVERT_H_ON    = L"Invert Horizontal Scroll" STR_STATE_ON;
    constexpr const wchar_t *WHEEL_INVERT_H_OFF   = L"Invert Horizontal Scroll" STR_STATE_OFF;
    constexpr const wchar_t *START_FULLSCREEN_ON  = L"Start in Fullscreen" STR_STATE_ON;
    constexpr const wchar_t *START_FULLSCREEN_OFF = L"Start in Fullscreen" STR_STATE_OFF;
    constexpr const wchar_t *CTRL_C_COPY_ON       = L"Ctrl+C Copy" STR_STATE_ON;
    constexpr const wchar_t *CTRL_C_COPY_OFF      = L"Ctrl+C Copy" STR_STATE_OFF;
    constexpr const wchar_t *THUMB_COPY_OP_ON     = L"Thumbnail Copy" STR_STATE_ON;
    constexpr const wchar_t *THUMB_COPY_OP_OFF    = L"Thumbnail Copy" STR_STATE_OFF;
    constexpr const wchar_t *THUMB_MOVE_OP_ON     = L"Thumbnail Move" STR_STATE_ON;
    constexpr const wchar_t *THUMB_MOVE_OP_OFF    = L"Thumbnail Move" STR_STATE_OFF;
    constexpr const wchar_t *THUMB_DELETE_OP_ON   = L"Thumbnail Delete" STR_STATE_ON;
    constexpr const wchar_t *THUMB_DELETE_OP_OFF  = L"Thumbnail Delete" STR_STATE_OFF;
    constexpr const wchar_t *THUMB_PASTE_OP_ON    = L"Thumbnail Paste" STR_STATE_ON;
    constexpr const wchar_t *THUMB_PASTE_OP_OFF   = L"Thumbnail Paste" STR_STATE_OFF;

    // View modes (keys 1-5)
    constexpr const wchar_t *VIEW_MODE_PREFIX = L"View: ";
    constexpr const wchar_t *VIEW_MODE_NAMES[] = {
        L"Fit to View",
        L"Fit to Width",
        L"Fit to Height",
        L"Fit to Window",
        L"Original Size"
    };

    // Zoom popup
    constexpr const wchar_t *ZOOM_TO_PREFIX = L"Zoom: ";
    // %s pairs — the bounds arrive pre-formatted from
    // Converters::FormatPercentCompact so their precision follows the value.
    // A fixed "%.1f" here printed ZOOM_MIN = 0.01 as "0.0".
    constexpr const wchar_t *ZOOM_TO_INPUT_HINT_FMT = L"Enter zoom in percent (%s%% – %s%%, 0 = reset)";

    // ZoomWnd — hint line below the input box
    // The out-of-range / invalid messages are built at draw time straight from
    // Constants::ZoomPanel::ZOOM_MIN and ZOOM_MAX — both are already percents.
    constexpr const wchar_t *ZOOM_ENTER_HINT = L"Enter = apply zoom  •  Esc = cancel";
    constexpr const wchar_t *ZOOM_OUT_OF_RANGE_FMT = L"Out of range — type a value between %s%% and %s%%";
    constexpr const wchar_t *ZOOM_RESET_MESSAGE = L"Zoom restored to default";
    // Shown centre-screen when a zoom keypress/wheel tick is capped by
    // ZoomPanel::ZOOM_MIN / ZOOM_MAX — otherwise the input looks ignored.
    constexpr const wchar_t *ZOOM_MIN_REACHED = L"Minimum zoom reached";
    constexpr const wchar_t *ZOOM_MAX_REACHED = L"Maximum zoom reached";
}

namespace Constants::Strings {
    // ── BOT_LEFT slot: active color-effect labels ───────────────────────────
    // Used in OverlayManager::UpdateEffects() via appendLine()

    // Named colour effects. These strings are also the KEYS in
    // app.activeEffectsList, so the renderer matches on them — changing a value
    // changes what RendererD2D::ChainEffectByName() compares against.
    //
    // Shown as "Desaturate", not "Grayscale", deliberately: effects stack, so a
    // later Sepia tints this one's output and the picture ends up brown while
    // the label is still listed. "Grayscale" reads as a promise that the final
    // image IS gray and looks broken the moment anything follows it;
    // "Desaturate" names the operation being performed, which is what the entry
    // actually is. The identifier stays EFFECT_GRAYSCALE to match
    // app.effectGrayscale and Command::ToggleGrayscale.
    constexpr const wchar_t *EFFECT_GRAYSCALE = L"Desaturate";
    constexpr const wchar_t *EFFECT_INVERT = L"Invert";
    constexpr const wchar_t *EFFECT_SEPIA = L"Sepia";
    constexpr const wchar_t *EFFECT_SOLARIZE = L"Solarize";
    constexpr const wchar_t *EFFECT_OUTLINE = L"Outline";
    constexpr const wchar_t *EFFECT_THRESHOLD = L"Threshold";

    // Continuous-parameter labels (prefix before the numeric value)
    constexpr const wchar_t *LABEL_BRIGHTNESS = L"Brightness: ";
    constexpr const wchar_t *LABEL_CONTRAST = L"Contrast: ";
    constexpr const wchar_t *LABEL_SATURATION = L"Saturation: ";
    constexpr const wchar_t *LABEL_GAMMA = L"Gamma: ";

    // Brightness sign prefix (positive values)
    constexpr const wchar_t *SIGN_POSITIVE = L"+";
}
// Prevent helper macros leaking outside this header
#undef STR_STATE_OFF
#undef STR_STATE_ON
#undef STR_SEPARATOR
#undef STR_CACHE_WINDOW
#undef STR_THUMBNAIL_STRIP
