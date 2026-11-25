#include "Text.h"
#include "Smooth.h"

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
        // FreeType allocates from PSRAM here (FileSupport.h), so a big glyph
        // cache costs no internal RAM. At 8 KB every repaint reloaded every
        // glyph, 5-15 ms a string.
        render.setCacheSize(128, 128, 64 * 1024);
        if (render.loadFont(robotoStart, robotoEnd - robotoStart) != 0) {
            Serial.println("Text: couldn't load the font");
            return false;
        }
        return true;
    }

    void drawPx(LGFX_Sprite &spr, const String &str, int x, int y, unsigned px, Align align, uint16_t fg, uint16_t bg) {
        render.setDrawer(spr);
        FT_BBox box = render.calculateBoundingBox(0, 0, px, Align::TopLeft, Layout::Horizontal, str.c_str());
        render.setAlignment(align);
        render.setFontSize(px);
        render.drawString(str.c_str(), x, y - box.yMin, fg, bg);
    }

    void drawOver(LGFX_Sprite &spr, const String &str, int x, int y, unsigned px, Align align, uint16_t fg) {
        if (str.isEmpty()) return;
        // White on black in a scratch picture the same size, so the green
        // channel (6 bits) is the text's cover at each pixel.
        LGFX_Sprite scratch;
        scratch.setColorDepth(16);
        scratch.setPsram(true);
        if (!scratch.createSprite(spr.width(), spr.height())) {
            drawPx(spr, str, x, y, px, align, fg, fg); // no room: hard edges rather than nothing
            return;
        }
        scratch.fillScreen(TFT_BLACK);
        drawPx(scratch, str, x, y, px, align, TFT_WHITE, TFT_BLACK);
        const uint16_t *src = (const uint16_t *) scratch.getBuffer();
        uint16_t *dst = (uint16_t *) spr.getBuffer();
        for (int i = 0, n = spr.width() * spr.height(); i < n; i++) {
            const int g = (__builtin_bswap16(src[i]) >> 5) & 63;
            if (g) dst[i] = __builtin_bswap16(smooth::blend256(fg, __builtin_bswap16(dst[i]), (g * 256 + 31) / 63));
        }
        scratch.deleteSprite();
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
