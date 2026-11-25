#pragma once

#include "display/Display.h"
#include "orbit/Control.h"

namespace orbit {

    // An analog clock: a dial and hour, minute and second hands (hour and
    // minute move smoothly), drawn anti-aliased. Hands are outlines filled
    // with smooth edges, the form clock face files will use.
    //
    // params (all optional, all settings):
    //   clockFace ("classic")    a built-in clock face:
    //                            classic: circle, 12 ticks, thin hands (the
    //                            info-orbs geometry), coloured by the params
    //                            below.
    //                            platform: bold bars, black batons and a red
    //                            lollipop second hand on white.
    //                            minimal: twelve dots, cream batons, no
    //                            second hand, on black.
    //   background (black), tickColor (white), hourColor (white),
    //   minuteColor (white), secondColor (red): classic's colours.
    class AnalogClockControl : public Control {
    public:
        const char *name() const override { return "analogClock"; }
        int parse(JsonObjectConst params, JsonObject out, String &error) const override;
        void draw(LGFX_Sprite &spr, JsonObjectConst params, const DrawContext &ctx) override;
        uint32_t refreshMs(int, JsonObjectConst) const override { return 1000; }

    private:
        // The parts that stay still (background and dial), kept in
        // PSRAM (115 KB a screen, made the first time a screen shows a
        // clock) and repainted only when their colours change. Each frame
        // copies it and draws the hands on top.
        LGFX_Sprite layer[display::NUM_SCREENS];
        String layerKey[display::NUM_SCREENS];

        bool ensureLayer(int screen);
    };

}
