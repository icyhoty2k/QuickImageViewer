// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

// AppCommands.h
#pragma once
#include <windows.h>
#include <string>
#include <vector>
#define WM_TRAYICON (WM_APP + 1)
#define ID_TRAY_APP_ICON 1001

class AppCommands {
    public:
        static void ToggleFullscreen(HWND hWnd);

        static void ResetWindowLayoutAndEffects(HWND hWnd);

        static void AddTrayIcon(HWND hWnd);

        // Re-arms or releases the "keep the display on" request from
        // app.keepDisplayAwake. THE single place SetThreadExecutionState is
        // called: the flag is per-thread and cumulative, so scattering the call
        // would leave requests armed that nothing can clear. Call it after the
        // setting changes and whenever the main window's visibility changes;
        // it is idempotent, so calling it too often costs nothing.
        // UI thread only.
        static void ApplyDisplayAwake(HWND hWnd);

        static void changeAppThemeToDarkMode(HWND hWnd, bool isDarkThemed);
        static void changeAppCornerPreference(HWND hWnd, DWORD cornerStyle);
        static void changeAppThemeFactor(HWND hWnd, float newFactor);
        static void changeAppBackdropType(HWND hWnd, DWORD newType);

        // Slideshow
        static void toggleSlideshow(HWND hWnd);      // Ctrl+F1: start / stop
        static void pauseResumeSlideshow(HWND hWnd); // Space: pause / resume
        static void stopSlideshow(HWND hWnd);        // also called from WM_TIMER (end of playlist)

        // THE ONLY SUPPORTED WAY TO CHANGE THE SLIDE INTERVAL.
        //
        // Writing app.slideshow.intervalMs on its own does nothing to a running
        // show. SetTimer is PERIODIC: the timer armed when the slideshow started
        // keeps firing at the period it was given, and the advance path does not
        // re-arm it. So a new interval sat in the struct, was saved to the
        // registry, and changed nothing until the user stopped and restarted —
        // which is precisely how it was reported from the phone.
        //
        // Four sites had the same defect independently: the keyboard/menu
        // prompt, the numeric settings entry, the remote SlideshowSetInterval
        // command, and the mirroring Sync payload. They all call this now.
        //
        // The caller still owns PERSISTENCE. Not every one of them should write
        // the registry — a Sync payload is another instance's state arriving,
        // not this user choosing a value — so saving stays where the intent is
        // known.
        static void applySlideshowInterval(HWND hWnd, int ms);

        static void RemoveTrayIcon(HWND hWnd);

        // Opens the folder named by the blank-screen placeholder in Explorer,
        // walking up to the nearest parent that still exists when the folder
        // itself is gone (the Missing state).
        //
        // Shared because the placeholder offers TWO ways to do it — clicking
        // the line and pressing L — and they have to behave identically. The
        // keyboard path in particular cannot go through the normal
        // ShowInExplorer command: that one selects the CURRENT IMAGE, and while
        // this placeholder is up there is no current image by definition.
        // Returns false when there is nothing to open.
        static bool OpenOverlayFolderInExplorer(HWND hWnd);

        // File clipboard / shell operations (used by thumbnail panel context menu)
        static void CopyFileToClipboard(HWND hWnd, const std::wstring &path, bool cut = false);
        static void CopyFilesToClipboard(HWND hWnd, const std::vector<std::wstring> &paths, bool cut = false);
        static void DeleteFileToRecycleBin(const std::wstring &path);
        static void DeleteFilesToRecycleBin(const std::vector<std::wstring> &paths);
        static void PasteFilesFromClipboard(HWND hWnd, const std::wstring &targetDir);
        static bool ClipboardHasFiles();

        // Put plain text on the clipboard. Returns false when the clipboard
        // could not be opened — another process holds it, which is ordinary and
        // transient — so the caller can say so instead of appearing to succeed.
        //
        // ONE copy of this. The same fifteen lines had been written three times
        // (ExifWnd, RemoteLogWnd, LinkText) and the three did not agree: two
        // freed the handle when SetClipboardData failed and one leaked it, and
        // only one of them reported anything back. Three transcriptions of a
        // Win32 sequence is three chances to get the ownership rule wrong.
        static bool CopyTextToClipboard(HWND hWnd, const std::wstring &text);

        // ── What file is the picture on screen? ───────────────────────────────
        //
        // NOT app.playlist[app.currentIndex]. Every command that has to name the
        // current image on disk asks THIS, because the index is wrong in two
        // ways that are invisible at the call site:
        //
        //  * An interjection does not move the index. ArmInterjection is
        //    explicit that "nothing here touches app.playlist, app.currentIndex
        //    or the sort order", so while one is showing the index still names
        //    the picture it is COVERING.
        //  * A picture streamed in from the phone or another desktop instance is
        //    a temp file this process wrote and will DELETE at the next change
        //    of image (ownsTempFile). Any path handed out is dead on arrival.
        //
        // One helper rather than the same two checks at each call site: a caller
        // that forgets them gets a confident wrong answer, not a crash, which is
        // the failure shape this codebase is worst at noticing.
        enum class CurrentImage { Ok, None, Streamed };
        static CurrentImage GetCurrentImagePath(std::wstring &pathOut);

        // Raises Windows' own "Open with" chooser on one named file. Public
        // because the thumbnail strips call it on the thumbnail under the
        // cursor, which they already know and which never needs the guard above
        // — a thumbnail exists because a file on this disk was enumerated.
        //
        // ONE file, not a list: SHOpenWithDialog takes a single pcszFile and
        // there is no multi-file form of it.
        static bool OpenPathWith(HWND hWnd, const std::wstring &path);

    private:
        // This remains private and inaccessible to the rest of the app
        static void SaveImageToDisk(HWND hWnd);
        static void CopyImageToClipboard(HWND hWnd);
        // The current image's full path, as text. Private beside its sibling
        // above for the same reason: both read app.playlist / app.currentIndex,
        // and "which image is current" is the InputManager's question to ask.
        static void CopyImagePathToClipboard(HWND hWnd);
        // Raises Windows' own "Open with" chooser on the current image, so the
        // obvious next thing after looking at a picture — editing it — does not
        // mean finding it in Explorer first.
        static void OpenCurrentImageWith(HWND hWnd);

        // Sets the currently displayed file as the desktop wallpaper.
        // position: Constants::Wallpaper::FILL .. SPAN — mapped onto the native
        // DESKTOP_WALLPAPER_POSITION enum inside the .cpp so this header stays
        // free of <shobjidl.h>.
        static void SetDesktopWallpaper(HWND hWnd, int position);


        // Only the InputManager class can call the method above
        friend class InputManager;
};
