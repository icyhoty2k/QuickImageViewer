// SlideshowTransitions.cpp — Slideshow transition implementations.
// All transition logic lives here; AppMain.cpp drives the timer and calls these.
#include "SlideshowTransitions.h"
#include "AppState.h"
#include <algorithm>
#include <random>
#include <cmath>

extern AppState app;

// =============================================================================
// ParseTransitionType
// =============================================================================
TransitionType ParseTransitionType(const std::wstring& raw) {
    // Strip optional "-slideshowTransition=" prefix, normalise to lower-case
    std::wstring name = raw;
    const std::wstring prefix = L"-slideshowTransition=";
    if (name.size() > prefix.size() &&
        _wcsnicmp(name.c_str(), prefix.c_str(), prefix.size()) == 0)
        name = name.substr(prefix.size());

    if      (_wcsicmp(name.c_str(), L"Cut")      == 0) return TransitionType::Cut;
    else if (_wcsicmp(name.c_str(), L"Fade")     == 0) return TransitionType::Fade;
    else if (_wcsicmp(name.c_str(), L"Dissolve") == 0) return TransitionType::Dissolve;
    else if (_wcsicmp(name.c_str(), L"Ripple")   == 0) return TransitionType::Ripple;
    else if (_wcsicmp(name.c_str(), L"Push")     == 0) return TransitionType::Push;
    else if (_wcsicmp(name.c_str(), L"Zoom")     == 0) return TransitionType::Zoom;
    return TransitionType::Cut; // unknown → safe fallback
}

// =============================================================================
// RandomTransitionType  — excludes stub types (Dissolve, Ripple) until implemented
// =============================================================================
TransitionType RandomTransitionType() {
    static std::mt19937 rng{std::random_device{}()};
    // Implemented types only
    static const TransitionType pool[] = {
        TransitionType::Fade,
        TransitionType::Push,
        TransitionType::Zoom,
    };
    std::uniform_int_distribution<int> dist(0, static_cast<int>(std::size(pool)) - 1);
    return pool[dist(rng)];
}

// =============================================================================
// Easing — smooth-step so motion doesn't feel mechanical
// =============================================================================
static float SmoothStep(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// =============================================================================
// StartTransition  — called right after LoadImageIndex
// =============================================================================
void StartTransition(HWND hWnd, SlideshowTransitionState& t) {
    t.activeType = t.shuffle ? RandomTransitionType() : t.type;

    // Cut (or unimplemented stubs) → skip animation
    if (t.activeType == TransitionType::Cut     ||
        t.activeType == TransitionType::Dissolve ||
        t.activeType == TransitionType::Ripple) {
        t.active   = false;
        t.progress = 1.0f;
        return;
    }

    t.active    = true;
    t.progress  = 0.0f;
    t.startTick = GetTickCount();

    RECT rc{};
    GetClientRect(hWnd, &rc);
    t.winWidth = rc.right - rc.left;

    switch (t.activeType) {
        case TransitionType::Fade:
            t.savedOpacity = app.opacity;
            SetLayeredWindowAttributes(hWnd, 0, 0, LWA_ALPHA); // invisible
            break;

        case TransitionType::Push:
            // Shift new image off-screen to the right
            app.viewport.offsetX = static_cast<float>(t.winWidth);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case TransitionType::Zoom:
            // Start zoomed in 2× and animate down to fit
            app.viewport.zoom = 2.0f;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        default:
            break;
    }

    SetTimer(hWnd, Constants::Slideshow::TRANSITION_TIMER_ID,
             Constants::Slideshow::TRANSITION_TICK_MS, nullptr);
}

// =============================================================================
// StepTransition  — called every TRANSITION_TICK_MS while active
// =============================================================================
void StepTransition(HWND hWnd, SlideshowTransitionState& t) {
    if (!t.active) return;

    float elapsed = static_cast<float>(GetTickCount() - t.startTick);
    t.progress = std::clamp(elapsed / static_cast<float>(t.durationMs), 0.0f, 1.0f);
    float eased = SmoothStep(t.progress);

    switch (t.activeType) {
        case TransitionType::Fade: {
            BYTE alpha = static_cast<BYTE>(eased * static_cast<float>(t.savedOpacity));
            SetLayeredWindowAttributes(hWnd, 0, alpha, LWA_ALPHA);
            break;
        }
        case TransitionType::Push:
            app.viewport.offsetX = static_cast<float>(t.winWidth) * (1.0f - eased);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case TransitionType::Zoom:
            app.viewport.zoom = 2.0f - eased; // 2.0 → 1.0
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        default:
            break;
    }
}

// =============================================================================
// IsTransitionComplete
// =============================================================================
bool IsTransitionComplete(const SlideshowTransitionState& t) {
    return !t.active || t.progress >= 1.0f;
}

// =============================================================================
// FinishTransition  — snap to final values, mark done
// =============================================================================
void FinishTransition(HWND hWnd, SlideshowTransitionState& t) {
    switch (t.activeType) {
        case TransitionType::Fade:
            SetLayeredWindowAttributes(hWnd, 0, t.savedOpacity, LWA_ALPHA);
            break;
        case TransitionType::Push:
            app.viewport.offsetX = 0.0f;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        case TransitionType::Zoom:
            app.viewport.zoom = 1.0f;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        default:
            break;
    }
    t.active   = false;
    t.progress = 1.0f;
}
