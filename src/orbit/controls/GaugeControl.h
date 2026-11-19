#pragma once

#include "orbit/Control.h"

namespace orbit {

    // A circular gauge: a track arc, a fill arc for the value, the value in
    // the middle and a label under it. Same geometry and text as info-orbs.
    //
    // params (all optional):
    //   value (0), min (0), max (100)  numbers; "%" is shown only for 0-100
    //   label ("")                      shown upper-case under the value
    //   color (cyan), trackColor (darkgrey)
    //   style ("ring")                  ring (360°), speedometer (270°, gap at
    //                                   the bottom) or instrument (300°, ticks)
    //
    // value is live data and is never saved; the rest are settings. After a
    // restart a gauge comes back with its settings, showing 0 until the next
    // value is sent.
    class GaugeControl : public Control {
    public:
        const char *name() const override { return "gauge"; }
        int parse(JsonObjectConst params, JsonObject out, String &error) const override;
        void stripLive(JsonObject params) const override { params.remove("value"); }
        void draw(LGFX_Sprite &spr, JsonObjectConst params, const DrawContext &ctx) override;
    };

}
