// SlideshowTransitions.cpp — Slideshow transition implementations.
// All transition logic lives here; AppMain.cpp drives the timer and calls these.
#include "SlideshowTransitions.h"
#include "AppState.h"
#include "Platform/ConstantsStrings.h" // TRANSITION_NAMES — display order source
#include <algorithm>
#include <array>
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
    // Current names only — the pre-rename spellings (Push*, Zoom, ZoomFade,
    // Diagonal) were dropped. They only ever appeared on a command line, and
    // several of them described the wrong direction. Nothing persisted is
    // affected: the registry stores the enum's int, not a name.
    // Accept both the spaced and unspaced spellings.
    struct Alias { const wchar_t *a; const wchar_t *b; TransitionType t; };
    static const Alias kAliases[] = {
        {L"SlideLeft",     L"Slide Left",     TransitionType::SlideLeft    },
        {L"SlideRight",    L"Slide Right",    TransitionType::SlideRight   },
        {L"SlideUp",       L"Slide Up",       TransitionType::SlideUp      },
        {L"SlideDown",     L"Slide Down",     TransitionType::SlideDown    },
        {L"SlideDiagonal", L"Slide Diagonal", TransitionType::SlideDiagonal},
        {L"ZoomOut",       L"Zoom Out",       TransitionType::ZoomOut      },
        {L"ZoomIn",        L"Zoom In",        TransitionType::ZoomIn       },
        {L"SoftZoom",      L"Soft Zoom",      TransitionType::SoftZoom     },
        {L"Spin",          L"Spin",           TransitionType::Spin         },
        {L"SpinZoom",      L"Spin Zoom",      TransitionType::SpinZoom     },
        {L"DriftLeft",     L"Drift Left",     TransitionType::DriftLeft    },
        {L"DriftUp",       L"Drift Up",       TransitionType::DriftUp      },
        {L"Flicker",       L"Flicker",        TransitionType::Flicker      },
        {L"Bounce",        L"Bounce",         TransitionType::Bounce       },
        {L"Swing",         L"Swing",          TransitionType::Swing        },
        {L"Slam",          L"Slam",           TransitionType::Slam         },
        {L"Iris",          L"Iris",           TransitionType::Iris         },
    };
    for (const Alias &al : kAliases)
        if (_wcsicmp(name.c_str(), al.a) == 0 || _wcsicmp(name.c_str(), al.b) == 0)
            return al.t;

    return TransitionType::Cut; // unknown → safe fallback
}

// =============================================================================
// TransitionDisplayOrder — alphabetical by display name; the menu's 1..N order.
// Sorted once on first use; the names are compile-time constants so the result
// never changes afterwards.
// =============================================================================
const int *TransitionDisplayOrder() {
    static const auto order = [] {
        std::array<int, Constants::Slideshow::TRANSITION_COUNT> o{};
        for (int i = 0; i < Constants::Slideshow::TRANSITION_COUNT; ++i) o[i] = i;
        std::sort(o.begin(), o.end(), [](int a, int b) {
            return _wcsicmp(Constants::Messages::TRANSITION_NAMES[a],
                            Constants::Messages::TRANSITION_NAMES[b]) < 0;
        });
        return o;
    }();
    return order.data();
}

// =============================================================================
// RandomTransitionType  — every animated type (Cut excluded: it is "no animation")
// =============================================================================
TransitionType RandomTransitionType() {
    // Draws over the enum directly, skipping Cut at index 0. Order is irrelevant
    // for a random pick, so this needs no table to stay in sync with.
    static std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> dist(1, Constants::Slideshow::TRANSITION_COUNT - 1);
    return static_cast<TransitionType>(dist(rng));
}

// =============================================================================
// ResolveNextTransition — applies SOURCE (which transitions) then ORDER (how the
// next is drawn). Advances t.seqIndex when the order is SEQUENTIAL.
//
// SOURCE::NONE   ignores order entirely and always returns the picked type.
// SOURCE::ALL    pools every entry the menu lists (Cut included — "all" means all).
// SOURCE::LIST   pools only the ticked entries; an empty tick-list falls back to
//                the picked type so a slideshow never stalls on nothing.
// =============================================================================
static TransitionType ResolveNextTransition(SlideshowTransitionState &t) {
    namespace SS = Constants::Slideshow;
    constexpr int count = SS::TRANSITION_COUNT;

    if (t.source == SS::TransitionSource::NONE)
        return t.type;

    // Membership test for the active pool.
    const bool useList = (t.source == SS::TransitionSource::LIST);
    auto inPool = [&](int idx) {
        return !useList || (t.listMask & (1u << idx)) != 0u;
    };

    const int *order = TransitionDisplayOrder();

    if (t.order == SS::TransitionOrder::RANDOM) {
        // Collect the pool, then draw. Cheap — at most 21 entries.
        int pool[count];
        int n = 0;
        for (int i = 0; i < count; ++i) {
            const int idx = order[i];
            // A random "Cut" is just a dropped animation, so skip it unless the
            // user explicitly ticked it in LIST mode.
            if (idx == static_cast<int>(TransitionType::Cut) && !useList) continue;
            if (inPool(idx)) pool[n++] = idx;
        }
        if (n == 0) return t.type; // empty list → fall back
        static std::mt19937 rng{std::random_device{}()};
        std::uniform_int_distribution<int> dist(0, n - 1);
        return static_cast<TransitionType>(pool[dist(rng)]);
    }

    // SEQUENTIAL — walk the menu's 1..N order from the cursor to the next pool
    // member, so the numbering you see is the order you get.
    for (int step = 0; step < count; ++step) {
        const int slot = (t.seqIndex + step) % count;
        const int idx  = order[slot];
        if (inPool(idx)) {
            t.seqIndex = (slot + 1) % count;
            return static_cast<TransitionType>(idx);
        }
    }
    return t.type; // nothing ticked → fall back
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
    t.activeType = ResolveNextTransition(t);
    t.alpha = 1.0f; // only the fading types drop this below 1

    // Cut → no animation at all
    if (t.activeType == TransitionType::Cut) {
        t.active   = false;
        t.progress = 1.0f;
        return;
    }

    t.active    = true;
    t.progress  = 0.0f;
    t.startTick = GetTickCount();

    RECT rc{};
    GetClientRect(hWnd, &rc);
    t.winWidth  = rc.right - rc.left;
    t.winHeight = rc.bottom - rc.top;
    t.savedRotation = app.viewport.rotation; // rotational types animate around this

    const float driftX = static_cast<float>(t.winWidth)  * Constants::Slideshow::DRIFT_FRACTION;
    const float driftY = static_cast<float>(t.winHeight) * Constants::Slideshow::DRIFT_FRACTION;

    switch (t.activeType) {
        case TransitionType::Fade:
        case TransitionType::Dissolve:
            // Both start fully transparent; Step differentiates the alpha curve.
            t.alpha = 0.0f;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case TransitionType::Ripple:
            // Fades in while the zoom oscillates around 1.0.
            t.alpha = 0.0f;
            app.viewport.zoom = 1.0f + Constants::Slideshow::RIPPLE_AMPLITUDE;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case TransitionType::SlideLeft:
            // Park off-screen to the right, then travel left
            app.viewport.offsetX = static_cast<float>(t.winWidth);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case TransitionType::SlideUp:
            // Park off-screen below, then travel up
            app.viewport.offsetY = static_cast<float>(t.winHeight);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case TransitionType::ZoomOut:
            // Start magnified 2× and shrink to fit
            app.viewport.zoom = 2.0f;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case TransitionType::ZoomIn:
            // Start small and grow to fit
            app.viewport.zoom = Constants::Slideshow::ZOOM_IN_START;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case TransitionType::SlideRight:
            // Park off-screen to the left, then travel right
            app.viewport.offsetX = -static_cast<float>(t.winWidth);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case TransitionType::SlideDown:
            // Park off-screen above, then travel down
            app.viewport.offsetY = -static_cast<float>(t.winHeight);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case TransitionType::SlideDiagonal:
            app.viewport.offsetX = static_cast<float>(t.winWidth);
            app.viewport.offsetY = static_cast<float>(t.winHeight);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case TransitionType::SoftZoom:
            t.alpha = 0.0f;
            app.viewport.zoom = Constants::Slideshow::SOFT_ZOOM_START;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case TransitionType::Spin:
            t.alpha = 0.0f;
            app.viewport.rotation = t.savedRotation + Constants::Slideshow::SPIN_DEGREES;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case TransitionType::SpinZoom:
            app.viewport.rotation = t.savedRotation + Constants::Slideshow::SPIN_ZOOM_DEGREES;
            app.viewport.zoom = Constants::Slideshow::SPIN_ZOOM_START;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case TransitionType::DriftLeft:
            t.alpha = 0.0f;
            app.viewport.offsetX = driftX;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case TransitionType::DriftUp:
            t.alpha = 0.0f;
            app.viewport.offsetY = driftY;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case TransitionType::Flicker:
            t.alpha = 0.0f;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case TransitionType::Bounce:
            app.viewport.zoom = Constants::Slideshow::BOUNCE_START;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case TransitionType::Swing:
            t.alpha = 0.0f;
            app.viewport.rotation = t.savedRotation + Constants::Slideshow::SWING_DEGREES;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case TransitionType::Slam:
            t.alpha = 0.0f;
            app.viewport.zoom = Constants::Slideshow::SLAM_START;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case TransitionType::Iris:
            app.viewport.zoom = Constants::Slideshow::IRIS_START;
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
        case TransitionType::Fade:
            t.alpha = eased;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        // Same ramp as Fade, but quantised: alpha climbs in DISSOLVE_STEPS jumps
        // so the image appears to "dissolve" in rather than glide in.
        case TransitionType::Dissolve: {
            constexpr float steps = static_cast<float>(Constants::Slideshow::DISSOLVE_STEPS);
            t.alpha = std::floor(eased * steps) / steps;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }

        // Zoom oscillates around 1.0 with an amplitude that decays to zero as the
        // transition completes, while the window fades in — a settling "ripple".
        case TransitionType::Ripple: {
            t.alpha = eased;

            const float decay = 1.0f - t.progress; // amplitude envelope
            const float phase = t.progress * Constants::Slideshow::RIPPLE_WAVES *
                                2.0f * 3.14159265f;
            app.viewport.zoom = 1.0f + Constants::Slideshow::RIPPLE_AMPLITUDE *
                                       decay * std::cos(phase);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }

        case TransitionType::SlideLeft:
            app.viewport.offsetX = static_cast<float>(t.winWidth) * (1.0f - eased);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case TransitionType::SlideUp:
            app.viewport.offsetY = static_cast<float>(t.winHeight) * (1.0f - eased);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case TransitionType::ZoomOut:
            app.viewport.zoom = 2.0f - eased; // 2.0 → 1.0 (shrinks to fit)
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case TransitionType::ZoomIn: {
            constexpr float start = Constants::Slideshow::ZOOM_IN_START;
            app.viewport.zoom = start + (1.0f - start) * eased; // start → 1.0
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }

        case TransitionType::SlideRight:
            app.viewport.offsetX = -static_cast<float>(t.winWidth) * (1.0f - eased);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case TransitionType::SlideDown:
            app.viewport.offsetY = -static_cast<float>(t.winHeight) * (1.0f - eased);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case TransitionType::SlideDiagonal:
            app.viewport.offsetX = static_cast<float>(t.winWidth)  * (1.0f - eased);
            app.viewport.offsetY = static_cast<float>(t.winHeight) * (1.0f - eased);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case TransitionType::SoftZoom: {
            constexpr float start = Constants::Slideshow::SOFT_ZOOM_START;
            t.alpha = eased;
            app.viewport.zoom = start + (1.0f - start) * eased; // 1.5 → 1.0
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }

        case TransitionType::Spin:
            t.alpha = eased;
            app.viewport.rotation = t.savedRotation +
                static_cast<int>(Constants::Slideshow::SPIN_DEGREES * (1.0f - eased));
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case TransitionType::SpinZoom: {
            constexpr float start = Constants::Slideshow::SPIN_ZOOM_START;
            app.viewport.rotation = t.savedRotation +
                static_cast<int>(Constants::Slideshow::SPIN_ZOOM_DEGREES * (1.0f - eased));
            app.viewport.zoom = start + (1.0f - start) * eased;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }

        case TransitionType::DriftLeft:
            t.alpha = eased;
            app.viewport.offsetX = static_cast<float>(t.winWidth) *
                                   Constants::Slideshow::DRIFT_FRACTION * (1.0f - eased);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        case TransitionType::DriftUp:
            t.alpha = eased;
            app.viewport.offsetY = static_cast<float>(t.winHeight) *
                                   Constants::Slideshow::DRIFT_FRACTION * (1.0f - eased);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;

        // Unstable strobe that settles: the wobble is scaled by the remaining
        // (1 - eased), so alpha always lands exactly on 1.0.
        case TransitionType::Flicker: {
            const float phase = t.progress * Constants::Slideshow::FLICKER_CYCLES *
                                2.0f * 3.14159265f;
            const float wobble = 0.5f + 0.5f * std::fabs(std::cos(phase));
            t.alpha = 1.0f - (1.0f - eased) * wobble;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }

        // Grows to 1.0 while a half-sine overshoot pushes briefly past it.
        case TransitionType::Bounce: {
            constexpr float start = Constants::Slideshow::BOUNCE_START;
            const float base = start + (1.0f - start) * eased;
            const float overshoot = Constants::Slideshow::BOUNCE_OVERSHOOT *
                                    std::sin(3.14159265f * eased);
            app.viewport.zoom = base + overshoot;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }

        case TransitionType::Swing: {
            t.alpha = eased;
            const float decay = 1.0f - t.progress;
            const float phase = t.progress * Constants::Slideshow::SWING_WAVES *
                                2.0f * 3.14159265f;
            app.viewport.rotation = t.savedRotation +
                static_cast<int>(Constants::Slideshow::SWING_DEGREES * decay * std::cos(phase));
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }

        // Cubic ease-out on raw progress — covers most of the distance instantly
        // then decelerates hard, which is what sells the "impact".
        case TransitionType::Slam: {
            const float remain = 1.0f - t.progress;
            t.alpha = eased;
            app.viewport.zoom = 1.0f + (Constants::Slideshow::SLAM_START - 1.0f) *
                                       (remain * remain * remain);
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }

        case TransitionType::Iris: {
            constexpr float start = Constants::Slideshow::IRIS_START;
            app.viewport.zoom = start + (1.0f - start) * eased;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }

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
        case TransitionType::Dissolve:
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        case TransitionType::Ripple:
            app.viewport.zoom = 1.0f;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        case TransitionType::SlideLeft:
        case TransitionType::SlideRight:
        case TransitionType::DriftLeft:
            app.viewport.offsetX = 0.0f;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        case TransitionType::SlideUp:
        case TransitionType::SlideDown:
        case TransitionType::DriftUp:
            app.viewport.offsetY = 0.0f;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        case TransitionType::SlideDiagonal:
            app.viewport.offsetX = 0.0f;
            app.viewport.offsetY = 0.0f;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        case TransitionType::ZoomOut:
        case TransitionType::ZoomIn:
        case TransitionType::SoftZoom:
        case TransitionType::Bounce:
        case TransitionType::Slam:
        case TransitionType::Iris:
            app.viewport.zoom = 1.0f;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        // Restore the user's own rotation exactly — never straighten it.
        case TransitionType::Spin:
        case TransitionType::Swing:
            app.viewport.rotation = t.savedRotation;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        case TransitionType::SpinZoom:
            app.viewport.rotation = t.savedRotation;
            app.viewport.zoom = 1.0f;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        case TransitionType::Flicker:
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        default:
            break;
    }
    t.active   = false;
    t.progress = 1.0f;
    t.alpha    = 1.0f; // always restore full image opacity
}
