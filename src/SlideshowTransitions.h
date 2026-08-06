// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

// SlideshowTransitions.h — All slideshow transition types, state, and functions.
// Every transition type is defined and implemented here + SlideshowTransitions.cpp.
#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include "Platform/Constants.h"

static_assert(Constants::Slideshow::TRANSITION_COUNT <= 32,
              "SlideshowTransitionState::listMask is a 32-bit membership bitmask");

// =============================================================================
// Transition types  (set via -slideshowTransition=<name> or the S/R/R keys)
// =============================================================================
// NOTE: every transition is driven at WINDOW level — layered-window opacity plus
// viewport.offsetX/offsetY/zoom. The renderer is never asked to blend two bitmaps,
// so Dissolve and Ripple are faithful *approximations* built from those levers
// rather than per-pixel effects. Values are persisted as ints, so only ever
// APPEND new members — never reorder.
enum class TransitionType {
    Cut,      // instant switch — no animation (default)
    Fade,     // opacity 0 → app.opacity, smooth ease
    Dissolve, // opacity ramp quantised into discrete steps — grainy staircase fade
    Ripple,   // damped zoom oscillation around 1.0 + fade-in — pulsing "ripple"
    // The four Slide* members are named by the direction the image TRAVELS.
    // Sign convention (must match the renderer): the image is drawn at
    // left = center + offsetX, top = center + offsetY — so a POSITIVE offset
    // parks it right/below and it then travels left/up toward zero.
    // Zoom* are likewise named by what the image DOES, not by its start value:
    // starting above 1.0 and settling to fit is a zoom OUT; starting below 1.0
    // and growing to fit is a zoom IN.
    SlideLeft,  // starts off-screen right,  travels left   via viewport.offsetX
    ZoomOut,    // starts magnified 2×,  shrinks to fit     via viewport.zoom
    SlideUp,    // starts off-screen below,  travels up     via viewport.offsetY
    ZoomIn,     // starts small 0.4×,    grows to fit       via viewport.zoom
    SlideRight, // starts off-screen left,   travels right
    SlideDown,  // starts off-screen above,  travels down
    SoftZoom,   // gentle 1.5× shrink to fit while fading in
    Spin,       // full 360° rotation settling upright, fading in
    SpinZoom,   // half turn while growing from small
    DriftLeft,  // slow drift leftward into place + fade  (Ken-Burns feel)
    DriftUp,    // slow drift upward into place + fade
    Flicker,    // fades in through an unstable, strobing alpha
    Bounce,     // grows past 1× then settles back — springy overshoot
    Swing,      // damped rotational wobble settling upright
    Slam,       // rushes in from 3× with a hard ease-out — impact
    Iris,       // expands from a point outward
    SlideDiagonal, // starts off the bottom-right corner, travels up-left
};

// =============================================================================
// Per-transition runtime state  (stored inside SlideshowState in AppState.h)
// =============================================================================
struct SlideshowTransitionState {
    TransitionType type        = TransitionType::Cut;
    TransitionType activeType  = TransitionType::Cut; // resolved type when shuffle is on
    // Which transitions are in play — Constants::Slideshow::TransitionSource.
    int            source      = Constants::Slideshow::TransitionSource::NONE;
    // How the next one is drawn from that pool — Constants::Slideshow::TransitionOrder.
    int            order       = Constants::Slideshow::TransitionOrder::SEQUENTIAL;
    // Membership bitmask for source == LIST: bit N set = TransitionType N is in
    // the pool. Defaults to every animated type (Cut excluded).
    uint32_t       listMask    = 0xFFFFFFFEu;
    int            seqIndex    = 0;                   // rolling cursor for SEQUENTIAL
    bool           active      = false;               // true while animation is running
    float          progress    = 1.0f;                // 0.0 = start, 1.0 = complete
    DWORD          startTick   = 0;                   // GetTickCount64 value at transition start
    int            durationMs  = Constants::Slideshow::TRANSITION_DURATION_MS;
    BYTE           savedOpacity = 255;  // app.opacity — kept for restore safety
    // Image opacity 0..1 applied by the RENDERER (D2D layer) — NOT the window.
    // Fading the window with SetLayeredWindowAttributes made the desktop show
    // through; fading only the drawn image keeps the background opaque.
    float          alpha        = 1.0f;
    int            winWidth     = 0;    // client width  captured at start for Push
    int            winHeight    = 0;    // client height captured at start for SlideUp/Down
    // The user's own rotation (R key), captured at start. Spin/SpinZoom/Swing
    // animate RELATIVE to this and restore it exactly, so a rotated image is
    // never silently straightened by a transition.
    int            savedRotation = 0;
};

// =============================================================================
// Functions implemented in SlideshowTransitions.cpp
// =============================================================================

// Parse "-slideshowTransition=Cut" (or just "Fade", "Push", …).
// Returns TransitionType::Cut if the name is unrecognised.
TransitionType ParseTransitionType(const std::wstring& name);

// Pick a random transition type (excludes Cut — that is "no animation").
TransitionType RandomTransitionType();

// TransitionType values ordered alphabetically by their display name, i.e. the
// exact order the Slideshow › Transition menu lists them in (entry 1 .. N).
// Returns a pointer to TRANSITION_COUNT ints, built once and never mutated.
//
// SINGLE SOURCE: both the menu and SEQUENTIAL playback walk this, so the order
// you see numbered in the menu is the order the slideshow plays.
const int *TransitionDisplayOrder();

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
