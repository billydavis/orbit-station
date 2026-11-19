#pragma once

#include "orbit/Control.h"

namespace orbit {

    // A stock, crypto or forex price card from twelvedata (key in
    // /api/v1/settings). Same layout as info-orbs.
    //
    // params: symbol (required, e.g. "AAPL", "BTC/USD"), pollIntervalSeconds
    // (default 900, raised to at least 300 to stay inside twelvedata's free
    // daily limit). Both settings; prices are never saved.
    // POST /orbit/api/v1/screens/{n}/refresh fetches the price now.
    class TickerControl : public Control {
    public:
        const char *name() const override { return "ticker"; }
        int parse(JsonObjectConst params, JsonObject out, String &error) const override;
        void draw(LGFX_Sprite &spr, JsonObjectConst params, const DrawContext &ctx) override;
        uint32_t refreshMs(int, JsonObjectConst) const override { return 1000; }
    };

}
