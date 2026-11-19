#include "AnalogClockControl.h"
#include "core/TimeKeeper.h"

namespace orbit {

    namespace {
        constexpr int CENTRE = 120;
        constexpr int FACE_RADIUS = 110; // inside the round bezel's visible area

        // angle in degrees clockwise from 12 o'clock.
        void endpoint(float angle, int length, int &x, int &y) {
            const float rad = angle * DEG_TO_RAD;
            x = CENTRE + (int) (length * sinf(rad));
            y = CENTRE - (int) (length * cosf(rad));
        }

        void hand(LGFX_Sprite &spr, float angle, int length, uint16_t color) {
            int x, y;
            endpoint(angle, length, x, y);
            spr.drawLine(CENTRE, CENTRE, x, y, color);
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

    void AnalogClockControl::draw(LGFX_Sprite &spr, JsonObjectConst p, const DrawContext &) {
        const uint16_t tick = p["tickColor"];
        spr.fillScreen((uint16_t) p["background"]);
        spr.drawCircle(CENTRE, CENTRE, FACE_RADIUS, tick);
        for (int i = 0; i < 12; i++) {
            int x1, y1, x2, y2;
            endpoint(i * 30.0f, FACE_RADIUS, x1, y1);
            endpoint(i * 30.0f, FACE_RADIUS - 10, x2, y2);
            spr.drawLine(x1, y1, x2, y2, tick);
        }

        struct tm t;
        if (!timekeeper::now(t)) return; // just the face until the clock is set

        const float second = t.tm_sec * 6.0f;
        const float minute = t.tm_min * 6.0f + t.tm_sec * 0.1f;
        const float hour = (t.tm_hour % 12) * 30.0f + t.tm_min * 0.5f;
        const uint16_t hourColor = p["hourColor"];
        hand(spr, hour, 55, hourColor);
        hand(spr, minute, 85, (uint16_t) p["minuteColor"]);
        hand(spr, second, 95, (uint16_t) p["secondColor"]);
        spr.fillCircle(CENTRE, CENTRE, 4, hourColor);
    }

}
