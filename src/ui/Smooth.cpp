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
        // The signed-area accumulation rasterizer font renderers use (as in
        // font-rs): each edge adds, to the pixels it crosses on a row, how
        // much of each pixel lies to its right, signed by its direction. A
        // running sum across the row then gives every pixel's exact cover.
        // One row at a time, in internal RAM; two spare cells, as an edge
        // adds to the pixel after the last it touches.
        float acc[320 + 3];
        int accLo, accHi; // the cells touched on this row

        inline void add(int i, float v) {
            acc[i] += v;
            accLo = min(accLo, i);
            accHi = max(accHi, i);
        }

        // One edge's part of row y: from (x, top) to (xNext, bottom) within
        // the row, d its height there signed by direction.
        void addLine(float x, float xNext, float d, int w) {
            x = fminf(fmaxf(x, 0), (float) w);
            xNext = fminf(fmaxf(xNext, 0), (float) w);
            const float lo = fminf(x, xNext), hi = fmaxf(x, xNext);
            const float loFloor = floorf(lo), hiCeil = ceilf(hi);
            const int i0 = (int) loFloor, i1 = (int) hiCeil;
            if (i1 <= i0 + 1) {
                // Within one pixel: the part right of the line's middle.
                const float mid = 0.5f * (x + xNext) - loFloor;
                add(i0, d - d * mid);
                add(i0 + 1, d * mid);
                return;
            }
            const float s = 1.0f / (hi - lo);
            const float f0 = lo - loFloor;
            const float a0 = 0.5f * s * (1 - f0) * (1 - f0);
            const float f1 = hi - hiCeil + 1;
            const float am = 0.5f * s * f1 * f1;
            add(i0, d * a0);
            if (i1 == i0 + 2) {
                add(i0 + 1, d * (1 - a0 - am));
            } else {
                const float a1 = s * (1.5f - f0);
                add(i0 + 1, d * (a1 - a0));
                for (int i = i0 + 2; i < i1 - 1; i++) acc[i] += d * s;
                const float a2 = a1 + (i1 - i0 - 3) * s;
                add(i1 - 1, d * (1 - a2 - am));
            }
            add(i1, d * am);
        }

        // An edge from its top to its bottom; dir is +1 if it runs down.
        struct Edge {
            float x, top, bottom, slope, dir;
        };
    }


    void polygon(LGFX_Sprite &spr, const float *xy, int n, uint16_t color) {
        if (n < 3 || n > MAX_POINTS) return;
        const int w = min(spr.width(), 320), h = spr.height();
        float minY = xy[1], maxY = xy[1];
        for (int i = 1; i < n; i++) minY = fminf(minY, xy[2 * i + 1]), maxY = fmaxf(maxY, xy[2 * i + 1]);
        const int y0 = max(0, (int) floorf(minY)), y1 = min(h - 1, (int) ceilf(maxY) - 1);
        if (y0 > y1) return;
        Edge edges[MAX_POINTS];
        int count = 0;
        for (int i = 0, j = n - 1; i < n; j = i++) {
            float ax = xy[2 * j], ay = xy[2 * j + 1], bx = xy[2 * i], by = xy[2 * i + 1];
            if (ay == by) continue; // level: adds nothing
            float dir = 1;
            if (ay > by) std::swap(ax, bx), std::swap(ay, by), dir = -1;
            edges[count++] = {ax, ay, by, (bx - ax) / (by - ay), dir};
        }
        uint16_t *buf = (uint16_t *) spr.getBuffer(); // byte-swapped RGB565
        for (int y = y0; y <= y1; y++) {
            accLo = w + 2, accHi = -1;
            for (int e = 0; e < count; e++) {
                const Edge &g = edges[e];
                const float top = fmaxf(g.top, (float) y), bottom = fminf(g.bottom, (float) (y + 1));
                if (bottom <= top) continue;
                const float x = g.x + (top - g.top) * g.slope;
                addLine(x, x + (bottom - top) * g.slope, (bottom - top) * g.dir, w);
            }
            if (accHi < accLo) continue;
            uint16_t *line = buf + y * spr.width();
            float sum = 0;
            for (int x = accLo; x <= accHi + 1 && x <= w + 2; x++) {
                sum += acc[x];
                acc[x] = 0; // ready for the next row
                if (x >= w) continue;
                const float cover = fminf(fabsf(sum), 1.0f);
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
