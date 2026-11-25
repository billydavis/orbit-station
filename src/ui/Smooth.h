#pragma once

// Anti-aliased drawing that LovyanGFX doesn't have, straight into a 16-bit
// sprite's buffer. Meant for cached layers: correct edges matter more than
// speed, though none of it is slow.

#include <LovyanGFX.hpp>

namespace smooth {

    // fg over bg, k from 0 (all bg) to 256 (all fg), both RGB565.
    inline uint16_t blend256(uint16_t fg, uint16_t bg, int k) {
        const int r = ((bg >> 11) * (256 - k) + (fg >> 11) * k) >> 8;
        const int g = (((bg >> 5) & 63) * (256 - k) + ((fg >> 5) & 63) * k) >> 8;
        const int b = ((bg & 31) * (256 - k) + (fg & 31) * k) >> 8;
        return (r << 11) | (g << 5) | b;
    }

    // The same with a from 0 to 1. a is never negative, so adding a half and
    // truncating rounds it.
    inline uint16_t blend(uint16_t fg, uint16_t bg, float a) { return blend256(fg, bg, (int) (a * 256 + 0.5f)); }

    // An arc round (cx, cy), radius R and half a line's width thick, from 12
    // o'clock clockwise through sweep degrees, with round ends (360 or more
    // is a whole ring). Each pixel's cover comes from its distance to the
    // arc, blended over what's already there.
    void arc(LGFX_Sprite &spr, int cx, int cy, float R, float half, float sweep, uint16_t color);

}
