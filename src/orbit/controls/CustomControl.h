#pragma once

#include "display/Display.h"
#include "orbit/Control.h"

namespace orbit {

    // A drawing sent by the client, in info-orbs' WebDataWidget format: params
    // is one "displays" entry, so existing clients' drawings work unchanged.
    //
    //   label, labelColor        a heading at the top (built-in font 2, size 2)
    //   data                     a string (word-wrapped, centred) or an array of
    //                            elements: text, character, line, rectangle,
    //                            triangle, circle, arc (unknown types skipped)
    //   color, background        default colours (names, or RGB565 integers)
    //   overlay: true            draw on top of the current drawing instead of
    //                            clearing first; not saved, GET keeps the base
    //
    // Element coordinates, fonts (1, 2, 4, 6, 7, 8), sizes, alignments ("mc",
    // "top left"...) and arc angles (0 = 6 o'clock, clockwise) follow
    // TFT_eSPI, as in info-orbs.
    //
    // A drawing is saved in its own LittleFS file, written only when it
    // changes; GET reports params exactly as written.
    class CustomControl : public Control {
    public:
        const char *name() const override { return "custom"; }
        int parse(JsonObjectConst params, JsonObject out, String &error) const override;
        int parseWrite(const WriteContext &ctx, JsonObjectConst params, JsonObject out, String &error) override;
        AfterWrite applied(int screen, JsonObjectConst request, JsonObjectConst stored) override;
        bool savedInFile() const override { return true; }
        void draw(LGFX_Sprite &spr, JsonObjectConst params, const DrawContext &ctx) override;

    private:
        // The picture so far, so overlays can build on it. Made on first use.
        LGFX_Sprite canvas[display::NUM_SCREENS];
        bool stale[display::NUM_SCREENS] = {true, true, true, true, true};
        bool overlay[display::NUM_SCREENS] = {}; // the write being applied is an overlay frame

        bool ensureCanvas(int screen, JsonObjectConst params);
    };

}
