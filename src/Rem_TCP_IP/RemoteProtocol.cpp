#include "RemoteProtocol.h"
#include "RemoteMirror.h" // SessionActive — the switch BlockedNow hangs off
#include "Platform/Constants.h"

#include <algorithm>
#include <cwctype>

namespace Remote {

namespace RT = Constants::RemoteTcpIp;

// =============================================================================
// THE REACHABLE-COMMAND TABLE
//
// This is the whole remote API surface. A Command that is not listed here cannot
// be driven over the network, whatever a client sends.
//
// Several rows may map to the same Command — that is how the short scripting
// aliases work (`next` and `NextImage` are the same thing). The canonical enum
// name is always accepted; the short alias is listed first because that is what
// people actually type.
//
// DELIBERATELY ABSENT, and why:
//   SaveImage                  raises a Save dialog and writes files
//   CmdArgsExport/Import/      all raise file choosers; a blocked dialog would
//     GenerateShortcut/Test    hold the connection open indefinitely
//   ToggleDedicated            switches the persistence namespace at runtime
//   OpenFile without payload   the payload form is exposed as `open`; the bare
//                              command raises a file chooser
// =============================================================================
namespace {
    constexpr CommandEntry TABLE[] = {
        // --- Navigation ---
        { L"NextImage",             Command::NextImage,                       PayloadRule::None,
          L"next image in the playlist" },
        { L"PrevImage",             Command::PrevImage,                       PayloadRule::None,
          L"previous image in the playlist" },
        { L"GoToFirstImage",        Command::GoToFirstImage,                  PayloadRule::None,
          L"first image of the whole playlist" },
        { L"GoToLastImage",         Command::GoToLastImage,                   PayloadRule::None,
          L"last image of the whole playlist" },
        { L"GoToLastImageInCurrentFolder",
                                    Command::GoToLastImageInCurrentFolder,    PayloadRule::None,
          L"last image of the CURRENT folder — different from GoToLastImage when the "
          L"playlist spans several folders" },
        { L"ToggleFirstLastImageInCurrentFolder",
                                    Command::ToggleFirstLastImageInCurrentFolder, PayloadRule::None,
          L"jump between the first and last image of the current folder, alternating "
          L"on each call" },
        { L"ShowInExplorer",        Command::ShowInExplorer,                  PayloadRule::None,
          L"open Explorer on the current file, selected. Opens a window ON THAT "
          L"MACHINE — of no use on a screen nobody is sitting at" },
        { L"ToggleLastDir",         Command::ToggleLastDir,                   PayloadRule::None,
          L"jump back to the previous FOLDER, and again to come back — two folders, "
          L"alternating" },
        { L"ToggleLastImage",       Command::ToggleLastImage,                 PayloadRule::None,
          L"jump back to the previous IMAGE, and again to come back — the A/B compare" },
        { L"PrevHistoryFolder",     Command::PrevHistoryFolder,               PayloadRule::None,
          L"walk back through recently opened folders (the Tab history list)" },
        { L"NextHistoryFolder",     Command::NextHistoryFolder,               PayloadRule::None,
          L"walk forward through recently opened folders" },
        { L"NextFavoriteFolder",    Command::NextFavoriteFolder,              PayloadRule::None,
          L"open the next FAVOURITE folder" },
        { L"PrevFavoriteFolder",    Command::PrevFavoriteFolder,              PayloadRule::None,
          L"open the previous FAVOURITE folder" },
        { L"ReloadCurrentDir",      Command::ReloadCurrentDir,                PayloadRule::None,
          L"rescan the current folder and rebuild the playlist — after files were "
          L"added or removed behind the viewer's back" },

        // --- Commands carrying a value ---
        // Payload rows carry TWO descriptions: what the command does, and what its
        // value means. The Ctrl+F10 panel puts one over each box.
        { L"JumpToImage",           Command::JumpToImage,                     PayloadRule::Required,
          L"go to a numbered image in the current playlist",
          L"image NUMBER, 1-based, 1…count — the same number the overlay and the "
          L"JumpTo panel show.   e.g.  12   ·  out of range is refused, not clamped" },
        { L"OpenFile",              Command::OpenFile,                        PayloadRule::Required,
          L"open a file, or a folder — and STAY there: the playlist is rebuilt and "
          L"that folder becomes where the instance lives",
          L"full path, read on the RECEIVER's disk. A file opens its folder and lands "
          L"on it; a folder opens at the first image. Quotes are tolerated.   e.g.  "
          L"D:\\Photos\\IMG_0042.jpg   ·   D:\\Photos" },
        { L"FindImage",             Command::FindImage,                       PayloadRule::Required,
          L"jump to the best name match in this playlist. Refused while a connection "
          L"is live — it searches one playlist and the two ends may hold different ones",
          L"text matched against FILE NAMES, case-insensitive. Plain text is a fuzzy "
          L"match, * and ? make it a wildcard.   e.g.  IMG_0042   ·   *.png   ·   dsc?9" },
        { L"ZoomTo",                Command::ZoomTo,                          PayloadRule::Required,
          L"set the zoom level, ignoring the view mode's fit",
          L"percent, no sign.   e.g.  150  = one and a half times   ·   50  = half   "
          L"·   0  returns to the view mode's natural fit" },

        // --- Read-only ---
        // The one row that CHANGES NOTHING. Every other command reports its
        // value as a side effect of doing something; this exists to be asked,
        // and Ctrl+Enter's push is built on the answer: it is how the driving
        // instance finds out whether the far end already holds this folder in
        // this order, and therefore whether an index is safe to send at all.
        { L"QueryState",            Command::QueryState,                      PayloadRule::None,
          L"read-only, changes NOTHING: answers with that instance's folder, sort "
          L"order, playlist length and current position. What Ctrl+Enter asks before "
          L"deciding whether an image number is safe to send.   answer:  "
          L"count=238;index=12;sort=0;sortrev=0;name=IMG_0012.jpg;folder=D:\\Photos" },
        { L"QueryHistory",          Command::QueryHistory,                    PayloadRule::None,
          L"read-only, changes NOTHING: answers with this instance's folder history, "
          L"so a client that cannot see that machine's disk can offer a folder to jump "
          L"to and then send OpenFile. Favourites carry a leading *.   answer:  "
          L"count=12;folders=*D:\\Photos|D:\\Scans|C:\\Wallpapers" },
        { L"QueryClients",          Command::QueryClients,                    PayloadRule::None,
          L"read-only, changes NOTHING: how many clients this listener has, its "
          L"configured cap, and the address it is bound to. The count INCLUDES the "
          L"caller.   answer:  clients=2;max=4;endpoint=0.0.0.0:8770" },

        // --- One-shot display, and the image transfer that feeds it ---
        //
        // All five are in the table because the Ctrl+F10 Send Command panel builds
        // its list from it: a feature that cannot be exercised by hand cannot be
        // diagnosed by hand either. They show a picture WITHOUT joining it to
        // anything — no playlist entry, no index, no sort order — and the next
        // image change drops it. Read paths, like `OpenFile` and less invasive.
        { L"ShowImageOnce",         Command::ShowImageOnce,                   PayloadRule::Required,
          L"show ONE image once, in place of the current slide, and change nothing "
          L"else — no folder, no sort order, no playlist position. Gone at the next "
          L"image change. Same machine only: for another machine, stream it instead",
          L"full path to one image file, read on the RECEIVER's disk. A folder is "
          L"refused.   e.g.  D:\\Ads\\promo.png" },
        { L"StreamImageBegin",      Command::StreamImageBegin,                PayloadRule::Required,
          L"start an image transfer — the first of three: Begin, then Chunk for each "
          L"slice, then Show. Replaces any transfer already in progress",
          L"<totalBytes> <fileName>.  The count is enforced on every chunk and proved "
          L"at Show; the name is used ONLY for its extension, to pick a decoder, and "
          L"never as a path.   e.g.  204800 promo.png" },
        { L"StreamImageChunk",      Command::StreamImageChunk,                PayloadRule::Required,
          L"append one slice to the transfer in progress. Refused if the total would "
          L"exceed what Begin declared",
          L"base64 of the next raw bytes of the file. Any slice size fits, as long as "
          L"the whole line stays under the protocol's line limit — 96 KB of bytes per "
          L"chunk is what this app sends" },
        { L"StreamImageShow",       Command::StreamImageShow,                 PayloadRule::None,
          L"the transfer is complete: check the byte count, then show it once, exactly "
          L"as ShowImageOnce would. A short transfer is refused rather than displayed "
          L"torn" },
        { L"SendDisplayedImage",    Command::SendDisplayedImage,              PayloadRule::None,
          L"read-only: answers with the picture currently on THAT screen — including a "
          L"one-shot image if one is up. Body lines of base64, then the OK naming the "
          L"size and file.   answer:  DATA <base64> … then  "
          L"OK SendDisplayedImage=204800;IMG_0012.jpg  (0; when it shows nothing)" },

        // --- The three driving commands, so they can be sent by hand too ---
        //
        // Reachable so the Send Command panel can exercise them, and so a script
        // can. Sending one to a target makes THAT instance do it to its own
        // targets, which terminates: the commands it then sends do not themselves
        // trigger another send. None of them is mirrorable — see IsMirrorable.
        { L"SendImagePositionToRemotes",
                                    Command::SendImagePositionToRemotes,      PayloadRule::None,
          L"folder + sort order + position to the instances under Control (same "
          L"machine only). They GO there and stay" },
        { L"StreamImageToRemotes",  Command::StreamImageToRemotes,            PayloadRule::None,
          L"stream the current image's bytes to the instances under Control, shown "
          L"once. Works to any machine" },
        { L"StreamImageFromRemote", Command::StreamImageFromRemote,           PayloadRule::None,
          L"ask ONE controlled instance for the picture it is displaying and show "
          L"it here, once. Works from any machine" },
        { L"SlideshowSetInterval",  Command::SlideshowSetInterval,            PayloadRule::Required,
          L"set how long each slide is held. Takes effect on the next slide; does not "
          L"start or stop the slideshow",
          L"milliseconds, 100 … 60000 — outside that is refused.   e.g.  5000  = five "
          L"seconds   ·   30000  = half a minute" },

        // --- View modes ---
        // The five fits, by their numbers — the same 1…5 the V-key cycle and the
        // `view=` key in `Sync` use, so a script and the keyboard agree.
        { L"ViewMode1",             Command::ViewMode1,                       PayloadRule::None,
          L"fit inside the window, aspect ratio KEPT (the default)" },
        { L"ViewMode2",             Command::ViewMode2,                       PayloadRule::None,
          L"fit to the window's WIDTH, aspect ratio not kept" },
        { L"ViewMode3",             Command::ViewMode3,                       PayloadRule::None,
          L"fit to the window's HEIGHT, aspect ratio not kept" },
        { L"ViewMode4",             Command::ViewMode4,                       PayloadRule::None,
          L"stretch to fill the window, aspect ratio not kept" },
        { L"ViewMode5",             Command::ViewMode5,                       PayloadRule::None,
          L"original pixel size, no scaling — 1:1" },

        // --- Zoom / viewport ---
        { L"ZoomIn",                Command::ZoomIn,                          PayloadRule::None,
          L"zoom in one step. ZoomTo sets an exact percentage instead" },
        { L"ZoomOut",               Command::ZoomOut,                         PayloadRule::None,
          L"zoom out one step" },
        { L"ZoomReset",             Command::ZoomReset,                       PayloadRule::None,
          L"drop zoom and pan, back to the view mode's own fit" },
        { L"ToggleViewportLock",    Command::ToggleViewportLock,              PayloadRule::None,
          L"carry the current zoom and pan to the NEXT image instead of resetting it "
          L"— for flipping through same-framed shots at one detail" },
        { L"PanLeft",               Command::PanLeft,                         PayloadRule::None,
          L"pan the view one step left. Only does anything while zoomed in" },
        { L"PanRight",              Command::PanRight,                        PayloadRule::None,
          L"pan one step right (only while zoomed in)" },
        { L"PanUp",                 Command::PanUp,                           PayloadRule::None,
          L"pan one step up (only while zoomed in)" },
        { L"PanDown",               Command::PanDown,                         PayloadRule::None,
          L"pan one step down (only while zoomed in)" },

        // --- Transform ---
        // Display-only, all four: nothing is written to the file. They are also
        // overwritten by the next image's EXIF orientation, which is why a rotation
        // does not persist across a navigation.
        { L"RotateCW",              Command::RotateCW,                        PayloadRule::None,
          L"rotate the VIEW 90° clockwise. Display only — the file is never touched" },
        { L"RotateCCW",             Command::RotateCCW,                       PayloadRule::None,
          L"rotate the view 90° anticlockwise. Display only" },
        { L"FlipH",                 Command::FlipH,                           PayloadRule::None,
          L"mirror the view left-to-right. Display only" },
        { L"FlipV",                 Command::FlipV,                           PayloadRule::None,
          L"mirror the view top-to-bottom. Display only" },

        // --- Slideshow ---
        { L"SlideshowToggle",       Command::SlideshowToggle,                 PayloadRule::None,
          L"start the slideshow, or stop it if it is running" },
        { L"SlideshowPauseResume",  Command::SlideshowPauseResume,            PayloadRule::None,
          L"hold the current slide, or carry on — the slideshow stays STARTED either "
          L"way, unlike SlideshowToggle which ends it" },
        { L"SlideshowToggleLoop",   Command::SlideshowToggleLoop,             PayloadRule::None,
          L"at the end of the playlist: start again, or stop" },
        { L"SlideshowToggleShuffle",Command::SlideshowToggleShuffle,          PayloadRule::None,
          L"random order instead of playlist order. The permutation is rebuilt when "
          L"the folder changes under it" },
        { L"SlideshowCycleTransition",
                                    Command::SlideshowCycleTransition,        PayloadRule::None,
          L"step to the next slide TRANSITION effect (cut, fade, and the rest)" },

        // --- Window / chrome ---
        // Geometry, all of it. Reachable by hand and by script, but deliberately NOT
        // mirrored: slaves are placed on fixed monitors, usually fullscreen, and a
        // forwarded snap or move knocks one off the screen it was put on.
        { L"ToggleFullscreen",      Command::ToggleFullscreen,                PayloadRule::None,
          L"borderless fullscreen on the monitor the window is on, or back" },
        { L"ToggleAlwaysOnTop",     Command::ToggleAlwaysOnTop,               PayloadRule::None,
          L"keep this window (and its panels) above other windows" },
        { L"AutosizeToWorkArea",    Command::AutosizeToWorkArea,              PayloadRule::None,
          L"resize to fill the monitor's work area — the screen minus the taskbar" },
        { L"ResizeWindowLarger",    Command::ResizeWindowLarger,              PayloadRule::None,
          L"grow the window one step, centre held" },
        { L"ResizeWindowSmaller",   Command::ResizeWindowSmaller,             PayloadRule::None,
          L"shrink the window one step" },
        { L"MoveWindowLeft",        Command::MoveWindowLeft,                  PayloadRule::None,
          L"nudge the window one step left" },
        { L"MoveWindowRight",       Command::MoveWindowRight,                 PayloadRule::None,
          L"nudge the window one step right" },
        { L"MoveWindowUp",          Command::MoveWindowUp,                    PayloadRule::None,
          L"nudge the window one step up" },
        { L"MoveWindowDown",        Command::MoveWindowDown,                  PayloadRule::None,
          L"nudge the window one step down" },
        { L"SnapLeft",              Command::SnapLeft,                        PayloadRule::None,
          L"snap to the LEFT HALF of the work area" },
        { L"SnapRight",             Command::SnapRight,                       PayloadRule::None,
          L"snap to the right half" },
        { L"SnapTop",               Command::SnapTop,                         PayloadRule::None,
          L"snap to the top half" },
        { L"SnapBottom",            Command::SnapBottom,                      PayloadRule::None,
          L"snap to the bottom half" },
        { L"SnapTopLeft",           Command::SnapTopLeft,                     PayloadRule::None,
          L"snap to the top-left QUARTER" },
        { L"SnapTopRight",          Command::SnapTopRight,                    PayloadRule::None,
          L"snap to the top-right quarter" },
        { L"SnapBottomLeft",        Command::SnapBottomLeft,                  PayloadRule::None,
          L"snap to the bottom-left quarter" },
        { L"SnapBottomRight",       Command::SnapBottomRight,                 PayloadRule::None,
          L"snap to the bottom-right quarter" },
        { L"ToggleCornerPreference",Command::ToggleCornerPreference,          PayloadRule::None,
          L"square window corners, or the rounded ones Windows 11 draws" },
        { L"CycleBackdropType",     Command::CycleBackdropType,               PayloadRule::None,
          L"step through the window backdrop materials — plain, Mica, acrylic" },

        // --- Panels / overlays ---
        // Panels raise a WINDOW on the receiving screen, which then has to be closed
        // from here — reachable on purpose (a script setting a wall up), never
        // mirrored, because one keypress must not open a window on every screen.
        { L"ToggleHelp",            Command::ToggleHelp,                      PayloadRule::None,
          L"open or close the Help panel on that instance" },
        { L"ToggleCache",           Command::ToggleCache,                     PayloadRule::None,
          L"open or close the VRAM cache panel" },
        { L"ClearCache",            Command::ClearCache,                      PayloadRule::None,
          L"drop every decoded bitmap from the VRAM cache. The current image is "
          L"re-decoded; nothing on disk changes" },
        { L"ToggleDir",             Command::ToggleDir,                       PayloadRule::None,
          L"open or close the folder thumbnail strip" },
        { L"ToggleHistory",         Command::ToggleHistory,                   PayloadRule::None,
          L"open or close the recent-folders list" },
        { L"ToggleHistoryFull",     Command::ToggleHistoryFull,               PayloadRule::None,
          L"the same list in its FULL form — every recorded folder, not the short "
          L"recent set" },
        { L"ToggleOverlay",         Command::ToggleOverlay,                   PayloadRule::None,
          L"the on-image text overlay (file name, size, position) on or off" },
        { L"CycleOverlayLayout",    Command::CycleOverlayLayout,              PayloadRule::None,
          L"step the overlay through its layouts — where the text sits and how much "
          L"of it there is" },
        { L"ToggleOverlayBackground",
                                    Command::ToggleOverlayBackground,         PayloadRule::None,
          L"the shaded plate behind the overlay text, for light images" },
        { L"ShowInfo",              Command::ShowInfo,                        PayloadRule::None,
          L"open or close the EXIF / file information panel" },
        { L"ToggleStats",           Command::ToggleStats,                     PayloadRule::None,
          L"open or close the render statistics panel" },
        { L"CloseAllPanels",        Command::CloseAllPanels,                  PayloadRule::None,
          L"close every open panel, remembering which were open" },
        { L"RestoreAllPanels",      Command::RestoreAllPanels,                PayloadRule::None,
          L"reopen the panels CloseAllPanels remembered" },
        { L"ToggleAllPanels",       Command::ToggleAllPanels,                 PayloadRule::None,
          L"close them all, or restore them — whichever the current state is not" },
        { L"ToggleDedicatedPanel",  Command::ToggleDedicatedPanel,            PayloadRule::None,
          L"open or close the Dedicated-instance configuration panel" },
        { L"ToggleThumbnailWrapAround",
                                    Command::ToggleThumbnailWrapAround,       PayloadRule::None,
          L"whether the wheel over a thumbnail strip wraps past the ends or stops" },
        { L"ToggleThumbnailEffects",Command::ToggleThumbnailEffects,          PayloadRule::None,
          L"whether thumbnails are drawn with the active colour effects applied" },

        // --- Colour effects ---
        // The six named effects STACK, in the order they are switched on, each
        // applying to the result of the last. That order is state a flag cannot
        // express, which is why `Sync` carries the list and not six booleans.
        { L"ToggleGrayscale",       Command::ToggleGrayscale,                 PayloadRule::None,
          L"grayscale on or off. Effects stack in the order switched on" },
        { L"ToggleInvert",          Command::ToggleInvert,                    PayloadRule::None,
          L"invert the colours (negative)" },
        { L"ToggleSepia",           Command::ToggleSepia,                     PayloadRule::None,
          L"sepia tone" },
        { L"ToggleSolarize",        Command::ToggleSolarize,                  PayloadRule::None,
          L"solarize — invert only the tones above a threshold" },
        { L"ToggleOutline",         Command::ToggleOutline,                   PayloadRule::None,
          L"edge detection: draw the outlines only" },
        { L"ToggleThreshold",       Command::ToggleThreshold,                 PayloadRule::None,
          L"reduce to two tones at a cut-off — pure black and white" },
        { L"ToggleEffectPreview",   Command::ToggleEffectPreview,             PayloadRule::None,
          L"show the effect stack applied to a small preview instead of the whole "
          L"image, for judging one before committing to it" },
        { L"ResetEffects",          Command::ResetEffects,                    PayloadRule::None,
          L"clear every colour effect and the four adjustments below. Leaves the view "
          L"mode, zoom and window alone — ResetAll is the bigger hammer" },
        { L"GammaUp",               Command::GammaUp,                         PayloadRule::None,
          L"gamma one step up — brightens the midtones, leaves black and white put" },
        { L"GammaDown",             Command::GammaDown,                       PayloadRule::None,
          L"gamma one step down" },
        { L"BrightnessUp",          Command::BrightnessUp,                    PayloadRule::None,
          L"brightness one step up — lifts the whole range" },
        { L"BrightnessDown",        Command::BrightnessDown,                  PayloadRule::None,
          L"brightness one step down" },
        { L"ContrastUp",            Command::ContrastUp,                      PayloadRule::None,
          L"contrast one step up" },
        { L"ContrastDown",          Command::ContrastDown,                    PayloadRule::None,
          L"contrast one step down" },
        { L"SaturationUp",          Command::SaturationUp,                    PayloadRule::None,
          L"colour saturation one step up" },
        { L"SaturationDown",        Command::SaturationDown,                  PayloadRule::None,
          L"saturation one step down; far enough is grey" },

        // --- Sort order ---
        // TOGGLES, not setters: the command for the order you are already in flips
        // ascending/descending instead. Sending it twice from a different order is
        // therefore how a caller reaches "this order, descending".
        { L"SortByName",            Command::SortByName,                      PayloadRule::None,
          L"sort the playlist by NAME. Already sorted by name? Then this reverses it "
          L"— the sort commands are toggles" },
        { L"SortByDate",            Command::SortByDate,                      PayloadRule::None,
          L"sort by modified DATE; again to reverse" },
        { L"SortBySize",            Command::SortBySize,                      PayloadRule::None,
          L"sort by file SIZE; again to reverse" },
        { L"SortByType",            Command::SortByType,                      PayloadRule::None,
          L"sort by file TYPE (extension); again to reverse" },
        { L"SortByDisk",            Command::SortByDisk,                      PayloadRule::None,
          L"physical order on the drive — fastest to read, and REFUSED while a "
          L"connection is live: it reproduces on no other drive, so every shared "
          L"image number would land somewhere else" },

        // --- Wallpaper ---
        // All six set the desktop wallpaper of the machine that RECEIVES them, from
        // the image it is displaying. A write to that machine's settings, not to any
        // file of the user's.
        { L"SetWallpaperFill",      Command::SetWallpaperFill,                PayloadRule::None,
          L"set the current image as desktop wallpaper, FILL (crop to cover)" },
        { L"SetWallpaperFit",       Command::SetWallpaperFit,                 PayloadRule::None,
          L"as wallpaper, FIT — whole image, bars where it does not reach" },
        { L"SetWallpaperStretch",   Command::SetWallpaperStretch,             PayloadRule::None,
          L"as wallpaper, STRETCHED to the screen, aspect ratio not kept" },
        { L"SetWallpaperTile",      Command::SetWallpaperTile,                PayloadRule::None,
          L"as wallpaper, TILED at original size" },
        { L"SetWallpaperCenter",    Command::SetWallpaperCenter,              PayloadRule::None,
          L"as wallpaper, CENTRED at original size" },
        { L"SetWallpaperSpan",      Command::SetWallpaperSpan,                PayloadRule::None,
          L"as wallpaper, SPANNED across all monitors as one surface" },

        // --- Theme ---
        { L"ThemeFactorUp",         Command::ThemeFactorUp,                   PayloadRule::None,
          L"lighten the app's own background one step — the surround, not the image" },
        { L"ThemeFactorDown",       Command::ThemeFactorDown,                 PayloadRule::None,
          L"darken the app background one step" },
        { L"ThemeFactorReset",      Command::ThemeFactorReset,                PayloadRule::None,
          L"back to the default background shade" },

        // --- Clipboard ---
        { L"CopyToClipboard",       Command::CopyToClipboard,                 PayloadRule::None,
          L"copy the current image to the clipboard OF THAT MACHINE — which is why it "
          L"is never mirrored: a clipboard is per-machine and copying on a wall screen "
          L"achieves nothing" },

        // --- Application control ---
        // HideToTray and quit are the two a screen-management script actually
        // needs; both are safe because the tray icon remains the way back.
        { L"HideToTray",            Command::HideToTray,                      PayloadRule::None,
          L"hide that instance to the tray. The tray icon is the way back, which is "
          L"what makes this safe to send remotely" },
        { L"NewWindow",             Command::NewWindow,                       PayloadRule::None,
          L"launch ANOTHER viewer process on that machine, on the same folder" },
        { L"ResetAll",              Command::ResetAll,                        PayloadRule::None,
          L"reset everything: effects, adjustments, zoom, rotation, view mode AND the "
          L"window layout. ResetEffects is colour only; ResetWindowLayout geometry only" },
        { L"ResetWindowLayout",     Command::ResetWindowLayout,               PayloadRule::None,
          L"window back to its default size and position, leaving the image, the view "
          L"mode and every effect exactly as they are" },
        { L"HardQuit",              Command::HardQuit,                        PayloadRule::None,
          L"exit that instance IMMEDIATELY, without saving its session. Never "
          L"mirrored: one keypress must not close every screen at once" },

        // --- Window opacity ---
        { L"OpacityUp",             Command::OpacityUp,                       PayloadRule::None,
          L"make the window one step more opaque" },
        { L"OpacityDown",           Command::OpacityDown,                     PayloadRule::None,
          L"make the window one step more transparent — far enough and what is behind "
          L"it shows through" },

        // --- History walk, all rows (the horizontal-wheel scope) ---
        { L"PrevHistoryFolderAll",  Command::PrevHistoryFolderAll,            PayloadRule::None,
          L"previous folder across the WHOLE history, ignoring the recent/favourite "
          L"split that PrevHistoryFolder walks" },
        { L"NextHistoryFolderAll",  Command::NextHistoryFolderAll,            PayloadRule::None,
          L"next folder across the whole history" },

        // --- Mirroring / observing ---
        // `observe` is how a CALLER asks to be fed this instance's actions: it
        // adds the calling connection to the observer list, and `observe 0`
        // removes it. There is no way to nominate a THIRD party — an observer can
        // only ever be the connection that asked, which is what stops this from
        // being a way to make one screen shout at another.
        { L"Observe",               Command::Observe,                         PayloadRule::Required,
          L"ask the receiver to report what IT does back to the caller — how you watch "
          L"a screen running its own slideshow. The observer can only ever be the "
          L"connection that asked; there is no way to nominate a third party",
          L"1 = start reporting, 0 = stop.   e.g.  1" },
        // `sync` pushes the sender's whole view/effect state. Exists because
        // mirroring forwards TOGGLES: a toggle applied to a different starting
        // state diverges, and the effect CHAIN ORDER cannot be repaired by
        // resending toggles at all.
        { L"Sync",                  Command::Sync,                            PayloadRule::Required,
          L"adopt the sender's WHOLE view state: sort order, view mode, rotation, "
          L"flips, gamma/brightness/contrast/saturation, the effect list in order, "
          L"overlay, slideshow settings, and the folder. The cure for toggle drift — "
          L"and far heavier than Ctrl+Enter, which moves only the position",
          L"k=v;k=v… Unknown keys are ignored, so a newer sender can drive an older "
          L"receiver. Normally built by the console rather than typed.   e.g.  "
          L"view=1;rot=0;gamma=1.000;effects=none" },

        // --- Diagnostics ---
        // `enablelog 1` / `enablelog 0` — switch the Ctrl+F12 wire log on or off
        // on THIS instance. Payload-required rather than a toggle so a driving
        // instance can put a whole wall of screens into the same state; a toggle
        // sent to ends that already disagree only swaps which one is wrong.
        //
        // Reachable remotely on purpose: the screen whose behaviour you are
        // trying to explain is usually not the one you are sitting at.
        { L"EnableRemoteLog",       Command::EnableRemoteLog,                 PayloadRule::Required,
          L"switch the RECEIVER's wire log recording on or off. A value rather than a "
          L"toggle so a whole wall can be put into the same state; recording is off by "
          L"default and costs nothing while off",
          L"1 = start recording, 0 = stop.   e.g.  1" },

        // `msg <text>` — say something on the RECEIVER's screen. The console's
        // Identify button sends each target its own name, which is how you tell
        // two otherwise identical viewers apart.
        //
        { L"msgRemote",             Command::msgRemote,                       PayloadRule::Required,
          L"put a centre-screen message on the receiver — how a wall of identical "
          L"viewers is told apart. The F10 console's Identify button sends each target "
          L"its own name",
          L"the text to show, trimmed to 200 characters. It fades on that screen's "
          L"normal message timer.   e.g.  Monitor 2   ·   check this one" },
    };

    constexpr size_t TABLE_COUNT = sizeof(TABLE) / sizeof(TABLE[0]);

    // =========================================================================
    // ONE NAME PER COMMAND, and it is the enumerator's own spelling.
    //
    // The table used to carry a short alias beside the long one — `goto` for
    // JumpToImage, `quit` for HardQuit, `msg` for msgRemote. That is gone. A
    // command now has exactly one wire name, identical to its Command.h
    // enumerator, so there is one spelling to learn and no question about which
    // of two names a log line or a script is using.
    //
    // The one command that cannot follow the rule is JumpToImage: `goto` is a
    // C++ keyword, so `Command::goto` could never have existed. The long name is
    // the only name there, which is exactly the rule anyway.
    //
    // The two asserts below enforce it from the table alone:
    constexpr bool TableHasOneRowPerCommand() {
        for (size_t i = 0; i < TABLE_COUNT; ++i)
            for (size_t j = i + 1; j < TABLE_COUNT; ++j)
                if (TABLE[i].cmd == TABLE[j].cmd) return false;
        return true;
    }

    constexpr bool ConstEqualI(const wchar_t *a, const wchar_t *b) {
        for (;; ++a, ++b) {
            const wchar_t ca = (*a >= L'A' && *a <= L'Z') ? *a + 32 : *a;
            const wchar_t cb = (*b >= L'A' && *b <= L'Z') ? *b + 32 : *b;
            if (ca != cb) return false;
            if (ca == 0) return true;
        }
    }

    constexpr bool TableNamesAreUnique() {
        for (size_t i = 0; i < TABLE_COUNT; ++i)
            for (size_t j = i + 1; j < TABLE_COUNT; ++j)
                // Case-insensitive, because LookupCommand is: two rows differing
                // only in case would be one name with two meanings, and which
                // one won would depend on table order.
                if (ConstEqualI(TABLE[i].name, TABLE[j].name)) return false;
        return true;
    }

    static_assert(TableHasOneRowPerCommand(),
                  "two rows map to the same Command — aliases were removed on purpose, "
                  "give the command one name and make it the enumerator's spelling");
    static_assert(TableNamesAreUnique(),
                  "two rows share a wire name (lookup is case-insensitive), so which one "
                  "answers would depend on table order");

    // NOT checked, and it cannot be from here: that every remotely-reachable
    // Command HAS a row. Proving that needs a second list of what ought to be
    // reachable, which is the thing this table already is. A missing row means a
    // command is silently unreachable — an inconvenience. An extra row could
    // mean deleted files, and THAT is what the NEVER_REMOTE assert below covers.

    // =========================================================================
    // NEVER_REMOTE — commands that alter files.
    //
    // Not "not currently exposed". Not "we remembered to leave them out". These
    // must be impossible to drive over a socket, and the static_assert below
    // turns that from a convention into a build error: give any of them a row in
    // TABLE and the project stops compiling.
    //
    // Why the strength: authentication happens once, when the connection opens
    // (RemoteServer::Authenticate). Every line after that is trusted with no
    // per-message signature, so on a routable network an attacker who can inject
    // into an established session issues commands as the authenticated caller.
    // Better authentication would narrow that; keeping `delete` off the menu
    // entirely removes it.
    // =========================================================================
    constexpr Command NEVER_REMOTE[] = {
        Command::FileDeleteSelection,
        Command::FileMoveSelection,
        Command::FilePasteIntoFolder,
        Command::FileCopySelection,
        Command::SaveImage,
    };

    consteval bool NeverRemoteHasNoTableRow() {
        for (const Command c : NEVER_REMOTE)
            for (const CommandEntry &e : TABLE)
                if (e.cmd == c) return false;
        return true;
    }
    static_assert(NeverRemoteHasNoTableRow(),
                  "A file-altering command was given a row in the remote command "
                  "table. Remove it: these must never be reachable over a socket, "
                  "whatever the authentication state.");

    // =========================================================================
    // SESSION_BLOCKED — fine alone, unsafe while connected.
    //
    // Two failure modes, both silent without this: a command that changes the
    // FILE SET (every index after the change then points at a different
    // picture), and a command that means something different on each end.
    //
    // The reason travels with the entry because a keypress that quietly does
    // nothing is indistinguishable from a bug.
    // =========================================================================
    struct BlockedEntry {
        Command        cmd;
        const wchar_t *reason;
    };

    constexpr BlockedEntry SESSION_BLOCKED[] = {
        // Physical disk order is a property of the drive, so two instances both
        // "sorting by disk" genuinely produce different orders — and every index
        // exchanged after that lands on the wrong file.
        { Command::SortByDisk,
          L"physical disk order differs per drive" },

        // Searches this playlist only, so it lands somewhere the other end is
        // not. Navigation covers the same ground while connected.
        { Command::FindImage,
          L"searches this playlist only" },

        // The four that change what is in the folder. A file added or removed
        // shifts every index after it, which is exactly what the two ends are
        // relying on to stay aligned.
        { Command::FileDeleteSelection,
          L"changing the folder shifts every index after it" },
        { Command::FileMoveSelection,
          L"changing the folder shifts every index after it" },
        { Command::FilePasteIntoFolder,
          L"changing the folder shifts every index after it" },
        { Command::SaveImage,
          L"writing a file shifts every index after it" },

        // NOTE: FileCopySelection is deliberately absent. Copying to the
        // clipboard reads the file and writes nothing, so it changes no index
        // and there is no reason to take it away.
    };

    std::wstring TrimWs(const std::wstring &s) {
        size_t b = 0, e = s.size();
        while (b < e && ::iswspace(s[b])) ++b;
        while (e > b && ::iswspace(s[e - 1])) --e;
        return s.substr(b, e - b);
    }
}

const CommandEntry *CommandTable(size_t &countOut) {
    countOut = TABLE_COUNT;
    return TABLE;
}

bool LookupCommand(const std::wstring &name, const CommandEntry *&entryOut) {
    for (size_t i = 0; i < TABLE_COUNT; ++i) {
        if (_wcsicmp(TABLE[i].name, name.c_str()) == 0) {
            entryOut = &TABLE[i];
            return true;
        }
    }
    return false;
}

bool NameForCommand(Command cmd, std::wstring &nameOut) {
    // First row wins. The table lists the short alias before the canonical enum
    // name for exactly this reason — a mirrored session that a human may be
    // watching with netcat should read "next", not "NextImage".
    for (size_t i = 0; i < TABLE_COUNT; ++i) {
        if (TABLE[i].cmd == cmd) {
            nameOut = TABLE[i].name;
            return true;
        }
    }
    return false;
}

bool IsMirrorable(Command cmd) {
    switch (cmd) {
        // ── Never fanned out ────────────────────────────────────────────────
        // Each of these is reachable on purpose as a single deliberate act (the
        // F9 Send box, a script) and wrong as a broadcast.

        // Would end or hide every connected screen at once.
        case Command::HardQuit:
        case Command::HideToTray:
        case Command::NewWindow:
            return false;

        // Opens Explorer on a machine nobody is sitting at.
        case Command::ShowInExplorer:
            return false;

        // Writes files / raises a save dialog.
        case Command::SaveImage:
            return false;

        // Clipboard is per-machine; copying on a wall screen achieves nothing.
        case Command::CopyToClipboard:
            return false;

        // Panels raise a window on the far screen that then has to be closed
        // from here. The driving instance wants its OWN panels, not theirs.
        case Command::ToggleHelp:
        case Command::ToggleHistory:
        case Command::ToggleHistoryFull:
        case Command::ToggleCache:
        case Command::ToggleDir:
        case Command::ShowInfo:
        case Command::ToggleStats:
        case Command::FindImage:
        case Command::JumpToImage:
        case Command::ZoomTo:
        case Command::CloseAllPanels:
        case Command::RestoreAllPanels:
        case Command::ToggleAllPanels:
        case Command::ToggleDedicatedPanel:
        case Command::ToggleRemotePanel:
        case Command::ToggleRemotesConsole:
            return false;

        // Geometry. Slaves are placed on fixed monitors, usually fullscreen;
        // forwarding a snap or a move knocks one off the screen it was put on.
        case Command::SnapLeft:      case Command::SnapRight:
        case Command::SnapTop:       case Command::SnapBottom:
        case Command::SnapTopLeft:   case Command::SnapTopRight:
        case Command::SnapBottomLeft:case Command::SnapBottomRight:
        case Command::MoveWindowLeft:case Command::MoveWindowRight:
        case Command::MoveWindowUp:  case Command::MoveWindowDown:
        case Command::ResizeWindowLarger:
        case Command::ResizeWindowSmaller:
        case Command::AutosizeToWorkArea:
        case Command::ResetWindowLayout:
        case Command::ResetAll:
            return false;

        // Configuration and identity — a dedicated instance's whole point is
        // that these are ITS OWN and nobody else writes them.
        case Command::ToggleDedicated:
        case Command::CmdArgsExport:
        case Command::CmdArgsImport:
        case Command::CmdArgsGenerateShortcut:
        case Command::CmdArgsTest:
            return false;

        // The mirroring controls themselves. Forwarding MirrorToggle would have
        // every slave start mirroring to ITS targets — and where two of them
        // point at each other, that is an infinite exchange. Observe/Sync are
        // remote-only verbs and are never something a local keypress fans out.
        case Command::MirrorToggle:
        case Command::MirrorPick:
        case Command::MirrorLocalToggle:
        case Command::Observe:
        case Command::Sync:
            return false;

        // Fanned out by BroadcastEnableLog, which sends the STATE (`enablelog 1`)
        // — not by the generic mirror path, which would send the bare verb and
        // have every target reject it for a missing payload. Excluded here so
        // the two routes cannot both fire and send it twice.
        case Command::EnableRemoteLog:
            return false;

        // Aimed, never fanned out. `msg` is sent to ONE target on purpose — the
        // Identify button gives each screen a DIFFERENT text, its own name, so a
        // broadcast would be meaningless. And a slave that re-forwarded it to
        // its own targets would put one instance's name on somebody else's
        // screen.
        case Command::msgRemote:
            return false;

        // Asked, never fanned out. QueryState is how a push INTERROGATES one
        // named target; broadcasting it would query every screen and throw the
        // answers away. The three driving commands are the acts themselves — a
        // keystroke fanned out to every screen would have each of them drive its
        // own targets, which is not what the key means. A slave told to
        // perform it would push its own picture back at whoever asked.
        case Command::QueryState:
        case Command::QueryHistory:
        case Command::QueryClients:
        case Command::SendImagePositionToRemotes:
        case Command::StreamImageToRemotes:
        case Command::StreamImageFromRemote:
            return false;

        // The one-shot display commands. Each is sent EXPLICITLY, by a streaming
        // job that does its own per-target work; through the generic mirror path
        // `ShowImageOnce` would carry a path to machines where it means nothing,
        // and a chunk fanned out to every screen would assemble half a transfer on
        // each of them. A slave that re-forwarded any of them would also stream
        // into ITS targets.
        case Command::ShowImageOnce:
        case Command::StreamImageBegin:
        case Command::StreamImageChunk:
        case Command::StreamImageShow:
        case Command::SendDisplayedImage:
            return false;

        default:
            break;
    }

    // Everything else mirrors — but only if it is on the wire at all. A command
    // with no table row has no name to send, so reachability is still the outer
    // bound: this function narrows that set, it never widens it.
    std::wstring unused;
    return NameForCommand(cmd, unused);
}

bool IsMirrorableRemote(Command cmd) {
    if (!IsMirrorable(cmd)) return false;

    switch (cmd) {
        // Disk order is a property of the drive. Both ends "sort by disk" and
        // arrive at different orders, which then quietly poisons every index.
        // Also refused locally while connected — this is the belt to that brace.
        case Command::SortByDisk:
            return false;

        // Nothing else needs excluding HERE, because the two things that do not
        // survive the trip are not commands: the `goto <n>` that LoadImageIndex
        // emits, and the `folder=` field inside a `sync`. Both are filtered
        // where they are produced, since a bare Command carries neither.
        default:
            return true;
    }
}

bool IsNeverRemote(Command cmd) {
    for (const Command c : NEVER_REMOTE)
        if (c == cmd) return true;
    return false;
}

bool BlockedNow(Command cmd, const wchar_t *&reasonOut) {
    if (!Mirror::SessionActive()) return false;
    return IsBlockedInSession(cmd, reasonOut);
}

bool IsBlockedInSession(Command cmd, const wchar_t *&reasonOut) {
    // Linear scan, deliberately. The table is a handful of entries that fit in
    // a cache line; hashing one Command costs more than comparing all of them,
    // and this runs once per keypress.
    for (const BlockedEntry &e : SESSION_BLOCKED) {
        if (e.cmd == cmd) {
            reasonOut = e.reason;
            return true;
        }
    }
    return false;
}

RemoteRequest ParseLine(const std::wstring &line) {
    RemoteRequest req;

    // Bound first, before any other work. An unbounded line is the one input a
    // socket fully controls, so it is rejected on length alone.
    if (line.size() > RT::MAX_LINE_LEN) {
        req.status = ParseStatus::LineTooLong;
        return req;
    }

    // Tolerate CRLF and stray indentation — telnet and a hand-typed session both
    // arrive slightly untidy, and rejecting that would be pointless pedantry.
    const std::wstring trimmed = TrimWs(line);
    if (trimmed.empty()) {
        req.status = ParseStatus::EmptyLine;
        return req;
    }

    // Split on the FIRST run of whitespace only: everything after it is payload,
    // verbatim. A path with spaces must survive intact and must not be quoted,
    // because quoting would then have to be unquoted and paths legitimately
    // contain quote characters on Windows.
    const size_t sp = trimmed.find_first_of(L" \t");
    std::wstring name, payload;
    if (sp == std::wstring::npos) {
        name = trimmed;
    } else {
        name    = trimmed.substr(0, sp);
        payload = TrimWs(trimmed.substr(sp + 1));
    }

    req.rawName = name;

    // Protocol verbs are checked before the command table. They answer from the
    // socket thread without ever reaching the viewer, which is what makes `ping`
    // a true liveness check: a reply proves the listener is up, and says nothing
    // about whether the UI thread happens to be busy.
    if (_wcsicmp(name.c_str(), L"help") == 0 ||
        _wcsicmp(name.c_str(), L"?")    == 0) {
        req.verb   = Verb::Help;
        req.status = ParseStatus::Verb;
        return req;
    }
    if (_wcsicmp(name.c_str(), L"ping") == 0) {
        req.verb   = Verb::Ping;
        req.status = ParseStatus::Verb;
        return req;
    }
    if (_wcsicmp(name.c_str(), L"version") == 0) {
        req.verb   = Verb::Version;
        req.status = ParseStatus::Verb;
        return req;
    }

    const CommandEntry *entry = nullptr;
    if (!LookupCommand(name, entry)) {
        req.status = ParseStatus::UnknownCommand;
        return req;
    }

    if (entry->payload == PayloadRule::Required && payload.empty()) {
        req.status = ParseStatus::PayloadRequired;
        return req;
    }
    // A payload on a command that takes none is an error rather than something
    // to discard: silently ignoring it would hide a typo that the sender
    // believes is doing something.
    if (entry->payload == PayloadRule::None && !payload.empty()) {
        req.status = ParseStatus::PayloadRejected;
        return req;
    }

    req.cmd     = entry->cmd;
    req.payload = payload;
    req.status  = ParseStatus::Ok;
    return req;
}

// --- Responses -------------------------------------------------------------

std::wstring MakeOk() { return RT::RESP_OK; }

std::wstring MakeOk(const std::wstring &text) {
    if (text.empty()) return MakeOk();
    return std::wstring(RT::RESP_OK) + L" " + text;
}

std::wstring MakeErr(int code, const std::wstring &reason) {
    return std::wstring(RT::RESP_ERR) + L" " + std::to_wstring(code) + L" " + reason;
}

std::wstring MakeErrFor(const RemoteRequest &req) {
    switch (req.status) {
        case ParseStatus::UnknownCommand:
            return MakeErr(RT::ERR_UNKNOWN_COMMAND,
                           L"unknown command '" + req.rawName + L"' - send 'help' for the list");
        case ParseStatus::PayloadRequired:
            return MakeErr(RT::ERR_PAYLOAD_REQUIRED,
                           L"'" + req.rawName + L"' requires a value");
        case ParseStatus::PayloadRejected:
            return MakeErr(RT::ERR_PAYLOAD_REJECTED,
                           L"'" + req.rawName + L"' takes no value");
        case ParseStatus::LineTooLong:
            return MakeErr(RT::ERR_LINE_TOO_LONG, L"line too long");
        default:
            return MakeErr(RT::ERR_INTERNAL, L"unhandled parse state");
    }
}

std::wstring BuildHelpText() {
    // Built from the same table the parser uses, so the listing can never drift
    // from what is actually accepted.
    //
    // MACHINE-READABLE, not just human-readable. This is the one call a foreign
    // client — the phone app — makes to learn what this instance accepts, so it
    // carries the DESCRIPTIONS too and in a shape that parses without guessing:
    //
    //   CMD <name>|<0|1 takes a value>|<what it does>|<what the value means>
    //
    // Pipe-separated because a description contains spaces and commas and a name
    // never contains a pipe; four fields always, empty where there is nothing to
    // say, so a reader can split on '|' and stop thinking. A client that only wants
    // names reads field 1 and ignores the rest.
    //
    // The alternative was every client hardcoding its own copy of a ninety-row
    // table, which would be wrong the first time a command was added here.
    std::wstring out = L"qIV remote protocol v" +
                       std::to_wstring(RT::PROTOCOL_VERSION) + L"\r\n";
    out += L"VERB help|this listing\r\n";
    out += L"VERB ping|liveness check, executes nothing\r\n";
    out += L"VERB version|app and protocol version\r\n";
    out += L"FORMAT CMD <name>|<takesValue 0|1>|<description>|<value description>\r\n";

    // A '|' inside a description would silently give a reader a fifth field. No
    // row contains one today; this makes sure the format survives one that does.
    auto field = [](const wchar_t *s) {
        std::wstring v = s ? s : L"";
        std::replace(v.begin(), v.end(), L'|', L'/');
        return v;
    };

    for (size_t i = 0; i < TABLE_COUNT; ++i) {
        const CommandEntry &e = TABLE[i];
        out += L"CMD ";
        out += e.name;
        out += (e.payload == PayloadRule::Required) ? L"|1|" : L"|0|";
        out += field(e.desc);
        out += L"|";
        out += field(e.valueDesc);
        out += L"\r\n";
    }
    return out;
}

// --- Encoding ---------------------------------------------------------------

std::string ToUtf8(const std::wstring &s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring FromUtf8(const char *data, size_t len) {
    if (!data || len == 0) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, data, static_cast<int>(len), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, data, static_cast<int>(len), out.data(), n);
    return out;
}

} // namespace Remote
