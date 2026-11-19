#pragma once

// TrueType text drawn into a screen's picture, with the same font and sizes
// as info-orbs (Roboto through OpenFontRender), so ported controls look the
// same. Only call from the task that draws the screens.

#include <LovyanGFX.hpp>
#include <OpenFontRender.h>

namespace text {

    // Loads the font. Call once before drawing.
    bool begin();

    // Draws text with (x, y) at the point given by align. size uses info-orbs'
    // font sizes (scaled the same way), so a control's numbers carry over.
    // Antialiasing blends into bg, so pass the colour the text sits on.
    void draw(LGFX_Sprite &spr, const String &str, int x, int y, unsigned size, Align align, uint16_t fg, uint16_t bg);

    // Width in pixels the text would take at this size.
    int width(const String &str, unsigned size);

}
