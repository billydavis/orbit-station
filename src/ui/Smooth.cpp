#include "Smooth.h"

namespace smooth {

    namespace {
        // Whether the point (dx, dy) from the centre is within a sweep of a
        // radians clockwise from 12 o'clock (sinA and cosA are a's), without
        // working out its angle. With u east and v north, the point's angle
        // t is at most a when sin(a - t) = (sinA * v - cosA * u) / r is not
        // negative, on the side of the circle the sweep is in.
        bool withinSweep(int dx, int dy, float a, float sinA, float cosA) {
            const float u = dx, v = -dy, side = sinA * v - cosA * u;
            if (a <= PI) return u >= 0 && side >= 0; // the right half, up to the end
            return u >= 0 || side >= 0;               // all but the left half past the end
        }
    }

    // Each row only visits the band the ring crosses (the whole square round
    // a 112 px ring is ~54,000 pixels, the band ~4,000).
    void arc(LGFX_Sprite &spr, int cx, int cy, float R, float half, float sweep, uint16_t color) {
        if (sweep <= 0) return;
        const bool whole = sweep >= 360;
        const float reach = half + 1.0f;
        const float outer2 = (R + reach) * (R + reach), inner2 = R > reach ? (R - reach) * (R - reach) : 0;
        const float a = sweep * DEG_TO_RAD, sinA = sinf(a), cosA = cosf(a);
        const float endX = R * sinA, endY = -R * cosA; // the start is (0, -R)
        const int span = ceilf(R + reach);
        const int w = spr.width(), h = spr.height();
        uint16_t *buf = (uint16_t *) spr.getBuffer(); // byte-swapped RGB565
        for (int dy = -span; dy <= span; dy++) {
            const int y = cy + dy;
            if (y < 0 || y >= h) continue;
            // Pixels further out than edge are past the band's outer edge,
            // and nearer the centre than skip inside its inner edge (each
            // one pixel generous, so rounding can't lose any).
            const float outside = outer2 - dy * dy, inside = inner2 - dy * dy;
            if (outside < 0) continue;
            const int edge = min(span, (int) sqrtf(outside) + 1);
            const int skip = inside > 0 ? (int) sqrtf(inside) - 1 : 0;
            for (int dx = -edge; dx <= edge; dx++) {
                if (dx > -skip && dx < skip) dx = skip;
                const int x = cx + dx;
                if (x < 0 || x >= w) continue;
                const float d2 = dx * dx + dy * dy;
                if (d2 > outer2 || d2 < inner2) continue;
                float dist = fabsf(sqrtf(d2) - R);
                if (!whole && !withinSweep(dx, dy, a, sinA, cosA)) {
                    // past either end: measure to the nearer round cap
                    const float sy = dy + R, ex = dx - endX, ey = dy - endY;
                    dist = sqrtf(fminf(dx * dx + sy * sy, ex * ex + ey * ey));
                }
                const float cover = half + 0.5f - dist;
                if (cover <= 0) continue;
                uint16_t &px = buf[y * w + x];
                px = __builtin_bswap16(cover >= 1 ? color : blend(color, __builtin_bswap16(px), cover));
            }
        }
    }

    namespace {
        // Sub-rows a pixel row is sampled at; across each sub-row the cover
        // is exact. 16 gives near-horizontal edges 16 levels of shading.
        constexpr int SUB = 16;

        // The polygon's cover of each pixel on the row being filled, in
        // internal RAM (the sprite's width at most).
        float rowCover[320];

        void addSpan(float xa, float xb, int w) {
            xa = fmaxf(xa, 0);
            xb = fminf(xb, (float) w);
            if (xb <= xa) return;
            const int ia = (int) xa, ib = (int) xb;
            constexpr float k = 1.0f / SUB;
            if (ia == ib) {
                rowCover[ia] += (xb - xa) * k;
                return;
            }
            rowCover[ia] += (ia + 1 - xa) * k;
            for (int i = ia + 1; i < ib; i++) rowCover[i] += k;
            if (ib < w) rowCover[ib] += (xb - ib) * k;
        }
    }

    void polygon(LGFX_Sprite &spr, const float *xy, int n, uint16_t color) {
        if (n < 3 || n > MAX_POINTS) return;
        const int w = min(spr.width(), (int) (sizeof rowCover / sizeof rowCover[0])), h = spr.height();
        float minX = xy[0], maxX = xy[0], minY = xy[1], maxY = xy[1];
        for (int i = 1; i < n; i++) {
            minX = fminf(minX, xy[2 * i]), maxX = fmaxf(maxX, xy[2 * i]);
            minY = fminf(minY, xy[2 * i + 1]), maxY = fmaxf(maxY, xy[2 * i + 1]);
        }
        const int x0 = max(0, (int) floorf(minX)), x1 = min(w - 1, (int) floorf(maxX));
        const int y0 = max(0, (int) floorf(minY)), y1 = min(h - 1, (int) floorf(maxY));
        if (x0 > x1 || y0 > y1) return;
        uint16_t *buf = (uint16_t *) spr.getBuffer(); // byte-swapped RGB565
        float cross[MAX_POINTS];
        for (int y = y0; y <= y1; y++) {
            for (int x = x0; x <= x1; x++) rowCover[x] = 0;
            for (int s = 0; s < SUB; s++) {
                const float sy = y + (s + 0.5f) / SUB;
                int found = 0;
                for (int i = 0, j = n - 1; i < n; j = i++) {
                    const float ax = xy[2 * j], ay = xy[2 * j + 1], bx = xy[2 * i], by = xy[2 * i + 1];
                    if ((ay <= sy) != (by <= sy)) cross[found++] = ax + (sy - ay) * (bx - ax) / (by - ay);
                }
                // Few crossings: sort them by insertion.
                for (int i = 1; i < found; i++) {
                    const float v = cross[i];
                    int j = i - 1;
                    for (; j >= 0 && cross[j] > v; j--) cross[j + 1] = cross[j];
                    cross[j + 1] = v;
                }
                for (int i = 0; i + 1 < found; i += 2) addSpan(cross[i], cross[i + 1], w);
            }
            uint16_t *line = buf + y * spr.width();
            for (int x = x0; x <= x1; x++) {
                const float cover = rowCover[x];
                if (cover <= 0.002f) continue;
                line[x] = __builtin_bswap16(cover >= 0.998f ? color : blend(color, __builtin_bswap16(line[x]), cover));
            }
        }
    }

    void disc(LGFX_Sprite &spr, float cx, float cy, float r, uint16_t color) {
        if (r <= 0) return;
        const int w = spr.width(), h = spr.height();
        const int x0 = max(0, (int) floorf(cx - r - 1)), x1 = min(w - 1, (int) ceilf(cx + r + 1));
        const int y0 = max(0, (int) floorf(cy - r - 1)), y1 = min(h - 1, (int) ceilf(cy + r + 1));
        const float inner2 = r > 0.5f ? (r - 0.5f) * (r - 0.5f) : 0, outer2 = (r + 0.5f) * (r + 0.5f);
        uint16_t *buf = (uint16_t *) spr.getBuffer();
        for (int y = y0; y <= y1; y++) {
            const float dy = y + 0.5f - cy;
            for (int x = x0; x <= x1; x++) {
                const float dx = x + 0.5f - cx, d2 = dx * dx + dy * dy;
                if (d2 >= outer2) continue;
                uint16_t &px = buf[y * w + x];
                if (d2 <= inner2) {
                    px = __builtin_bswap16(color);
                    continue;
                }
                const float cover = r + 0.5f - sqrtf(d2);
                px = __builtin_bswap16(blend(color, __builtin_bswap16(px), cover));
            }
        }
    }

}
