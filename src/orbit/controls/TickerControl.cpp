#include "TickerControl.h"
#include "data/Feeds.h"
#include "ui/Text.h"

namespace orbit {

    namespace {
        constexpr int CENTRE = 120;
        constexpr uint16_t LIGHTGREY = 0xD69A;
    }

    int TickerControl::parse(JsonObjectConst in, JsonObject out, String &error) const {
        if (!in["symbol"].is<const char *>() || strlen(in["symbol"]) == 0) {
            error = "control 'ticker' requires params.symbol";
            return 400;
        }
        if (int status = optionalString(in, out, "symbol", "", 24, error)) return status;
        JsonVariantConst poll = in["pollIntervalSeconds"];
        if (!poll.isNull() && !poll.is<long>()) {
            error = "params.pollIntervalSeconds must be a whole number";
            return 400;
        }
        // Raised, not refused: see the class comment.
        out["pollIntervalSeconds"] = max(poll.isNull() ? 900L : poll.as<long>(), 300L);
        return 0;
    }

    void TickerControl::draw(LGFX_Sprite &spr, JsonObjectConst p, const DrawContext &) {
        const String symbol = p["symbol"].as<String>();
        const feeds::Quote q = feeds::quote(symbol, p["pollIntervalSeconds"].as<long>() * 1000UL);
        spr.fillScreen(TFT_BLACK);
        if (!q.valid) {
            text::draw(spr, symbol, CENTRE, CENTRE - 14, 20, Align::MiddleCenter, TFT_WHITE, TFT_BLACK);
            text::draw(spr, q.problem.length() ? q.problem : String("Loading price"), CENTRE, CENTRE + 18, 12, Align::MiddleCenter, 0x7BEF, TFT_BLACK);
            return;
        }

        const bool down = q.percentChange < 0;
        const uint16_t trend = down ? TFT_RED : TFT_GREEN;
        spr.fillRect(0, 70, 240, 49, TFT_WHITE);
        spr.fillRect(0, 111, 240, 20, LIGHTGREY);
        spr.fillArc(CENTRE, CENTRE, 120, 118, 0, 360, trend); // the rim shows the day's direction
        if (down) spr.fillTriangle(110, 11, 130, 11, 120, 23, trend);
        else spr.fillTriangle(110, 23, 130, 23, 120, 11, trend);

        text::draw(spr, String(q.percentChange, 2) + "%", CENTRE, 48, 29, Align::MiddleCenter, trend, TFT_BLACK);
        text::draw(spr, q.ticker, CENTRE, 92, 29, Align::MiddleCenter, TFT_BLACK, TFT_WHITE);
        text::draw(spr, q.company, CENTRE, 121, 11, Align::MiddleCenter, TFT_BLACK, LIGHTGREY);
        text::draw(spr, q.currency + String(q.price, 2), CENTRE, 155, 29, Align::MiddleCenter, TFT_WHITE, TFT_BLACK);
        text::draw(spr, "52 Week:", CENTRE, 185, 11, Align::MiddleCenter, TFT_WHITE, TFT_BLACK);
        text::draw(spr, "H: " + q.currency + String(q.high52, 2), CENTRE, 200, 11, Align::MiddleCenter, TFT_WHITE, TFT_BLACK);
        text::draw(spr, "L: " + q.currency + String(q.low52, 2), CENTRE, 215, 11, Align::MiddleCenter, TFT_WHITE, TFT_BLACK);
    }

}
