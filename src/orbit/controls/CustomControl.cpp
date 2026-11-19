#include "CustomControl.h"

namespace orbit {

    namespace {
        constexpr int MAX_ELEMENTS = 400;
        constexpr int WRAP_CHARS = 10;
        constexpr int MAX_LINES = 10;

        // Colours here follow WebDataWidget: a name (unknown names draw black)
        // or, as a convenience, an RGB565 integer.
        uint16_t colorOf(JsonVariantConst v, uint16_t fallback) {
            if (v.is<uint16_t>()) return v.as<uint16_t>();
            if (!v.is<const char *>() || v.as<String>().isEmpty()) return fallback;
            uint16_t c;
            return colorFromName(v.as<String>(), c) ? c : (uint16_t) TFT_BLACK;
        }

        int intOf(JsonVariantConst v, int fallback) { return v.is<int>() ? v.as<int>() : fallback; }

        // TFT_eSPI's built-in fonts, which LovyanGFX also carries.
        const lgfx::IFont *fontOf(int n) {
            switch (n) {
            case 1:
                return &fonts::Font0;
            case 4:
                return &fonts::Font4;
            case 6:
                return &fonts::Font6;
            case 7:
                return &fonts::Font7;
            case 8:
                return &fonts::Font8;
            default:
                return &fonts::Font2;
            }
        }

        // "mc", "tl"... or words: "top left", "middle center".
        textdatum_t datumOf(JsonVariantConst v) {
            String a = v.is<const char *>() ? v.as<String>() : String("mc");
            a.toLowerCase();
            a.trim();
            const int space = a.indexOf(' ');
            if (space > 0) a = String(a[0]) + a[space + 1];
            if (a == "tl") return top_left;
            if (a == "tc") return top_center;
            if (a == "tr") return top_right;
            if (a == "ml") return middle_left;
            if (a == "mr") return middle_right;
            if (a == "bl") return bottom_left;
            if (a == "bc") return bottom_center;
            if (a == "br") return bottom_right;
            return middle_center;
        }

        bool isTrue(JsonVariantConst v) { return v.is<bool>() ? v.as<bool>() : intOf(v, 0) != 0; }

        void drawText(LGFX_Sprite &spr, JsonObjectConst e, const String &s, uint16_t fg, uint16_t bg) {
            spr.setFont(fontOf(intOf(e["font"], 2)));
            spr.setTextSize(intOf(e["size"], 2));
            spr.setTextDatum(datumOf(e["alignment"]));
            spr.setTextColor(colorOf(e["color"], fg), colorOf(e["background"], bg));
            spr.drawString(s, intOf(e["x"], 0), intOf(e["y"], 0));
        }

        // An arc in TFT_eSPI's angles (0 = 6 o'clock, clockwise); LovyanGFX
        // counts from 3 o'clock.
        void drawArc(LGFX_Sprite &spr, int x, int y, int r, int ir, float a0, float a1, uint16_t color) {
            if (a1 <= a0) a1 += 360;
            float s = fmodf(a0 + 90, 360), e = s + (a1 - a0);
            if (e <= 360) {
                spr.fillArc(x, y, r, ir, s, e, color);
            } else {
                spr.fillArc(x, y, r, ir, s, 360, color);
                spr.fillArc(x, y, r, ir, 0, e - 360, color);
            }
        }

        void drawElement(LGFX_Sprite &spr, JsonObjectConst e, uint16_t fg, uint16_t bg) {
            const String type = e["type"].as<String>();
            const uint16_t color = colorOf(e["color"], fg);
            // Rectangles and triangles take x1/y1 as well as x/y.
            const int x = e["x1"].is<int>() ? e["x1"].as<int>() : intOf(e["x"], 0);
            const int y = e["y1"].is<int>() ? e["y1"].as<int>() : intOf(e["y"], 0);
            if (type == "text") {
                drawText(spr, e, e["text"].as<String>(), fg, bg);
            } else if (type == "character") {
                const String c = e["character"].as<String>();
                if (c.length()) drawText(spr, e, c.substring(0, 1), fg, bg);
            } else if (type == "line") {
                spr.drawLine(x, y, intOf(e["x2"], 0), intOf(e["y2"], 0), color);
            } else if (type == "rectangle") {
                const int w = e["width"].is<int>() ? e["width"].as<int>() : intOf(e["x2"], x) - x;
                const int h = e["height"].is<int>() ? e["height"].as<int>() : intOf(e["y2"], y) - y;
                if (isTrue(e["filled"])) spr.fillRect(x, y, w, h, color);
                else spr.drawRect(x, y, w, h, color);
            } else if (type == "triangle") {
                const int x2 = intOf(e["x2"], 0), y2 = intOf(e["y2"], 0), x3 = intOf(e["x3"], 0), y3 = intOf(e["y3"], 0);
                if (isTrue(e["filled"])) spr.fillTriangle(x, y, x2, y2, x3, y3, color);
                else spr.drawTriangle(x, y, x2, y2, x3, y3, color);
            } else if (type == "circle") {
                const int r = intOf(e["radius"], 0);
                if (isTrue(e["filled"])) spr.fillCircle(x, y, r, color);
                else spr.drawCircle(x, y, r, color);
            } else if (type == "arc") {
                drawArc(spr, x, y, intOf(e["radius"], 0), intOf(e["innerRadius"], 0), intOf(e["angleStart"], 0),
                        intOf(e["angleEnd"], 0), color);
            }
        }

        // Paints a drawing; clear first unless it's an overlay.
        void paint(LGFX_Sprite &spr, JsonObjectConst p, bool clear) {
            const uint16_t bg = colorOf(p["background"], TFT_BLACK);
            const uint16_t fg = colorOf(p["color"], TFT_WHITE);
            if (clear) spr.fillScreen(bg);

            if (p["label"].is<const char *>() && strlen(p["label"]) > 0) {
                spr.setFont(&fonts::Font2);
                spr.setTextSize(2);
                spr.setTextDatum(middle_center);
                spr.setTextColor(colorOf(p["labelColor"], TFT_WHITE), bg);
                spr.drawString(p["label"].as<const char *>(), 120, 70);
            }

            JsonVariantConst data = p["data"];
            if (data.is<JsonArrayConst>()) {
                int n = 0;
                for (JsonObjectConst e : data.as<JsonArrayConst>()) {
                    if (++n > MAX_ELEMENTS) break;
                    drawElement(spr, e, fg, bg);
                }
                return;
            }
            if (data.isNull() || data.is<JsonObjectConst>()) return; // nothing to show

            // Plain text (or a bare number), word-wrapped in short lines.
            String s = data.as<String>();
            spr.setFont(&fonts::Font2);
            spr.setTextSize(2);
            spr.setTextDatum(middle_center);
            spr.setTextColor(fg, bg);
            int y = 110;
            for (int line = 0; line < MAX_LINES && s.length(); line++, y += 42) {
                int end = s.indexOf('\n');
                if (end < 0) end = s.length();
                if (end > WRAP_CHARS) {
                    int cut = s.lastIndexOf(' ', WRAP_CHARS);
                    end = cut > 0 ? cut : WRAP_CHARS;
                }
                spr.drawString(s.substring(0, end), 120, y);
                s = s.substring(end);
                if (s.startsWith(" ") || s.startsWith("\n")) s.remove(0, 1);
            }
        }
    }

    int CustomControl::parse(JsonObjectConst in, JsonObject out, String &) const {
        // Kept exactly as written; drawing is lenient, like WebDataWidget.
        if (!in.isNull()) out.set(in);
        out.remove("overlay");
        return 0;
    }

    int CustomControl::parseWrite(const WriteContext &ctx, JsonObjectConst in, JsonObject out, String &error) {
        // An overlay only has something to go on top of if this screen already
        // shows a drawing; anywhere else it's an ordinary drawing.
        overlay[ctx.screen] = in["overlay"] == true && ctx.sameControl;
        if (overlay[ctx.screen]) {
            out.set(ctx.current); // an overlay frame doesn't replace the drawing
            return 0;
        }
        return parse(in, out, error);
    }

    bool CustomControl::ensureCanvas(int screen, JsonObjectConst params) {
        LGFX_Sprite &c = canvas[screen];
        if (!c.getBuffer()) {
            c.setColorDepth(16);
            c.setPsram(true);
            if (!c.createSprite(display::WIDTH, display::HEIGHT)) return false;
            stale[screen] = true;
        }
        if (stale[screen]) {
            paint(c, params, true);
            stale[screen] = false;
        }
        return true;
    }

    AfterWrite CustomControl::applied(int screen, JsonObjectConst request, JsonObjectConst stored) {
        if (overlay[screen] && ensureCanvas(screen, stored)) {
            overlay[screen] = false;
            paint(canvas[screen], request, false);
        } else {
            overlay[screen] = false;
            stale[screen] = true; // a new drawing: start from a clean screen
        }
        return AfterWrite::Keep;
    }

    void CustomControl::draw(LGFX_Sprite &spr, JsonObjectConst params, const DrawContext &ctx) {
        if (ensureCanvas(ctx.screen, params)) {
            memcpy(spr.getBuffer(), canvas[ctx.screen].getBuffer(), display::WIDTH * display::HEIGHT * 2);
        } else {
            paint(spr, params, true); // no memory for a canvas: draw directly, no overlays
        }
    }

}
