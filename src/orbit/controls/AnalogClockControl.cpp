#include "AnalogClockControl.h"
#include "core/TimeKeeper.h"
#include "ui/Smooth.h"

namespace orbit {

    namespace {
        constexpr int CENTRE = 120;
        constexpr int FACE_RADIUS = 110; // inside the round bezel's visible area

        // Hand lengths and half-widths in pixels.
        constexpr int HOUR_LENGTH = 55, MINUTE_LENGTH = 85, SECOND_LENGTH = 95;
        constexpr float HOUR_HALF = 1.0f, MINUTE_HALF = 0.75f, SECOND_HALF = 0.5f, TICK_HALF = 0.5f;
        constexpr int HUB_RADIUS = 4;

        // angle in degrees clockwise from 12 o'clock, rounded to the nearest
        // pixel (LovyanGFX's wedge lines take whole-pixel ends).
        void endpoint(float angle, int length, int &x, int &y) {
            const float rad = angle * DEG_TO_RAD;
            x = CENTRE + lroundf(length * sinf(rad));
            y = CENTRE - lroundf(length * cosf(rad));
        }

        void hand(LGFX_Sprite &spr, float angle, int length, float half, uint16_t color) {
            int x, y;
            endpoint(angle, length, x, y);
            spr.drawWedgeLine(CENTRE, CENTRE, x, y, half, half, color);
        }

        void paintFace(LGFX_Sprite &spr, uint16_t background, uint16_t tick) {
            spr.fillScreen(background);
            smooth::arc(spr, CENTRE, CENTRE, FACE_RADIUS, 0.5f, 360, tick);
            for (int i = 0; i < 12; i++) {
                int x1, y1, x2, y2;
                endpoint(i * 30.0f, FACE_RADIUS, x1, y1);
                endpoint(i * 30.0f, FACE_RADIUS - 10, x2, y2);
                spr.drawWedgeLine(x1, y1, x2, y2, TICK_HALF, TICK_HALF, tick);
            }
        }
    }

    int AnalogClockControl::parse(JsonObjectConst in, JsonObject out, String &error) const {
        int status;
        if ((status = optionalColor(in, out, "background", TFT_BLACK, error))) return status;
        if ((status = optionalColor(in, out, "tickColor", TFT_WHITE, error))) return status;
        if ((status = optionalColor(in, out, "hourColor", TFT_WHITE, error))) return status;
        if ((status = optionalColor(in, out, "minuteColor", TFT_WHITE, error))) return status;
        return optionalColor(in, out, "secondColor", TFT_RED, error);
    }

    void AnalogClockControl::draw(LGFX_Sprite &spr, JsonObjectConst p, const DrawContext &ctx) {
        const uint16_t background = p["background"], tick = p["tickColor"];
        String key = String(background);
        key += '|';
        key += tick;
        if (ensureLayer(ctx.screen)) {
            if (key != layerKey[ctx.screen]) {
                paintFace(layer[ctx.screen], background, tick);
                layerKey[ctx.screen] = key;
            }
            memcpy(spr.getBuffer(), layer[ctx.screen].getBuffer(), display::WIDTH * display::HEIGHT * 2);
        } else {
            paintFace(spr, background, tick); // no PSRAM for a layer: slower, same picture
        }

        struct tm t;
        if (!timekeeper::now(t)) return; // just the face until the clock is set

        const float second = t.tm_sec * 6.0f;
        const float minute = t.tm_min * 6.0f + t.tm_sec * 0.1f;
        const float hour = (t.tm_hour % 12) * 30.0f + t.tm_min * 0.5f;
        const uint16_t hourColor = p["hourColor"];
        hand(spr, hour, HOUR_LENGTH, HOUR_HALF, hourColor);
        hand(spr, minute, MINUTE_LENGTH, MINUTE_HALF, (uint16_t) p["minuteColor"]);
        hand(spr, second, SECOND_LENGTH, SECOND_HALF, (uint16_t) p["secondColor"]);
        spr.fillSmoothCircle(CENTRE, CENTRE, HUB_RADIUS, hourColor);
    }

    bool AnalogClockControl::ensureLayer(int screen) {
        LGFX_Sprite &l = layer[screen];
        if (l.getBuffer()) return true;
        l.setColorDepth(16);
        l.setPsram(true);
        layerKey[screen] = ""; // nothing painted yet
        return l.createSprite(display::WIDTH, display::HEIGHT) != nullptr;
    }

}
