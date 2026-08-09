// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#include "RemoteExec.h"
#include "RemoteInbound.h" // InboundSource — which connection asked to observe
#include "RemoteServer.h"  // Add/RemoveObserver
#include "RemoteLog.h"     // enablelog — the recording switch, both halves

#include "AppState.h"
#include "Platform/Constants.h"
#include "Platform/ConstantsStrings.h"
#include "Platform/FileHandler.h"
#include "Input/AppCommands.h"   // applySlideshowInterval — re-arms a running show
#include "Persistence/RegistryManager.h"
#include "Overlays/OverlayManager.h"
#include "Common/FuzzyMatch.h"
#include "Common/Converters.h"
#include "Common/Base64.h"      // StreamImageChunk — decoding one chunk
#include "RemoteImageXfer.h"    // the shared transfer rules, both directions

#include <algorithm>
#include <cstring>    // _wcsicmp — file names compared case-insensitively
#include <filesystem>

extern AppState app;
extern OverlayManager g_overlayManager;

namespace Remote {

namespace RT = Constants::RemoteTcpIp;
namespace fs = std::filesystem;

namespace {

    std::wstring ToLower(const std::wstring &s) {
        std::wstring out = s;
        std::transform(out.begin(), out.end(), out.begin(), ::towlower);
        return out;
    }

    // Strict integer parse. _wtoi silently yields 0 for garbage, which would
    // turn "goto abc" into a jump rather than an error.
    bool ParseInt(const std::wstring &s, int &out) {
        if (s.empty()) return false;
        size_t i = 0;
        bool neg = false;
        if (s[0] == L'-' || s[0] == L'+') { neg = (s[0] == L'-'); i = 1; }
        if (i >= s.size()) return false;

        long long acc = 0;
        for (; i < s.size(); ++i) {
            if (s[i] < L'0' || s[i] > L'9') return false;
            acc = acc * 10 + (s[i] - L'0');
            if (acc > 2147483647LL) return false; // refuse rather than wrap
        }
        out = static_cast<int>(neg ? -acc : acc);
        return true;
    }

    // Accepts "150", "150.5" and "150%" — a human typing at a socket will use
    // all three, and rejecting the percent sign would be pointless pedantry.
    bool ParseFloatPercent(const std::wstring &raw, float &out) {
        std::wstring s = raw;
        if (!s.empty() && s.back() == L'%') s.pop_back();
        if (s.empty()) return false;

        size_t i = 0;
        if (s[0] == L'+') i = 1;
        bool sawDigit = false, sawDot = false;
        for (size_t j = i; j < s.size(); ++j) {
            if (s[j] == L'.') {
                if (sawDot) return false;
                sawDot = true;
            } else if (s[j] >= L'0' && s[j] <= L'9') {
                sawDigit = true;
            } else {
                return false;
            }
        }
        if (!sawDigit) return false;

        try {
            out = std::stof(s.substr(i));
        } catch (...) {
            return false;
        }
        return out >= 0.0f;
    }

    // --- goto <n> -----------------------------------------------------------
    // 1-BASED, matching JumpToWnd exactly (it calls LoadImageIndex(number - 1)).
    // The overlay and the JumpTo panel both show 1-based numbers, so a remote
    // caller reading the screen and sending what it sees must land on that image.
    std::wstring DoJump(HWND hWnd, const std::wstring &payload) {
        int n = 0;
        if (!ParseInt(payload, n))
            return MakeErr(RT::ERR_BAD_PAYLOAD, L"expected an image number");

        const int total = static_cast<int>(app.playlist.size());
        if (total <= 0)
            return MakeErr(RT::ERR_BAD_PAYLOAD, L"playlist is empty");
        if (n < 1 || n > total)
            return MakeErr(RT::ERR_BAD_PAYLOAD,
                           L"out of range 1-" + std::to_wstring(total));

        LoadImageIndex(hWnd, n - 1);
        InvalidateRect(hWnd, nullptr, FALSE);
        return MakeOk(std::to_wstring(n) + L"/" + std::to_wstring(total));
    }

    // --- open <path> --------------------------------------------------------
    // A file opens that image; a folder opens the folder. Both are READ paths —
    // nothing here writes, deletes or moves, so the worst a permitted caller can
    // do is make the screen show a different picture.
    std::wstring DoOpen(HWND hWnd, const std::wstring &payload) {
        // Tolerate a quoted path: "Copy as path" in Explorer wraps in quotes and
        // people paste it straight into a socket session.
        std::wstring path = payload;
        if (path.size() >= 2 && path.front() == L'"' && path.back() == L'"')
            path = path.substr(1, path.size() - 2);
        if (path.empty())
            return MakeErr(RT::ERR_BAD_PAYLOAD, L"expected a path");

        std::error_code ec;
        const fs::path p(path);

        if (fs::is_directory(p, ec) && !ec) {
            OpenDirectory(hWnd, path);
            return MakeOk(L"folder opened");
        }
        if (fs::is_regular_file(p, ec) && !ec) {
            if (!is_image_ext(p.extension().wstring()))
                return MakeErr(RT::ERR_BAD_PAYLOAD, L"not a supported image type");
            OpenSpecificImage(hWnd, path);
            return MakeOk(L"image opened");
        }
        return MakeErr(RT::ERR_BAD_PAYLOAD, L"no such file or folder");
    }

    // --- ShowImageOnce <path> ------------------------------------------------
    // Show ONE image, once, and change nothing else. The path form: no transfer,
    // so it costs nothing — and it only works where the far end can read the path.
    // For another machine the caller streams the bytes instead (below).
    //
    // Not a lighter `open`: `open` joins the file to this viewer — it rebuilds the
    // playlist, moves the index, and the folder becomes where this instance lives.
    // This does none of that. The picture goes up in place of the current slide,
    // the playlist and index never move, and the next image change of any kind
    // retires it (LoadImageIndex). A slideshow therefore carries on afterwards
    // from exactly where it was, as if nothing had happened — which is the point:
    // an advert between two slides.
    //
    // WHEN it appears is decided here:
    //   slideshow running → the next SLIDE BOUNDARY, so the slide currently on
    //                       screen is not cut short (AppMain's timer).
    //   otherwise         → now, or the instant its decode lands.
    //
    // A read path, like `open`: nothing is written, moved or deleted, so the worst
    // a permitted caller can do is put a picture on the screen for one slide.
    std::wstring DoInterject(HWND hWnd, const std::wstring &payload) {
        std::wstring path = payload;
        if (path.size() >= 2 && path.front() == L'"' && path.back() == L'"')
            path = path.substr(1, path.size() - 2);
        if (path.empty())
            return MakeErr(RT::ERR_BAD_PAYLOAD, L"expected a path");

        std::error_code ec;
        const fs::path p(path);
        if (!fs::is_regular_file(p, ec) || ec)
            return MakeErr(RT::ERR_BAD_PAYLOAD, L"no such file");
        if (!is_image_ext(p.extension().wstring()))
            return MakeErr(RT::ERR_BAD_PAYLOAD, L"not a supported image type");
        // A FOLDER is refused rather than interpreted: `open` takes either, but
        // "show this one image once" has no reading for a folder.

        // Not a temp file: it belongs to the user, so retiring the interjection
        // must not delete it.
        if (ArmInterjection(hWnd, path, /*immediate=*/false, /*ownsTempFile=*/false))
            return MakeOk(L"shown");
        return MakeOk(app.slideshow.running && !app.slideshow.paused
                          ? L"queued for the next slide"
                          : L"decoding");
    }

    // --- The image STREAM (StreamImageBegin / Chunk / Show) -----------------
    //
    // The same one-shot display as above, for a caller that cannot name a file
    // this machine can read — another machine, or a phone. The picture's own file
    // bytes arrive base64-encoded across several commands and are assembled here.
    //
    // UI THREAD ONLY, like every other command handler, so this buffer needs no
    // lock. One transfer at a time per instance: a second StreamImageBegin
    // abandons the first, which is the only sane reading of "start a transfer"
    // and stops a caller that died mid-stream from wedging the next one.
    struct InboundStream {
        std::wstring fileName;
        size_t       declared = 0;   // bytes the sender promised
        std::vector<unsigned char> bytes;
    };
    InboundStream g_inStream;

    // `StreamImageBegin <totalBytes> <fileName>`
    std::wstring DoStreamBegin(const std::wstring &payload) {
        const size_t sp = payload.find(L' ');
        if (sp == std::wstring::npos)
            return MakeErr(RT::ERR_BAD_PAYLOAD, L"expected: <totalBytes> <fileName>");

        int declared = 0;
        if (!ParseInt(payload.substr(0, sp), declared) || declared <= 0)
            return MakeErr(RT::ERR_BAD_PAYLOAD, L"byte count must be a positive number");
        if (static_cast<size_t>(declared) > RT::STREAM_MAX_BYTES)
            return MakeErr(RT::ERR_BAD_PAYLOAD, Constants::Messages::STREAM_ERR_TOO_LARGE);

        std::wstring name = payload.substr(sp + 1);
        if (name.empty())
            return MakeErr(RT::ERR_BAD_PAYLOAD, L"expected a file name");
        // A NAME, never a path. Only its extension matters — it selects the
        // decoder — and a caller that sent a path must not have any of it used as
        // one. Stripped rather than refused: the extension is still good.
        name = fs::path(name).filename().wstring();

        g_inStream = InboundStream{};
        g_inStream.fileName = name;
        g_inStream.declared = static_cast<size_t>(declared);
        // Reserved once, from the declared size, so a large transfer does not
        // reallocate its way up in a dozen steps.
        g_inStream.bytes.reserve(g_inStream.declared);
        return MakeOk(std::to_wstring(declared));
    }

    // `StreamImageChunk <base64>`
    std::wstring DoStreamChunk(const std::wstring &payload) {
        if (g_inStream.declared == 0)
            return MakeErr(RT::ERR_BAD_PAYLOAD, Constants::Messages::STREAM_ERR_NO_TRANSFER);
        if (payload.empty())
            return MakeErr(RT::ERR_BAD_PAYLOAD, L"expected base64 data");

        std::vector<unsigned char> part;
        if (!Common::Base64::Decode(payload.c_str(), payload.size(), part))
            return MakeErr(RT::ERR_BAD_PAYLOAD, L"malformed base64");

        // Refused the moment it exceeds what was promised, rather than at Show:
        // the declared size is the only bound on this buffer, so it has to be
        // enforced on every chunk or it is not a bound at all.
        if (g_inStream.bytes.size() + part.size() > g_inStream.declared) {
            g_inStream = InboundStream{};
            return MakeErr(RT::ERR_BAD_PAYLOAD, L"more bytes than declared");
        }

        g_inStream.bytes.insert(g_inStream.bytes.end(), part.begin(), part.end());
        return MakeOk(std::to_wstring(g_inStream.bytes.size()) + L"/" +
                      std::to_wstring(g_inStream.declared));
    }

    // `StreamImageShow` — assemble and display, once.
    std::wstring DoStreamShow(HWND hWnd) {
        if (g_inStream.declared == 0)
            return MakeErr(RT::ERR_BAD_PAYLOAD, Constants::Messages::STREAM_ERR_NO_TRANSFER);

        // Proven, not assumed. A phone on a flaky link is the expected caller, and
        // half a JPEG decodes to a torn image rather than to an error.
        if (g_inStream.bytes.size() != g_inStream.declared) {
            const std::wstring got = std::to_wstring(g_inStream.bytes.size()) + L"/" +
                                     std::to_wstring(g_inStream.declared);
            g_inStream = InboundStream{};
            return MakeErr(RT::ERR_BAD_PAYLOAD,
                           std::wstring(Constants::Messages::STREAM_ERR_TRUNCATED) +
                               L" (" + got + L")");
        }

        // Written to a temp file so the ordinary decode/cache/display path can be
        // reused unchanged — and marked as ours, so retiring the interjection
        // deletes it.
        std::wstring tempPath;
        if (!Xfer::WriteTempImage(g_inStream.fileName, g_inStream.bytes, tempPath)) {
            g_inStream = InboundStream{};
            return MakeErr(RT::ERR_INTERNAL, L"could not write the received image");
        }

        const std::wstring name = g_inStream.fileName;
        const size_t       n    = g_inStream.bytes.size();
        // The buffer is released as soon as the file exists: holding the bytes AND
        // the decoded bitmap AND the file would be three copies of one picture.
        g_inStream = InboundStream{};

        const bool shown = ArmInterjection(hWnd, tempPath, /*immediate=*/false,
                                           /*ownsTempFile=*/true);
        return MakeOk(std::to_wstring(n) + L";" + name + L";" +
                      (shown ? L"shown" : L"queued"));
    }

    // `SendDisplayedImage` — the INBOUND direction, answered in one reply.
    //
    // Body lines first, then the OK that terminates them and names the size and
    // file. A reply may be multi-line (the `help` verb already is), so this needs
    // none of the framing the outbound direction does.
    //
    // Reads the file ON THE UI THREAD, which is the one blocking call in this
    // feature. Accepted deliberately: it is bounded by STREAM_MAX_BYTES, it
    // happens only when somebody explicitly asks, and the alternative — handing
    // the path to the socket thread — breaks the one-request-one-reply shape that
    // makes the protocol implementable by a phone in fifty lines.
    // The path of what is on screen, or empty. UI THREAD ONLY — it reads
    // app.playlist and app.interject.
    std::wstring DisplayedPath() {
        if (app.interject.showing && !app.interject.path.empty())
            return app.interject.path;
        if (app.currentIndex >= 0 &&
            app.currentIndex < static_cast<int>(app.playlist.size()))
            return app.playlist[app.currentIndex];
        return {};
    }

    // DoSendDisplayedImage USED TO LIVE HERE and is deliberately gone.
    //
    // It read the whole displayed file and base64-encoded it on the UI thread,
    // which is where this dispatch runs. That body now lives in ClientThread
    // (RemoteServer.cpp), reached through ORIGINAL_PATH_MARKER, so the bytes are
    // produced on the socket thread instead. The path selection it did by hand
    // is what DisplayedPath() above already returns, identically.
    //
    // Left as a note rather than deleted silently: a future reader looking for
    // the handler should find out where it went, not conclude the command is
    // unimplemented.

    // --- find <query> -------------------------------------------------------
    // Mirrors FindWnd's matching rules: a query containing * or ? is a wildcard
    // match, anything else is a fuzzy match, both case-insensitive against the
    // FILE NAME rather than the full path. Unlike the panel, which lists every
    // hit for a human to choose from, this jumps straight to the best one — a
    // socket client has nobody to present a list to.
    std::wstring DoFind(HWND hWnd, const std::wstring &payload) {
        const std::wstring q = ToLower(payload);
        const int qLen = static_cast<int>(q.size());
        if (qLen <= 0 || qLen >= Common::FUZZY_MAX_QUERY)
            return MakeErr(RT::ERR_BAD_PAYLOAD, L"query too short or too long");
        if (app.playlist.empty())
            return MakeErr(RT::ERR_BAD_PAYLOAD, L"playlist is empty");

        const bool wildcard = Common::IsWildcardQuery(q.c_str(), qLen);

        int bestIdx   = -1;
        int bestScore = 0;

        for (size_t i = 0; i < app.playlist.size(); ++i) {
            const std::wstring &full = app.playlist[i];
            const size_t slash = full.find_last_of(L"\\/");
            const std::wstring name =
                ToLower(slash == std::wstring::npos ? full : full.substr(slash + 1));

            if (wildcard) {
                if (Common::WildcardMatch(q.c_str(), name.c_str())) {
                    bestIdx = static_cast<int>(i);
                    break; // wildcard hits are unranked — first match wins
                }
                continue;
            }

            Common::FuzzyMatchResult fm;
            if (!Common::FuzzyMatch(q.c_str(), qLen, name.c_str(),
                                    static_cast<int>(name.size()), fm))
                continue;
            if (bestIdx < 0 || fm.score > bestScore) {
                bestIdx   = static_cast<int>(i);
                bestScore = fm.score;
            }
        }

        if (bestIdx < 0)
            return MakeErr(RT::ERR_BAD_PAYLOAD, L"no match");

        LoadImageIndex(hWnd, bestIdx);
        InvalidateRect(hWnd, nullptr, FALSE);

        const std::wstring &hit = app.playlist[bestIdx];
        const size_t slash = hit.find_last_of(L"\\/");
        return MakeOk(std::to_wstring(bestIdx + 1) + L" " +
                      (slash == std::wstring::npos ? hit : hit.substr(slash + 1)));
    }

    // --- enablelog <0|1> ----------------------------------------------------
    // Switch the Ctrl+F12 wire log on or off HERE. Reachable remotely because
    // the screen whose behaviour needs explaining is usually not the one you are
    // sitting at.
    //
    // Sets both halves of the flag: the UI-thread bool the menu and panel read,
    // and the atomic the socket/sender threads actually gate on. Setting one
    // without the other gives a viewer whose panel and whose recording disagree.
    //
    // Does NOT fan out further. This is the RECEIVING end of a fan-out that
    // BroadcastEnableLog already did; re-broadcasting would have every slave
    // instruct its own targets, and two instances pointed at each other would
    // trade the command forever.
    std::wstring DoEnableLog(const std::wstring &payload) {
        bool on = false;
        if (payload == L"1" || _wcsicmp(payload.c_str(), L"on") == 0 ||
            _wcsicmp(payload.c_str(), L"true") == 0) {
            on = true;
        } else if (payload == L"0" || _wcsicmp(payload.c_str(), L"off") == 0 ||
                   _wcsicmp(payload.c_str(), L"false") == 0) {
            on = false;
        } else {
            return MakeErr(RT::ERR_BAD_PAYLOAD, L"expected 0 or 1");
        }

        app.remoteLogEnabled = on;
        Log::SetEnabled(on);
        // An open panel is showing the OLD state on its Recording button — it
        // did not make this change and has no timer to notice it.
        Log::NotifyChanged();
        return MakeOk(on ? L"1" : L"0");
    }

    // --- msg <text> ---------------------------------------------------------
    // Put a centre-screen message on THIS instance, on somebody else's say-so.
    //
    // The whole point is the F10 console's Identify button: a wall of identical
    // viewers gives no clue which row drives which screen, so each target is
    // sent its own name and says who it is.
    //
    // Bounded, because the payload is attacker-controlled in the sense that
    // anything past the address gates can send it: an unbounded string would be
    // drawn every frame by the overlay until it expires.
    std::wstring DoMsgRemote(HWND hWnd, const std::wstring &payload) {
        if (payload.empty())
            return MakeErr(RT::ERR_BAD_PAYLOAD, L"expected some text");

        std::wstring text = payload;
        constexpr size_t MAX_MSG = 200;
        if (text.size() > MAX_MSG) text.resize(MAX_MSG);

        g_overlayManager.PostCenterMessage(hWnd, text);
        return MakeOk(text);
    }

    // --- zoom <percent> -----------------------------------------------------
    // Same arithmetic as ZoomWnd, and for the same reason: viewport.zoom is a
    // MULTIPLIER on the view mode's base scale, not an absolute percentage, so
    // setting it directly would land on the wrong size in every mode except
    // FitToView. The multiplier is derived from the effective zoom currently on
    // screen, then clamped by the shared limiter.
    std::wstring DoZoom(HWND hWnd, const std::wstring &payload) {
        float percent = 0.0f;
        if (!ParseFloatPercent(payload, percent))
            return MakeErr(RT::ERR_BAD_PAYLOAD, L"expected a percentage");

        // 0 means "back to the view mode's natural fit" — matches ZoomWnd, which
        // also recenters, otherwise the image stays parked off-centre at 100%.
        if (percent == 0.0f) {
            app.viewport.zoom    = 1.0f;
            app.viewport.offsetX = 0.0f;
            app.viewport.offsetY = 0.0f;
            g_overlayManager.PostCenterMessage(hWnd, Constants::Messages::ZOOM_RESET_MESSAGE);
            InvalidateRect(hWnd, nullptr, FALSE);
            return MakeOk(L"reset");
        }

        if (app.imgWidth <= 0 || app.imgHeight <= 0)
            return MakeErr(RT::ERR_BAD_PAYLOAD, L"no image loaded");

        const float target = Converters::PercentToRatio(percent);
        const float currentEffective = app.GetRealZoom(hWnd);
        if (!(currentEffective > 0.0f))
            return MakeErr(RT::ERR_INTERNAL, L"cannot determine current zoom");

        app.viewport.zoom *= target / currentEffective;
        ClampZoomToLimits(hWnd);
        ClampViewportOffset(hWnd);
        InvalidateRect(hWnd, nullptr, FALSE);

        // Report what was ACTUALLY applied, post-clamp — a caller that asked for
        // 5000% needs to know it got the ceiling instead.
        return MakeOk(Converters::FormatZoomPercent(app.GetRealZoom(hWnd)));
    }

    // --- interval <ms> ------------------------------------------------------
    // Same bounds and the same persistence as the dialog in CommandExecuter, so
    // a remotely set interval survives a restart exactly as a locally set one does.
    std::wstring DoInterval(HWND hWnd, const std::wstring &payload) {
        int ms = 0;
        if (!ParseInt(payload, ms))
            return MakeErr(RT::ERR_BAD_PAYLOAD, L"expected milliseconds");
        if (ms < Constants::Slideshow::INTERVAL_MIN_MS
            || ms > Constants::Slideshow::INTERVAL_MAX_MS)
            return MakeErr(RT::ERR_BAD_PAYLOAD,
                           L"out of range "
                           + std::to_wstring(Constants::Slideshow::INTERVAL_MIN_MS) + L"-"
                           + std::to_wstring(Constants::Slideshow::INTERVAL_MAX_MS));

        // Through applySlideshowInterval, which also RE-ARMS a running show.
        // Writing app.slideshow.intervalMs here directly is what made this
        // command appear to do nothing until the slideshow was restarted.
        AppCommands::applySlideshowInterval(hWnd, ms);
        Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_INTERVAL_MS,
                                           static_cast<DWORD>(ms));
        g_overlayManager.PostCenterMessage(
            hWnd, std::wstring(Constants::Messages::SLIDESHOW_INTERVAL_PREFIX) +
                      std::to_wstring(ms) + L" ms");
        return MakeOk(std::to_wstring(ms) + L" ms");
    }

    // --- observe 1|0 --------------------------------------------------------
    // The CALLER asks to be added to, or removed from, this instance's observer
    // list. It can only ever nominate itself: the connection comes from the
    // inbound guard, not from the payload, so there is no way to ask that events
    // be sent to some third machine.
    std::wstring DoObserve(const std::wstring &payload) {
        const ConnId self = InboundSource();
        if (self == CONN_NONE)
            return MakeErr(RT::ERR_INTERNAL, L"observe requires a connection");

        int on = 0;
        if (!ParseInt(payload, on) || (on != 0 && on != 1))
            return MakeErr(RT::ERR_BAD_PAYLOAD, L"expected 1 or 0");

        if (on) AddObserver(self);
        else    RemoveObserver(self);

        return MakeOk(on ? L"observing" : L"not observing");
    }

    // --- sync <k=v;k=v;…> ---------------------------------------------------
    // Adopt the sender's view and effect state wholesale.
    //
    // WHY THIS EXISTS: mirroring forwards TOGGLES, and a toggle applied to a
    // different starting state inverts rather than matches — press Grayscale on
    // a master whose slave already had it on and the two are now permanently
    // opposite. Worse, the effect CHAIN is ordered (each effect applies to the
    // previous one's result), so two instances can hold identical flags and
    // still draw visibly different images. No amount of resending toggles
    // repairs an ordering; only replacing the list does.
    //
    // THE POSITION TRAVELS AS A NAME, NEVER AS AN INDEX. Applying a folder
    // starts an asynchronous scan, so an index sent in the same breath would
    // race it — arrive against the one-file playlist that exists until the scan
    // lands, and mean the wrong picture or none. A path does not race anything:
    // OpenSpecificImage hands it to the scan as its target, so the folder is
    // opened and the picture is landed on by the same pass. (An index still
    // races, which is why the desync repair sends `sync` and THEN `goto`.)
    std::wstring DoSync(HWND hWnd, const std::wstring &payload) {
        std::wstring folder, image, fileName;
        int  sortOrder = -1, sortRev = -1;
        bool sawSort = false;

        // "k=v;k=v" — split on ';', then on the first '='. Unknown keys are
        // IGNORED rather than rejected: a newer instance driving an older one
        // must degrade to "applies what it understands", not fail outright.
        size_t start = 0;
        while (start <= payload.size()) {
            const size_t semi = payload.find(L';', start);
            const std::wstring tok =
                payload.substr(start, semi == std::wstring::npos ? std::wstring::npos
                                                                 : semi - start);
            start = (semi == std::wstring::npos) ? payload.size() + 1 : semi + 1;
            if (tok.empty()) continue;

            const size_t eq = tok.find(L'=');
            if (eq == std::wstring::npos) continue;
            const std::wstring k = tok.substr(0, eq);
            const std::wstring v = tok.substr(eq + 1);

            int  iv = 0;
            float fv = 0.0f;
            auto asInt   = [&] { return ParseInt(v, iv); };
            auto asFloat = [&] { return ParseFloatPercent(v, fv); };

            if      (k == L"folder")   folder = v;
            else if (k == L"image")    image  = v;   // same-machine: full path
            else if (k == L"name")     fileName = v; // elsewhere: bare file name
            else if (k == L"sort")    { if (asInt()) { sortOrder = iv; sawSort = true; } }
            else if (k == L"sortrev") { if (asInt()) { sortRev   = iv; sawSort = true; } }
            else if (k == L"view")    { if (asInt() && iv >= 1 && iv <= 5)
                                            app.viewMode = static_cast<Constants::ViewModes::ViewMode>(iv); }
            else if (k == L"rot")     { if (asInt()) app.viewport.rotation = ((iv % 360) + 360) % 360; }
            else if (k == L"fliph")   { if (asInt()) app.viewport.flippedH = (iv != 0); }
            else if (k == L"flipv")   { if (asInt()) app.viewport.flippedV = (iv != 0); }
            else if (k == L"gamma")   { if (asFloat()) app.gamma      = fv; }
            else if (k == L"bright")  { if (asFloat()) app.brightness = fv - 1.0f; } // see BuildSyncPayload
            else if (k == L"contrast"){ if (asFloat()) app.contrast   = fv; }
            else if (k == L"sat")     { if (asFloat()) app.saturation = fv; }
            else if (k == L"overlay") { if (asInt()) app.showOverlayInfoText = (iv != 0); }
            else if (k == L"layout")  { if (asInt() && iv >= 0 && iv < Constants::Overlay::LAYOUT_MODE_COUNT)
                                            app.overlayLayoutMode = iv; }
            // Through the helper, so a mirrored instance that is ALREADY running
            // a slideshow adopts the sender's pace immediately. Assigning the
            // field left it running at its own old interval — the mirror looked
            // synchronised in every respect except the one thing a slideshow is.
            else if (k == L"interval"){ if (asInt()
                                            && iv >= Constants::Slideshow::INTERVAL_MIN_MS
                                            && iv <= Constants::Slideshow::INTERVAL_MAX_MS)
                                            AppCommands::applySlideshowInterval(hWnd, iv); }
            else if (k == L"loop")    { if (asInt()) app.slideshow.loop    = (iv != 0); }
            else if (k == L"shuffle") { if (asInt()) app.slideshow.shuffle = (iv != 0); }
            else if (k == L"effects") {
                // REPLACED, not merged, and the ORDER is the payload's. This is
                // the one piece of state the boolean flags cannot express, and
                // the reason a flag-by-flag comparison can report "in sync"
                // while the two screens visibly differ.
                app.activeEffectsList.clear();
                app.effectGrayscale = app.effectInvert = app.effectSepia =
                app.effectSolarize  = app.effectOutline = app.effectThreshold = false;

                size_t es = 0;
                while (es <= v.size()) {
                    const size_t comma = v.find(L',', es);
                    const std::wstring name =
                        v.substr(es, comma == std::wstring::npos ? std::wstring::npos
                                                                 : comma - es);
                    es = (comma == std::wstring::npos) ? v.size() + 1 : comma + 1;
                    if (name.empty() || name == L"none") continue;

                    if      (name == Constants::Strings::EFFECT_GRAYSCALE) app.effectGrayscale = true;
                    else if (name == Constants::Strings::EFFECT_INVERT)    app.effectInvert    = true;
                    else if (name == Constants::Strings::EFFECT_SEPIA)     app.effectSepia     = true;
                    else if (name == Constants::Strings::EFFECT_SOLARIZE)  app.effectSolarize  = true;
                    else if (name == Constants::Strings::EFFECT_OUTLINE)   app.effectOutline   = true;
                    else if (name == Constants::Strings::EFFECT_THRESHOLD) app.effectThreshold = true;
                    else continue; // unknown effect from a newer build — skip it

                    app.activeEffectsList.push_back(name);
                }
            }
        }

        if (sawSort) {
            if (sortOrder >= 0) app.fileHandlerDefaultSortOrder     = sortOrder;
            if (sortRev   >= 0) app.fileHandlerIsReverseSortOrder   = (sortRev != 0);
        }

        // Where to land, last of all. Whichever form applies kicks off (or
        // re-uses) a scan that sorts with the order set just above, so the two
        // must happen in this sequence.
        //
        // A new sort is applied to the playlist BEFORE opening, not instead of
        // it: OpenSpecificImage has a fast path for a file already in the
        // current folder that reuses the playlist as it stands, and without this
        // that path would land on the right picture in the old order.
        if (sawSort && !app.playlist.empty()) ReSortPlaylistAndRebuildMap(hWnd);

        // Resolved before the chain below so each branch tests a plain bool —
        // one std::error_code reused across two filesystem calls has to be
        // cleared between them, and a missed clear reads as a failure that
        // never happened.
        std::error_code ec;
        const bool haveImage =
            !image.empty() && fs::is_regular_file(fs::path(image), ec) && !ec;
        ec.clear();
        const bool haveFolder =
            !folder.empty() && fs::is_directory(fs::path(folder), ec) && !ec;

        if (haveImage) {
            // Folder and picture in one. The sender only ever puts this key in
            // for an instance that shares its filesystem, so the path resolves
            // here to the same file it named there.
            OpenSpecificImage(hWnd, image);
        } else if (haveFolder) {
            OpenDirectory(hWnd, folder);
        } else if (!fileName.empty()) {
            // Another machine's picture, by name. Its folder was never sent —
            // this viewer keeps its own content and simply moves to the
            // same-named file if it happens to hold one, which is the most that
            // can be honestly done when the two ends own different files. A
            // linear walk, not a map lookup: the map is keyed by full path, and
            // this runs on a `sync`, not on the keystroke path.
            for (size_t i = 0; i < app.playlist.size(); ++i) {
                if (_wcsicmp(fs::path(app.playlist[i]).filename().wstring().c_str(),
                             fileName.c_str()) != 0) continue;
                LoadImageIndex(hWnd, static_cast<int>(i));
                break;
            }
        }

        app.UpdateRendererColorEffects(hWnd);
        g_overlayManager.UpdateEffects();
        InvalidateRect(hWnd, nullptr, FALSE);
        return MakeOk(L"synced");
    }

} // namespace

std::wstring BuildSyncPayload(bool includeFolder) {
    auto num = [](int v) { return std::to_wstring(v); };
    auto flt = [](float v) {
        wchar_t b[32];
        swprintf_s(b, L"%.3f", static_cast<double>(v));
        return std::wstring(b);
    };

    // WHERE the far end should land, in two spellings.
    //
    // `image` is the whole answer for an instance that shares this filesystem:
    // the folder AND the picture in one key, applied by OpenSpecificImage, which
    // opens the folder and lands on that file through the scan itself. That is
    // what makes a position safe to put in a sync at all — the older reasoning
    // (an index sent alongside a folder races the scan the folder starts) is
    // still true of an INDEX, and is exactly why this is a path.
    //
    // `name` is the bare file name, and goes to instances on other machines,
    // which are never sent a folder. Two boxes commonly hold the same collection
    // at different paths, and there the name is enough to land on the same
    // picture; where it is not, the far end has no such file and leaves itself
    // alone. `folder` still goes out beside them — it is the whole answer for a
    // viewer with nothing loaded, and the only one an older build understands.
    std::wstring folder, image, fileName;
    const bool haveCurrent = app.currentIndex >= 0 &&
                             app.currentIndex < static_cast<int>(app.playlist.size());
    if (haveCurrent) {
        const fs::path cur(app.playlist[app.currentIndex]);
        fileName = cur.filename().wstring();
        if (includeFolder) image = cur.wstring();
    }
    // `folder` travels ALONGSIDE `image`, not instead of it, even though this
    // build's DoSync would ignore it. An older instance does not know the newer
    // key and skips it silently — without the folder it would stay where it was
    // and the sync would look broken from the driving end. It costs one key on a
    // line sent by hand.
    if (includeFolder && !app.playlist.empty())
        folder = fs::path(app.playlist[0]).parent_path().wstring();

    std::wstring effects;
    for (const std::wstring &e : app.activeEffectsList) {
        if (!effects.empty()) effects += L',';
        effects += e;
    }
    if (effects.empty()) effects = L"none";

    // brightness is the one signed value here, and ParseFloatPercent (shared
    // with `zoom`) rejects a leading '-'. Biasing it by +1 keeps one number
    // parser for the whole protocol rather than adding a second that differs
    // only in sign handling; DoSync subtracts the same 1.
    // An empty folder= is omitted entirely rather than sent blank: DoSync treats
    // an absent key as "leave it alone", which is exactly right, whereas an
    // empty value would have to be special-cased at the far end.
    //
    // The path keys go FIRST, and everything after them is a number or a fixed
    // word. A file name containing ';' — legal, if unusual — therefore truncates
    // its own value and nothing else: the fragment left behind has no '=' and
    // DoSync skips it, so the sync still applies in full apart from the landing
    // place, which fails the exists() check and leaves that viewer where it was.
    return (folder.empty() ? std::wstring() : L"folder=" + folder + L";") +
           (image.empty()  ? std::wstring() : L"image="  + image  + L";") +
           (fileName.empty() ? std::wstring() : L"name=" + fileName + L";") +
           L"sort="       + num(app.fileHandlerDefaultSortOrder) +
           L";sortrev="  + num(app.fileHandlerIsReverseSortOrder ? 1 : 0) +
           L";view="     + num(static_cast<int>(app.viewMode)) +
           L";rot="      + num(app.viewport.rotation) +
           L";fliph="    + num(app.viewport.flippedH ? 1 : 0) +
           L";flipv="    + num(app.viewport.flippedV ? 1 : 0) +
           L";gamma="    + flt(app.gamma) +
           L";bright="   + flt(app.brightness + 1.0f) +
           L";contrast=" + flt(app.contrast) +
           L";sat="      + flt(app.saturation) +
           L";effects="  + effects +
           L";overlay="  + num(app.showOverlayInfoText ? 1 : 0) +
           L";layout="   + num(app.overlayLayoutMode) +
           L";interval=" + num(app.slideshow.intervalMs) +
           L";loop="     + num(app.slideshow.loop ? 1 : 0) +
           L";shuffle="  + num(app.slideshow.shuffle ? 1 : 0);
}

bool ExecutePayload(HWND hWnd, Command cmd, const std::wstring &payload,
                    std::wstring &replyOut) {
    switch (cmd) {
        case Command::Observe:
            replyOut = DoObserve(payload);
            return true;
        case Command::Sync:
            replyOut = DoSync(hWnd, payload);
            return true;
        case Command::JumpToImage:
            replyOut = DoJump(hWnd, payload);
            return true;
        case Command::OpenFile:
            replyOut = DoOpen(hWnd, payload);
            return true;
        case Command::ShowImageOnce:
            replyOut = DoInterject(hWnd, payload);
            return true;
        // The streaming trio and its inbound counterpart. Handled here rather than
        // through ExecuteCommand for the same reason every payload command is: they
        // answer with something a caller needs, and ExecuteCommand answers with
        // state.
        case Command::StreamImageBegin:
            replyOut = DoStreamBegin(payload);
            return true;
        case Command::StreamImageChunk:
            replyOut = DoStreamChunk(payload);
            return true;
        case Command::StreamImageShow:
            replyOut = DoStreamShow(hWnd);
            return true;
        // TWO-STAGE, exactly like SendDisplayedPreview below — and for a stronger
        // reason, because the ORIGINAL file is larger than any preview built from
        // it. Reading it and base64-encoding it here would hold the UI thread for
        // as long as that takes, which on a large photo is long enough to see.
        case Command::SendDisplayedImage:
            replyOut = std::wstring(ORIGINAL_PATH_MARKER) + DisplayedPath();
            return true;

        // TWO-STAGE, and this is only the first half.
        //
        // Decoding, scaling and re-encoding a 40-megapixel image takes far
        // longer than REPLY_TIMEOUT_MS and would freeze the viewer while it ran
        // — this function is on the UI thread. So the UI thread answers only
        // with the PATH, which costs nothing, and the socket thread does the
        // work and builds the real reply (see ClientThread).
        //
        // The marker is internal: this string never reaches a client. Anything
        // that did receive it would be a bug in the dispatch, which is why it is
        // spelled distinctively rather than looking like a plausible reply.
        case Command::SendDisplayedPreview:
            replyOut = std::wstring(PREVIEW_PATH_MARKER) + DisplayedPath();
            return true;
        case Command::FindImage:
            replyOut = DoFind(hWnd, payload);
            return true;
        case Command::ZoomTo:
            replyOut = DoZoom(hWnd, payload);
            return true;
        case Command::SlideshowSetInterval:
            replyOut = DoInterval(hWnd, payload);
            return true;
        case Command::EnableRemoteLog:
            replyOut = DoEnableLog(payload);
            return true;
        case Command::msgRemote:
            replyOut = DoMsgRemote(hWnd, payload);
            return true;
        // A NOTIFICATION: the sender says what it is now showing, the receiver
        // acknowledges and changes nothing. Handled here rather than in
        // ExecuteCommand's switch because it carries a payload, and everything
        // payload-carrying is answered from this function.
        //
        // The payload is echoed back so the reply says WHICH announcement was
        // acknowledged — with events arriving unsolicited mid-exchange, an
        // "OK" that named nothing would be unmatchable to what caused it.
        case Command::ImageChanged:
        // Its blank-screen counterpart, acknowledged the same way and for the
        // same reason: the sender says what its screen is now, the receiver
        // changes nothing, and the echoed payload is what makes the reply
        // matchable to the announcement that caused it.
        case Command::FolderChanged:
            replyOut = payload;
            return true;
        default:
            return false;
    }
}

} // namespace Remote
