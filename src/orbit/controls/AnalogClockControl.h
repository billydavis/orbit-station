#pragma once

#include "display/Display.h"
#include "orbit/Control.h"

namespace orbit {

    // A classic analog clock face: circle, 12 hour ticks, and hour, minute and
    // second hands (hour and minute move smoothly). Same geometry as info-orbs,
    // drawn anti-aliased.
    //
    // params (all optional colours): background (black), tickColor (white),
    // hourColor (white), minuteColor (white), secondColor (red). All settings.
    class AnalogClockControl : public Control {
    public:
        const char *name() const override { return "analogClock"; }
        int parse(JsonObjectConst params, JsonObject out, String &error) const override;
        void draw(LGFX_Sprite &spr, JsonObjectConst params, const DrawContext &ctx) override;
        uint32_t refreshMs(int, JsonObjectConst) const override { return 1000; }

    private:
        // The parts that stay still (background, circle, ticks), kept in
        // PSRAM (115 KB a screen, made the first time a screen shows a
        // clock) and repainted only when their colours change. Each frame
        // copies it and draws the hands on top.
        LGFX_Sprite layer[display::NUM_SCREENS];
        String layerKey[display::NUM_SCREENS];

        bool ensureLayer(int screen);
    };

}
