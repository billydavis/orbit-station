#include "Transition.h"
#include "display/Display.h"
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <math.h>

// The effects are adapted from Pixel Jukebox's (lib/jukebox_core/transitions),
// reworked for 240x240 round screens. Pixels are RGB565 as sprites store them,
// byte-swapped. Effects that only choose between the two pictures write the old
// one over the new one in place; the ones that move pixels get a copy.

namespace orbit {
    namespace transition {

        namespace {
            constexpr int N = display::WIDTH;
            constexpr int PIXELS = N * N;
            constexpr size_t BYTES = PIXELS * sizeof(uint16_t);

            using Render = void (*)(float t, uint32_t seed, const uint16_t *a, const uint16_t *b, uint16_t *out);

            inline float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
            inline float ease(float t) { return t * t * (3 - 2 * t); }
            inline int steps(float v) { return (int) (clamp01(v) * N + 0.5f); }

            uint32_t hash(uint32_t x) {
                x ^= x >> 16;
                x *= 0x7feb352dU;
                x ^= x >> 15;
                x *= 0x846ca68bU;
                x ^= x >> 16;
                return x;
            }

            // A pixel spread out so its channels can be scaled in one multiply.
            constexpr uint32_t SPREAD = 0x07E0F81F;
            inline uint32_t expand(uint16_t p) {
                const uint32_t c = __builtin_bswap16(p);
                return (c | (c << 16)) & SPREAD;
            }
            inline uint16_t pack(uint32_t v) {
                v &= SPREAD;
                return __builtin_bswap16((uint16_t) (v | (v >> 16)));
            }

            // Puts the old picture back on part of one line.
            inline void keepOld(uint16_t *out, const uint16_t *a, int y, int x0, int x1) {
                if (x1 > x0) memcpy(out + y * N + x0, a + y * N + x0, (x1 - x0) * sizeof(uint16_t));
            }

            void crossfade(float t, uint32_t, const uint16_t *a, const uint16_t *b, uint16_t *out) {
                const uint32_t k = (uint32_t) (t * 32);
                for (int i = 0; i < PIXELS; i++) out[i] = pack((expand(a[i]) * (32 - k) + expand(b[i]) * k) >> 5);
            }

            void fadeThroughBlack(float t, uint32_t, const uint16_t *a, const uint16_t *b, uint16_t *out) {
                const bool second = t >= 0.5f;
                const uint32_t k = (uint32_t) ((second ? 2 * t - 1 : 1 - 2 * t) * 32);
                const uint16_t *source = second ? b : a;
                for (int i = 0; i < PIXELS; i++) out[i] = pack((expand(source[i]) * k) >> 5);
            }

            // The new picture pushes the old one off, from one of four sides.
            void slide(float t, uint32_t seed, const uint16_t *a, const uint16_t *b, uint16_t *out) {
                const int shift = steps(ease(t));
                const int direction = seed & 3;
                for (int y = 0; y < N; y++) {
                    uint16_t *row = out + y * N;
                    if (direction == 0) {
                        memcpy(row, a + y * N + shift, (N - shift) * 2);
                        memcpy(row + N - shift, b + y * N, shift * 2);
                    } else if (direction == 1) {
                        memcpy(row, b + y * N + N - shift, shift * 2);
                        memcpy(row + shift, a + y * N, (N - shift) * 2);
                    } else if (direction == 2) {
                        const int from = y + shift;
                        memcpy(row, from < N ? a + from * N : b + (from - N) * N, N * 2);
                    } else {
                        const int from = y - shift;
                        memcpy(row, from >= 0 ? a + from * N : b + (from + N) * N, N * 2);
                    }
                }
            }

            // A straight edge sweeps across, from one of four sides.
            void wipe(float t, uint32_t seed, const uint16_t *a, const uint16_t *, uint16_t *out) {
                const int edge = steps(t);
                const int direction = seed & 3;
                for (int y = 0; y < N; y++) {
                    if (direction == 0) keepOld(out, a, y, edge, N);
                    else if (direction == 1) keepOld(out, a, y, 0, N - edge);
                    else if (direction == 2 ? y >= edge : y < N - edge) keepOld(out, a, y, 0, N);
                }
            }

            // A circle opens from the centre, to the edge of the round screen.
            void iris(float t, uint32_t, const uint16_t *a, const uint16_t *, uint16_t *out) {
                const float radius = t * 122;
                for (int y = 0; y < N; y++) {
                    const float dy = y - 119.5f;
                    const float left = radius * radius - dy * dy;
                    if (left <= 0) {
                        keepOld(out, a, y, 0, N);
                    } else {
                        const int half = (int) sqrtf(left);
                        keepOld(out, a, y, 0, N / 2 - half);
                        keepOld(out, a, y, N / 2 + half, N);
                    }
                }
            }

            // Twelve horizontal strips open together.
            void blinds(float t, uint32_t, const uint16_t *a, const uint16_t *, uint16_t *out) {
                const int rows = (int) (t * 20 + 0.5f);
                for (int y = 0; y < N; y++) {
                    if (y % 20 >= rows) keepOld(out, a, y, 0, N);
                }
            }

            // The picture breaks into bigger blocks, swaps at the coarsest, then sharpens.
            void mosaic(float t, uint32_t, const uint16_t *a, const uint16_t *b, uint16_t *out) {
                const bool second = t >= 0.5f;
                const float coarseness = second ? (1 - t) * 2 : t * 2;
                const int block = 1 << (int) (coarseness * 5.999f); // 1 to 32
                const uint16_t *source = second ? b : a;
                uint16_t row[N];
                for (int y = 0; y < N; y += block) {
                    const int sy = min(y + block / 2, N - 1);
                    for (int x = 0; x < N; x += block) {
                        const uint16_t v = source[sy * N + min(x + block / 2, N - 1)];
                        for (int k = x; k < min(x + block, N); k++) row[k] = v;
                    }
                    for (int line = y; line < min(y + block, N); line++) memcpy(out + line * N, row, sizeof(row));
                }
            }

            // Chunky squares switch over in random order.
            void dissolve(float t, uint32_t seed, const uint16_t *a, const uint16_t *, uint16_t *out) {
                constexpr int CELL = 6, CELLS = N / CELL;
                const uint32_t limit = (uint32_t) (t * 65536);
                bool old[CELLS];
                for (int cy = 0; cy < CELLS; cy++) {
                    for (int cx = 0; cx < CELLS; cx++) old[cx] = (hash(cx + cy * CELLS + seed * 4099U) & 0xFFFF) >= limit;
                    for (int y = cy * CELL; y < (cy + 1) * CELL; y++) {
                        for (int cx = 0; cx < CELLS; cx++) {
                            if (old[cx]) keepOld(out, a, y, cx * CELL, (cx + 1) * CELL);
                        }
                    }
                }
            }

            // Columns of the old picture slide down at different times.
            void melt(float t, uint32_t seed, const uint16_t *a, const uint16_t *b, uint16_t *out) {
                constexpr int WIDE = 4, COLUMNS = N / WIDE;
                int shift[COLUMNS];
                int wander = hash(seed) % 16;
                for (int c = 0; c < COLUMNS; c++) {
                    wander += (int) (hash(seed + c * 31U) % 5) - 2; // neighbours stay close
                    wander = constrain(wander, 0, 15);
                    shift[c] = steps((t - wander / 15.0f * 0.4f) / 0.6f);
                }
                for (int y = 0; y < N; y++) {
                    for (int c = 0; c < COLUMNS; c++) {
                        const int x = c * WIDE;
                        const uint16_t *source = y >= shift[c] ? a + (y - shift[c]) * N : b + y * N;
                        memcpy(out + y * N + x, source + x, WIDE * sizeof(uint16_t));
                    }
                }
            }

            // Alternate squares open from their centres, then the rest.
            void checkerboard(float t, uint32_t, const uint16_t *a, const uint16_t *, uint16_t *out) {
                constexpr int CELL = 24, HALF = CELL / 2, CELLS = N / CELL;
                const int openFirst = (int) (clamp01(t * 2) * HALF + 0.5f);
                const int openSecond = (int) (clamp01(t * 2 - 1) * HALF + 0.5f);
                for (int y = 0; y < N; y++) {
                    const int cy = y / CELL, line = y % CELL;
                    for (int cx = 0; cx < CELLS; cx++) {
                        const int open = ((cx + cy) & 1) == 0 ? openFirst : openSecond;
                        const int x0 = cx * CELL;
                        if (line < HALF - open || line >= HALF + open) {
                            keepOld(out, a, y, x0, x0 + CELL);
                        } else {
                            keepOld(out, a, y, x0, x0 + HALF - open);
                            keepOld(out, a, y, x0 + HALF + open, x0 + CELL);
                        }
                    }
                }
            }

            // The old picture squashes to a bright line, like a TV switching
            // off, and the new one opens out from it.
            void scanline(float t, uint32_t, const uint16_t *a, const uint16_t *b, uint16_t *out) {
                const bool second = t >= 0.5f;
                const float half = (second ? 2 * t - 1 : 1 - 2 * t) * (N / 2);
                const uint16_t *source = second ? b : a;
                for (int y = 0; y < N; y++) {
                    const float fromCentre = fabsf(y - 119.5f);
                    uint16_t *row = out + y * N;
                    if (half < 2 && fromCentre < 2) {
                        memset(row, 0xFF, N * 2);
                    } else if (fromCentre < half) {
                        const int sy = constrain((int) ((y - (N / 2 - half)) / (2 * half) * N), 0, N - 1);
                        memcpy(row, source + sy * N, N * 2);
                    } else {
                        memset(row, 0, N * 2);
                    }
                }
            }

            // The new picture drops into place in columns.
            void rain(float t, uint32_t seed, const uint16_t *a, const uint16_t *b, uint16_t *out) {
                constexpr int WIDE = 4, COLUMNS = N / WIDE;
                int rows[COLUMNS];
                for (int c = 0; c < COLUMNS; c++) {
                    const float delay = (hash(seed + c * 7919U) & 0xFF) / 255.0f * 0.5f;
                    rows[c] = steps((t - delay) / 0.5f);
                }
                for (int y = 0; y < N; y++) {
                    for (int c = 0; c < COLUMNS; c++) {
                        const int x = c * WIDE;
                        const uint16_t *source = y < rows[c] ? b + (y + N - rows[c]) * N : a + y * N;
                        memcpy(out + y * N + x, source + x, WIDE * sizeof(uint16_t));
                    }
                }
            }

            // A clock hand sweeps round the screen, either way.
            void sweep(float t, uint32_t seed, const uint16_t *a, const uint16_t *, uint16_t *out) {
                constexpr int CELL = 4, CELLS = N / CELL;
                bool old[CELLS];
                for (int cy = 0; cy < CELLS; cy++) {
                    for (int cx = 0; cx < CELLS; cx++) {
                        float angle = atan2f(cx - 29.5f, 29.5f - cy) / (2 * (float) M_PI);
                        if (angle < 0) angle += 1;
                        old[cx] = ((seed & 1) ? 1 - angle : angle) >= t;
                    }
                    for (int y = cy * CELL; y < (cy + 1) * CELL; y++) {
                        for (int cx = 0; cx < CELLS; cx++) {
                            if (old[cx]) keepOld(out, a, y, cx * CELL, (cx + 1) * CELL);
                        }
                    }
                }
            }

            struct Effect {
                Render render;
                bool movesPixels; // reads the new picture while writing it: needs a copy
            };

            // In the same order as the names in CHOICES, after random and none.
            const Effect EFFECTS[] = {
                {crossfade, false}, {fadeThroughBlack, false}, {slide, true},    {wipe, false},
                {iris, false},      {blinds, false},           {mosaic, true},   {dissolve, false},
                {melt, true},       {checkerboard, false},     {scanline, true}, {rain, true},
                {sweep, false},
            };
            constexpr int COUNT = sizeof(EFFECTS) / sizeof(EFFECTS[0]);
            constexpr int FIRST_EFFECT = 2;

            struct State {
                uint32_t content = 0;
                uint16_t *from = nullptr; // kept once allocated, so PSRAM doesn't fragment
                bool playing = false;
                int effect = 0, last = -1;
                uint32_t seed = 0, startedAt = 0;
            };
            State states[display::NUM_SCREENS];
            uint16_t *scratch = nullptr; // one is enough: there's only one renderer

            uint16_t *allocate() { return (uint16_t *) heap_caps_malloc(BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

            // The effect a choice names, a random one, or -1 for none.
            int pickEffect(const char *choice, int last) {
                if (strcmp(choice, "random") == 0) {
                    int e;
                    do {
                        e = esp_random() % COUNT;
                    } while (e == last);
                    return e;
                }
                for (int i = FIRST_EFFECT; CHOICES[i]; i++) {
                    if (strcmp(choice, CHOICES[i]) == 0) return i - FIRST_EFFECT;
                }
                return -1;
            }

            void start(int screen, State &st, const char *choice) {
                st.playing = false;
                const int effect = pickEffect(choice, st.last);
                if (effect < 0) return;
                if (!st.from) st.from = allocate();
                if (!scratch) scratch = allocate();
                const uint16_t *shown = display::lastFrame(screen);
                if (!st.from || !scratch || !shown) return; // the new picture just appears
                // Mid-change this is the blend on screen, so a quick second
                // change carries on from what's visible.
                memcpy(st.from, shown, BYTES);
                st.effect = st.last = effect;
                st.seed = esp_random();
                st.startedAt = millis();
                st.playing = true;
            }
        }

        const char *const CHOICES[] = {"random",   "none", "crossfade",    "fade",     "slide", "wipe",  "iris", "blinds",
                                       "mosaic",   "dissolve", "melt",     "checkerboard", "scanline", "rain", "sweep", nullptr};
        static_assert(sizeof(CHOICES) / sizeof(CHOICES[0]) == COUNT + FIRST_EFFECT + 1, "a name for every effect");

        void apply(int screen, LGFX_Sprite &spr, uint32_t contentId, const char *choice) {
            State &st = states[screen];
            if (contentId != st.content) {
                const bool hadContent = st.content != 0;
                st.content = contentId;
                if (hadContent && contentId) start(screen, st, choice);
                else st.playing = false;
            }
            if (!st.playing) return;
            const uint32_t elapsed = millis() - st.startedAt;
            if (elapsed >= DURATION_MS) {
                st.playing = false; // this frame is the new picture, untouched
                return;
            }
            uint16_t *out = (uint16_t *) spr.getBuffer();
            const uint16_t *to = out;
            const Effect &effect = EFFECTS[st.effect];
            if (effect.movesPixels) {
                memcpy(scratch, out, BYTES);
                to = scratch;
            }
            effect.render((float) elapsed / DURATION_MS, st.seed, st.from, to, out);
        }

        void cancel(int screen) {
            states[screen].content = 0;
            states[screen].playing = false;
        }

        bool active(int screen) { return states[screen].playing; }

    }
}
