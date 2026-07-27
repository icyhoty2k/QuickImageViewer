#include "RemoteExec.h"

#include "AppState.h"
#include "Platform/Constants.h"
#include "Platform/ConstantsStrings.h"
#include "Platform/FileHandler.h"
#include "Persistence/RegistryManager.h"
#include "Overlays/OverlayManager.h"
#include "Common/FuzzyMatch.h"
#include "Common/Converters.h"

#include <algorithm>
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
        if (ms < 100 || ms > 60000)
            return MakeErr(RT::ERR_BAD_PAYLOAD, L"out of range 100-60000");

        app.slideshow.intervalMs = ms;
        Persistence::Registry::SaveSetting(Constants::Registry::SLIDESHOW_INTERVAL_MS,
                                           static_cast<DWORD>(ms));
        g_overlayManager.PostCenterMessage(
            hWnd, std::wstring(Constants::Messages::SLIDESHOW_INTERVAL_PREFIX) +
                      std::to_wstring(ms) + L" ms");
        return MakeOk(std::to_wstring(ms) + L" ms");
    }

} // namespace

bool ExecutePayloadCommand(HWND hWnd, const RemoteRequest &req, std::wstring &replyOut) {
    switch (req.cmd) {
        case Command::JumpToImage:
            replyOut = DoJump(hWnd, req.payload);
            return true;
        case Command::OpenFile:
            replyOut = DoOpen(hWnd, req.payload);
            return true;
        case Command::FindImage:
            replyOut = DoFind(hWnd, req.payload);
            return true;
        case Command::ZoomTo:
            replyOut = DoZoom(hWnd, req.payload);
            return true;
        case Command::SlideshowSetInterval:
            replyOut = DoInterval(hWnd, req.payload);
            return true;
        default:
            return false;
    }
}

} // namespace Remote
