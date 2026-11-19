#pragma once

#include "orbit/Control.h"

namespace orbit {

    // The current time (H:MM, hour not zero-padded) in the middle of the
    // screen, with the date above and the weekday below if asked for. Same
    // layout and sizes as info-orbs' time control.
    //
    // params (all optional, default false): showDate, showDay, format24Hour.
    // All are settings; there is no live data.
    class TimeControl : public Control {
    public:
        const char *name() const override { return "time"; }
        int parse(JsonObjectConst params, JsonObject out, String &error) const override;
        void draw(LGFX_Sprite &spr, JsonObjectConst params, const DrawContext &ctx) override;
        uint32_t refreshMs(int, JsonObjectConst) const override { return 1000; }
    };

}
