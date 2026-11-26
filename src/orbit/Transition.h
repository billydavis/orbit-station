#pragma once

// Animated changes between two pictures on one screen.
//
// This sits between a control and the display, so a control doesn't draw its
// own transitions. It only has to say when its content changed (see
// Control::contentId) and offer a "transition" param. The old picture is the
// last one sent to the screen, and the new one keeps animating underneath
// while the change plays.
//
// Only the renderer task may call these.

#include <LovyanGFX.hpp>

namespace orbit {
    namespace transition {

        constexpr uint32_t DURATION_MS = 800;
        constexpr uint32_t FRAME_MS = 33; // how often a screen redraws during one

        // The values of a "transition" param: random, none, then each effect.
        // Ends with nullptr.
        extern const char *const CHOICES[];

        // Call between display::beginFrame() and endFrame(), after the control
        // has drawn. contentId is what the control is showing (0 = nothing to
        // transition from or to), choice one of CHOICES. True if it drew
        // into the picture (a change is playing).
        bool apply(int screen, LGFX_Sprite &spr, uint32_t contentId, const char *choice);

        // Forget this screen's content, so the next picture just appears.
        void cancel(int screen);

        // A change is playing, so the screen needs redrawing every FRAME_MS.
        bool active(int screen);

    }
}
