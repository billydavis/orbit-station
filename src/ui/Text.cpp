#include "Text.h"

extern const uint8_t robotoStart[] asm("_binary_fonts_RobotoRegular_ttf_start");
extern const uint8_t robotoEnd[] asm("_binary_fonts_RobotoRegular_ttf_end");

namespace text {

    namespace {
        OpenFontRender render;
        // info-orbs scales its Roboto sizes by this (its ttfFontMetrics table),
        // so a control asking for size 66 gets the same glyphs on both.
        constexpr float ROBOTO_SCALE = 1.37f;

        unsigned scaled(unsigned size) { return (unsigned) lroundf(size * ROBOTO_SCALE); }
    }

    bool begin() {
        render.setCacheSize(128, 128, 8192);
        if (render.loadFont(robotoStart, robotoEnd - robotoStart) != 0) {
            Serial.println("Text: couldn't load the font");
            return false;
        }
        return true;
    }

    namespace {
        void drawPx(LGFX_Sprite &spr, const String &str, int x, int y, unsigned px, Align align, uint16_t fg, uint16_t bg) {
            render.setDrawer(spr);
            FT_BBox box = render.calculateBoundingBox(0, 0, px, Align::TopLeft, Layout::Horizontal, str.c_str());
            render.setAlignment(align);
            render.setFontSize(px);
            render.drawString(str.c_str(), x, y - box.yMin, fg, bg);
        }
    }

    void drawFitted(LGFX_Sprite &spr, const String &str, int x, int y, int w, int h, Align align, uint16_t fg, uint16_t bg) {
        if (str.isEmpty()) return;
        unsigned px = h;
        while (px > 8) {
            FT_BBox box = render.calculateBoundingBox(0, 0, px, Align::TopLeft, Layout::Horizontal, str.c_str());
            if (box.xMax - box.xMin <= w && box.yMax - box.yMin <= h) break;
            px--;
        }
        drawPx(spr, str, x, y, px, align, fg, bg);
    }

    void draw(LGFX_Sprite &spr, const String &str, int x, int y, unsigned size, Align align, uint16_t fg, uint16_t bg) {
        render.setDrawer(spr);
        const unsigned px = scaled(size);
        // OpenFontRender places text slightly off vertically; correct it the
        // way info-orbs does (https://github.com/takkaO/OpenFontRender/issues/38).
        FT_BBox box = render.calculateBoundingBox(0, 0, px, Align::TopLeft, Layout::Horizontal, str.c_str());
        render.setAlignment(align);
        render.setFontSize(px);
        render.drawString(str.c_str(), x, y - box.yMin, fg, bg);
    }

    int width(const String &str, unsigned size) {
        FT_BBox box = render.calculateBoundingBox(0, 0, scaled(size), Align::TopLeft, Layout::Horizontal, str.c_str());
        return box.xMax - box.xMin;
    }

}
