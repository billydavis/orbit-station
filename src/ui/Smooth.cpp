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

}
