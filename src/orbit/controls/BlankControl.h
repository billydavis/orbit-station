#pragma once

#include "orbit/Control.h"

namespace orbit {

    // An empty, black screen: "nothing assigned here".
    class BlankControl : public Control {
    public:
        const char *name() const override { return "blank"; }
        int parse(JsonObjectConst, JsonObject, String &) const override { return 0; }
        void draw(LGFX_Sprite &spr, JsonObjectConst, const DrawContext &) override { spr.fillScreen(TFT_BLACK); }
    };

}
