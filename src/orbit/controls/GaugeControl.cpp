#include "GaugeControl.h"
#include "ui/Text.h"

namespace orbit {

    namespace {
        constexpr int CENTRE = 120;
        constexpr int OUTER_RADIUS = 108; // inside the round bezel
        constexpr int RING_THICKNESS = 16;
        constexpr int INSTRUMENT_THICKNESS = 10;
        constexpr int VALUE_Y = CENTRE - 8;
        constexpr int VALUE_SIZE = 34;
        constexpr int LABEL_Y = CENTRE + 24;
        constexpr int LABEL_SIZE = 13;
        constexpr uint16_t LABEL_COLOR = 0xC618; // silver
        constexpr uint16_t BACKGROUND = TFT_BLACK;

        const char *const STYLES[] = {"ring", "speedometer", "instrument", nullptr};

        // Angles here are degrees clockwise from 12 o'clock.
        struct Geometry {
            float start;
            float sweep;
            int thickness;
            bool ticks;
        };

        Geometry geometryFor(const char *style) {
            if (strcmp(style, "speedometer") == 0) return {-135.0f, 270.0f, RING_THICKNESS, false};
            if (strcmp(style, "instrument") == 0) return {-150.0f, 300.0f, INSTRUMENT_THICKNESS, true};
            return {0.0f, 360.0f, RING_THICKNESS, false};
        }

        void point(float angle, float radius, int &x, int &y) {
            const float rad = angle * DEG_TO_RAD;
            x = CENTRE + (int) lroundf(radius * sinf(rad));
            y = CENTRE - (int) lroundf(radius * cosf(rad));
        }

        // LovyanGFX measures arcs from 3 o'clock; split an arc that crosses
        // that point into two.
        void arc(LGFX_Sprite &spr, int inner, int outer, float start, float sweep, uint16_t color) {
            if (sweep <= 0) return;
            if (sweep >= 360) {
                spr.fillArc(CENTRE, CENTRE, outer, inner, 0, 360, color);
                return;
            }
            float s = fmodf(start - 90.0f + 720.0f, 360.0f);
            float e = s + sweep;
            if (e <= 360) {
                spr.fillArc(CENTRE, CENTRE, outer, inner, s, e, color);
            } else {
                spr.fillArc(CENTRE, CENTRE, outer, inner, s, 360, color);
                spr.fillArc(CENTRE, CENTRE, outer, inner, 0, e - 360, color);
            }
        }

        // The fill arc with rounded ends, like info-orbs' smooth arc.
        void roundedArc(LGFX_Sprite &spr, int inner, int outer, float start, float sweep, uint16_t color) {
            if (sweep <= 0) return;
            arc(spr, inner, outer, start, sweep, color);
            if (sweep >= 360) return;
            const float mid = (inner + outer) / 2.0f;
            const int capRadius = (outer - inner) / 2;
            int x, y;
            point(start, mid, x, y);
            spr.fillCircle(x, y, capRadius, color);
            point(start + sweep, mid, x, y);
            spr.fillCircle(x, y, capRadius, color);
        }
    }

    int GaugeControl::parse(JsonObjectConst in, JsonObject out, String &error) const {
        int status;
        if ((status = optionalString(in, out, "label", "", 32, error))) return status;
        if ((status = optionalNumber(in, out, "value", 0, error))) return status;
        if ((status = optionalNumber(in, out, "min", 0, error))) return status;
        if ((status = optionalNumber(in, out, "max", 100, error))) return status;
        if ((status = optionalColor(in, out, "color", TFT_CYAN, error))) return status;
        if ((status = optionalColor(in, out, "trackColor", TFT_DARKGREY, error))) return status;
        return optionalChoice(in, out, "style", STYLES, error);
    }

    void GaugeControl::draw(LGFX_Sprite &spr, JsonObjectConst p, const DrawContext &) {
        const Geometry g = geometryFor(p["style"]);
        const int inner = OUTER_RADIUS - g.thickness;
        const float value = p["value"], min = p["min"], max = p["max"];
        const uint16_t color = p["color"], track = p["trackColor"];

        float fraction = max != min ? (value - min) / (max - min) : 0;
        fraction = constrain(fraction, 0.0f, 1.0f);

        spr.fillScreen(BACKGROUND);
        arc(spr, inner, OUTER_RADIUS, g.start, g.sweep, track);
        if (g.ticks) {
            for (int i = 0; i <= 10; i++) {
                int x1, y1, x2, y2;
                point(g.start + g.sweep * i / 10.0f, inner - 2, x1, y1);
                point(g.start + g.sweep * i / 10.0f, inner - 10, x2, y2);
                spr.drawLine(x1, y1, x2, y2, track);
            }
        }
        roundedArc(spr, inner, OUTER_RADIUS, g.start, g.sweep * fraction, color);

        // "%" only for the usual 0-100 range; a temperature shouldn't get one.
        String valueText = String((int) lroundf(value));
        if (min == 0 && max == 100) valueText += "%";
        text::draw(spr, valueText, CENTRE, VALUE_Y, VALUE_SIZE, Align::MiddleCenter, color, BACKGROUND);

        String label = p["label"].as<String>();
        if (label.length()) {
            label.toUpperCase();
            text::draw(spr, label, CENTRE, LABEL_Y, LABEL_SIZE, Align::MiddleCenter, LABEL_COLOR, BACKGROUND);
        }
    }

}
