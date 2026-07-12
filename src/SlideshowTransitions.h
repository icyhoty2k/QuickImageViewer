// SlideshowTransitions.h — All slideshow transition types, state, and functions.
// Every transition type is defined and implemented here + SlideshowTransitions.cpp.
#pragma once
#include <windows.h>
#include <string>
#include "Platform/Constants.h"

// =============================================================================
// Transition types  (set via -slideshowTransition=<name> or the S/R/R keys)
// =============================================================================
enum class TransitionType {
    Cut,      // instant switch — no animation (default)
    Fade,     // opacity 0 → app.opacity  via SetLayeredWindowAttributes
    Dissolve, // reserved — requires renderer alpha-blend of two bitmaps (TODO)
    Ripple,   // reserved — requires renderer warp shader (TODO)
    Push,     // new image slides in from the right via viewport.offsetX
    Zoom,     // new image zooms in from 2× to 1× via viewport.zoom
};

// =============================================================================
// Per-transition runtime state  (stored inside SlideshowState in AppState.h)
// =============================================================================
struct SlideshowTransitionState {
    TransitionType type        = TransitionType::Cut;
    TransitionType activeType  = TransitionType::Cut; // resolved type when shuffle is on
    bool           shuffle     = false;               // -slideshowTransitionShuffle
    bool           active      = false;               // true while animation is running
    float          progress    = 1.0f;                // 0.0 = start, 1.0 = complete
    DWORD          startTick   = 0;                   // GetTickCount64 value at transition start
    int            durationMs  = Constants::Slideshow::TRANSITION_DURATION_MS;
    BYTE           savedOpacity = 255;  // app.opacity saved before Fade dims the window
    int            winWidth     = 0;    // client width captured at start for Push
};

// =============================================================================
// Functions implemented in SlideshowTransitions.cpp
// =============================================================================

// Parse "-slideshowTransition=Cut" (or just "Fade", "Push", …).
// Returns TransitionType::Cut if the name is unrecognised.
TransitionType ParseTransitionType(const std::wstring& name);

// Pick a random transition type (excludes Cut and unimplemented stubs).
TransitionType RandomTransitionType();

// Call immediately after LoadImageIndex — sets the initial distorted visual state
// (opacity=0 for Fade, offsetX=width for Push, zoom=2× for Zoom) and starts the
// TRANSITION_TIMER_ID tick timer.
// For Cut (or shuffle-resolved Cut), marks the transition inactive immediately.
void StartTransition(HWND hWnd, SlideshowTransitionState& t);

// Call from WM_TIMER when wParam == TRANSITION_TIMER_ID.
// Advances progress and applies the interpolated visual state.
void StepTransition(HWND hWnd, SlideshowTransitionState& t);

// Returns true when progress has reached 1.0.
bool IsTransitionComplete(const SlideshowTransitionState& t);

// Call once IsTransitionComplete() returns true — snaps viewport/opacity to their
// final values and marks the transition inactive.
void FinishTransition(HWND hWnd, SlideshowTransitionState& t);
