#pragma once

#include "orbit/Control.h"

namespace orbit {

    // One piece of the weather per screen, from Visual Crossing (key and
    // default location in /api/v1/settings). Same layouts as info-orbs' dark
    // mode.
    //
    // params:
    //   element      required: icon, temperature (with today's high and low)
    //                or condition (city and description)
    //   location     this screen's own place (up to 64 characters); omitted,
    //                the weatherLocation setting
    //   showCity     temperature: city above it (default true with a location)
    //   showHighLow  temperature: default true
    //   color, background, highColor, lowColor, cityColor  optional colours
    // All settings; the readings themselves are never saved.
    class WeatherControl : public Control {
    public:
        const char *name() const override { return "weather"; }
        int parse(JsonObjectConst params, JsonObject out, String &error) const override;
        void draw(LGFX_Sprite &spr, JsonObjectConst params, const DrawContext &ctx) override;
        // Readings arrive in the background; check for them every second.
        uint32_t refreshMs(int, JsonObjectConst) const override { return 1000; }
    };

}
