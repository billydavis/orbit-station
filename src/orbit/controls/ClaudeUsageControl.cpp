#include "ClaudeUsageControl.h"
#include "core/TimeKeeper.h"
#include "ui/Text.h"
#include <time.h>

namespace orbit {

    namespace {
        const char *const STYLES[] = {"pace", "orbit", "tanks", nullptr};
        const char *const LIVE[] = {"session", "week", "sessionResetsAt", "weekResetsAt"};

        // Colours from the mockup (docs/mockups/claude-monitor.html), as RGB565.
        constexpr uint16_t BG = TFT_BLACK;
        constexpr uint16_t ORANGE = 0xDBAA; // #d97757, the logo and the session
        constexpr uint16_t SAND = 0xEEF9;   // #eadfca, the week
        constexpr uint16_t AMBER = 0xF5A8;  // pace's verdict: ahead of pace
        constexpr uint16_t RED = 0xF289;    // pace's verdict: at a limit
        constexpr uint16_t GREEN = 0x8651;  // on pace
        constexpr uint16_t GREY = 0x738D;   // no reading
        constexpr uint16_t TRACK = 0x2944;
        constexpr uint16_t WHITE = 0xF7BD;
        constexpr uint16_t MUTED = 0xAD12;

        constexpr uint32_t SESSION_SECONDS = 5UL * 3600;
        constexpr uint32_t WEEK_SECONDS = 7UL * 24 * 3600;
        constexpr uint32_t ACTIVE_MS = 3UL * 60 * 1000; // animate this long after a reading changes
        constexpr uint32_t ANIMATE_MS = 66;             // about 15 fps
        constexpr uint32_t IDLE_MS = 1000;              // the countdowns and pace tick

        // The logo's twelve rays, as fractions of its radius. Uneven on purpose.
        constexpr float RAYS[12] = {1.0f, 0.78f, 0.93f, 0.72f, 0.98f, 0.8f, 0.9f, 0.7f, 1.0f, 0.76f, 0.94f, 0.82f};

        // One usage window as it should be shown now.
        struct Window {
            bool known = false;   // there's a reading
            float used = 0;       // percent, 0-100
            bool timed = false;   // its reset time is known and ahead
            bool renewed = false; // its reset time has passed since the reading
            uint32_t left = 0;    // seconds until it resets
            float elapsed = 0;    // fraction of the window gone, 0-1
        };

        Window window(JsonObjectConst p, const char *usedKey, const char *resetKey, uint32_t length, time_t now) {
            Window w;
            if (!p[usedKey].is<float>()) return w;
            w.known = true;
            w.used = constrain(p[usedKey].as<float>(), 0.0f, 100.0f);
            if (now <= 0 || !p[resetKey].is<uint32_t>()) return w;
            const int64_t left = (int64_t) p[resetKey].as<uint32_t>() - (int64_t) now;
            if (left <= 0) {
                w.used = 0;
                w.renewed = true;
                return w;
            }
            w.timed = true;
            w.left = (uint32_t) left;
            w.elapsed = constrain(1.0f - (float) left / length, 0.0f, 1.0f);
            return w;
        }

        // Each window keeps its own colour at any level; how full it is shows
        // well enough.
        uint16_t tone(const Window &w, uint16_t normal) { return w.known ? normal : GREY; }

        // "3d 6h", "2h 14m", "38m"; rounded up, so it never says 0m early.
        String duration(uint32_t seconds) {
            const uint32_t m = (seconds + 59) / 60;
            if (m >= 1440) return String(m / 1440) + "d " + String(m % 1440 / 60) + "h";
            if (m >= 60) return String(m / 60) + "h " + String(m % 60) + "m";
            return String(m) + "m";
        }

        String note(const Window &w) {
            if (!w.known) return "no data";
            if (w.renewed) return "new window";
            if (w.timed) return "resets " + duration(w.left);
            return "";
        }

        // The Claude spark: tapered rays around (x, y), turned spin degrees.
        // breathe sets them gently pulsing, each ray out of step with the next.
        void logo(LGFX_Sprite &spr, int x, int y, float r, uint16_t color, float seconds, bool breathe, float spin = 0) {
            for (int i = 0; i < 12; i++) {
                const float a = (i * 30 + 7 + spin) * DEG_TO_RAD;
                const float len = r * RAYS[i] * (breathe ? 1.0f + 0.08f * sinf(seconds * 3.2f + i * 1.7f) : 1.0f);
                const float s = sinf(a), c = cosf(a), base = r * 0.08f;
                spr.drawWedgeLine(x + lroundf(base * s), y - lroundf(base * c), x + lroundf(len * s), y - lroundf(len * c),
                                  r * 0.105f, r * 0.045f, color);
            }
        }

        String whole(float v) { return String((int) lroundf(v)); }

        // ---- pace ----
        constexpr int BAR_X = 28, BAR_W = 184, BAR_H = 8;
        constexpr int LOGO_X = 83, LOGO_Y = 43, LOGO_R = 16;

        // Everything the pace layout shows apart from the logo, worked out
        // first so it can tell whether the cached layer is still right.
        struct PaceRow {
            String percent, note;
            uint16_t color;
            int fill; // pixels
            int tick; // pixels along the bar, or -1
        };
        struct Pace {
            PaceRow rows[2];
            String verdict;
            uint16_t verdictColor;

            String key() const {
                String k = verdict;
                k += '|';
                k += verdictColor;
                for (const PaceRow &r : rows) {
                    k += '|';
                    k += r.percent;
                    k += '|';
                    k += r.note;
                    k += '|';
                    k += r.color;
                    k += '|';
                    k += r.fill;
                    k += '|';
                    k += r.tick;
                }
                return k;
            }
        };

        PaceRow paceRow(const Window &w, uint16_t normal) {
            PaceRow r;
            r.percent = w.known ? whole(w.used) + "%" : String("--");
            r.note = note(w);
            r.color = tone(w, normal);
            r.fill = w.known ? lroundf(BAR_W * w.used / 100.0f) : 0;
            // Where usage would be if it were spread evenly over the window.
            r.tick = w.timed ? lroundf(BAR_W * w.elapsed) : -1;
            return r;
        }

        Pace pace(const Window &session, const Window &week) {
            Pace v;
            v.rows[0] = paceRow(session, ORANGE);
            v.rows[1] = paceRow(week, SAND);
            v.verdictColor = GREEN;
            if (!session.known && !week.known) {
                v.verdict = "Waiting for data", v.verdictColor = GREY;
            } else if (session.known && session.used >= 99.5f) {
                v.verdict = "Session limit reached", v.verdictColor = RED;
            } else if (week.known && week.used >= 99.5f) {
                v.verdict = "Weekly limit reached", v.verdictColor = RED;
            } else if (week.timed) {
                // Percentage points between the week's use and an even spread.
                const int diff = lroundf(week.used - week.elapsed * 100.0f);
                if (diff > 8) v.verdict = "Week " + String(diff) + "% ahead of pace", v.verdictColor = AMBER;
                else if (diff < -8) v.verdict = String(-diff) + "% under weekly pace";
                else v.verdict = "On pace this week";
            }
            return v;
        }

        void paintRow(LGFX_Sprite &spr, int y, const char *label, const PaceRow &r) {
            text::draw(spr, label, BAR_X, y, 9, Align::MiddleLeft, WHITE, BG);
            text::draw(spr, r.percent, BAR_X + BAR_W, y - 2, 20, Align::MiddleRight, r.color, BG);
            spr.fillRect(BAR_X, y + 18, BAR_W, BAR_H, TRACK);
            if (r.fill > 0) spr.fillRect(BAR_X, y + 18, r.fill, BAR_H, r.color);
            if (r.tick >= 0) spr.fillRect(BAR_X + r.tick - 1, y + 14, 2, 16, WHITE);
            if (r.note.length()) text::draw(spr, r.note, BAR_X, y + 39, 8, Align::MiddleLeft, MUTED, BG);
        }

        // All of it but the logo. Text is slow to draw (5-15 ms a string), so
        // this goes into a cached layer that's only repainted when it changes.
        void paintPace(LGFX_Sprite &spr, const Pace &v) {
            spr.fillScreen(BG);
            text::draw(spr, "Claude", 105, 44, 17, Align::MiddleLeft, WHITE, BG);
            paintRow(spr, 83, "SESSION", v.rows[0]);
            paintRow(spr, 143, "WEEK", v.rows[1]);
            if (v.verdict.length()) text::draw(spr, v.verdict, 120, 209, 9, Align::MiddleCenter, v.verdictColor, BG);
        }

        // ---- orbit ----
        // Two rings round the edge, week outside, session inside, each with a
        // planet where its usage has got to.
        constexpr int CENTRE = 120;
        constexpr int RING_R[2] = {99, 112}; // session, week
        constexpr int ORBIT_LOGO_Y = 76, ORBIT_LOGO_R = 27;
        constexpr float SPIN_DEG_PER_S = 14;

        struct OrbitRing {
            String percent, note;
            uint16_t color;
            bool known;
            int sweep; // degrees clockwise from 12 o'clock
        };
        struct Orbit {
            OrbitRing rings[2]; // session, week

            String key() const {
                String k;
                for (const OrbitRing &r : rings) {
                    k += r.percent;
                    k += '|';
                    k += r.note;
                    k += '|';
                    k += r.color;
                    k += '|';
                    k += r.sweep;
                    k += '|';
                }
                return k;
            }
        };

        OrbitRing orbitRing(const Window &w, uint16_t normal) {
            OrbitRing r;
            r.known = w.known;
            r.percent = w.known ? whole(w.used) + "%" : String("--");
            r.color = tone(w, normal);
            r.sweep = w.known ? lroundf(w.used * 3.6f) : 0;
            if (!w.known) r.note = "no data";
            else if (w.renewed) r.note = "new window";
            else if (w.timed) r.note = duration(w.left);
            return r;
        }

        Orbit orbit(const Window &session, const Window &week) {
            Orbit o;
            o.rings[0] = orbitRing(session, ORANGE);
            o.rings[1] = orbitRing(week, SAND);
            return o;
        }

        // A point on a ring, angle in degrees clockwise from 12 o'clock.
        void onRing(float angle, float radius, int &x, int &y) {
            const float a = angle * DEG_TO_RAD;
            x = CENTRE + lroundf(radius * sinf(a));
            y = CENTRE - lroundf(radius * cosf(a));
        }

        // fg over bg, k from 0 (all bg) to 256 (all fg), both RGB565.
        uint16_t blend256(uint16_t fg, uint16_t bg, int k) {
            const int r = ((bg >> 11) * (256 - k) + (fg >> 11) * k) >> 8;
            const int g = (((bg >> 5) & 63) * (256 - k) + ((fg >> 5) & 63) * k) >> 8;
            const int b = ((bg & 31) * (256 - k) + (fg & 31) * k) >> 8;
            return (r << 11) | (g << 5) | b;
        }

        // The same with a from 0 to 1. a is never negative here, so adding
        // a half and truncating rounds it.
        uint16_t blend(uint16_t fg, uint16_t bg, float a) { return blend256(fg, bg, (int) (a * 256 + 0.5f)); }

        // An anti-aliased arc round the centre, radius R and half a line's
        // width thick, from 12 o'clock clockwise through sweep degrees, with
        // round ends (360 or more is a whole ring). Each pixel's cover comes
        // from its distance to the arc, so it's slow (a few ms); it only runs
        // when the cached layer is repainted.
        void smoothArc(LGFX_Sprite &spr, float R, float half, float sweep, uint16_t color) {
            if (sweep <= 0) return;
            const bool whole = sweep >= 360;
            const float reach = half + 1.0f;
            const float outer2 = (R + reach) * (R + reach), inner2 = R > reach ? (R - reach) * (R - reach) : 0;
            const float a = sweep * DEG_TO_RAD;
            const float endX = R * sinf(a), endY = -R * cosf(a); // the start is (0, -R)
            const int span = ceilf(R + reach);
            for (int dy = -span; dy <= span; dy++) {
                for (int dx = -span; dx <= span; dx++) {
                    const float d2 = dx * dx + dy * dy;
                    if (d2 > outer2 || d2 < inner2) continue;
                    float dist = fabsf(sqrtf(d2) - R);
                    if (!whole) {
                        float angle = atan2f(dx, -dy);
                        if (angle < 0) angle += 2 * PI;
                        if (angle > a) { // past either end: measure to the nearer round cap
                            const float s = hypotf(dx, dy + R), e = hypotf(dx - endX, dy - endY);
                            dist = fminf(s, e);
                        }
                    }
                    const float cover = half + 0.5f - dist;
                    if (cover <= 0) continue;
                    const int x = CENTRE + dx, y = CENTRE + dy;
                    spr.drawPixel(x, y, cover >= 1 ? color : blend(color, spr.readPixel(x, y), cover));
                }
            }
        }

        void paintOrbit(LGFX_Sprite &spr, const Orbit &o) {
            spr.fillScreen(BG);
            const int column[2] = {78, 162};
            const char *const label[2] = {"SESSION", "WEEK"};
            for (int i = 0; i < 2; i++) {
                const OrbitRing &r = o.rings[i];
                const int R = RING_R[i];
                smoothArc(spr, R, 1.5f, 360, TRACK);
                if (r.known) smoothArc(spr, R, 2.5f, r.sweep, r.color);
                text::draw(spr, r.percent, column[i], 132, 25, Align::MiddleCenter, r.color, BG);
                text::draw(spr, label[i], column[i], 158, 8, Align::MiddleCenter, WHITE, BG);
                if (r.note.length()) text::draw(spr, r.note, column[i], 175, 9, Align::MiddleCenter, MUTED, BG);
            }
        }

        // The planets go on top of the cached rings every frame; they pulse
        // while the readings are changing.
        void orbitPlanets(LGFX_Sprite &spr, const Orbit &o, float seconds, bool moving) {
            for (int i = 0; i < 2; i++) {
                const OrbitRing &r = o.rings[i];
                if (!r.known) continue;
                const float pulse = moving ? 1.0f + 0.22f * sinf(seconds * 4.0f + i * 2.0f) : 1.0f;
                int x, y;
                onRing(r.sweep, RING_R[i], x, y);
                spr.drawSpot(x, y, 6.0f * pulse, WHITE);
                spr.drawSpot(x, y, 3.2f, r.color);
            }
        }

        // ---- tanks ----
        // Session (left) and week (right) fill like two tanks of liquid; the
        // surfaces ripple while the readings are changing. The text sits over
        // the liquid, so the layer keeps an alpha mask of it: each frame only
        // the rows round each surface are redrawn, with the text put back
        // from the mask.
        constexpr int W = display::WIDTH;
        constexpr int TANK_LOGO_Y = 54, TANK_LOGO_R = 20, DISC_R = 25;
        constexpr int TANK_COLUMN[2] = {62, 178};
        constexpr float RIPPLE = 2.6f; // pixels either way
        // The rows the text can reach (the percent, the label and the note).
        constexpr int TEXT_TOP = 90, TEXT_BOTTOM = 190;

        // The liquid itself is a darker version of each window's colour.
        constexpr uint16_t SESSION_LIQUID = 0x5984; // #5c3024
        constexpr uint16_t WEEK_LIQUID = 0x4A27;    // #4b463c
        constexpr uint16_t GREY_LIQUID = 0x2924;    // #282725

        struct Tank {
            String percent, note;
            uint16_t line, fill;
            int level; // y of the surface
            bool full;
        };
        struct Tanks {
            Tank tanks[2]; // session, week

            String key() const {
                String k;
                for (const Tank &t : tanks) {
                    k += t.percent;
                    k += '|';
                    k += t.note;
                    k += '|';
                    k += t.line;
                    k += '|';
                    k += t.fill;
                    k += '|';
                    k += t.level;
                    k += '|';
                }
                return k;
            }
        };

        Tank tank(const Window &w, uint16_t normal, uint16_t liquid) {
            Tank t;
            t.percent = w.known ? whole(w.used) + "%" : String("--");
            t.line = tone(w, normal);
            t.fill = w.known ? liquid : GREY_LIQUID;
            t.level = lroundf(236 - (w.known ? w.used : 0) * 2.32f);
            t.full = w.known && w.used >= 99.5f;
            if (!w.known) t.note = "no data";
            else if (w.renewed) t.note = "new window";
            else if (w.timed) t.note = duration(w.left);
            return t;
        }

        Tanks tanks(const Window &session, const Window &week) {
            Tanks v;
            v.tanks[0] = tank(session, ORANGE, SESSION_LIQUID);
            v.tanks[1] = tank(week, SAND, WEEK_LIQUID);
            return v;
        }

        float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

        // The liquid and the 2 px line along its surface, d pixels below the
        // surface (negative is above it). Away from the surface it's plain
        // liquid or plain background, with no blending.
        uint16_t liquidAt(const Tank &t, float d) {
            if (d >= 1.5f) return t.fill;
            if (d <= -1.5f) return BG;
            const uint16_t c = blend(t.fill, BG, clamp01(d + 0.5f));
            return blend(t.line, c, clamp01(1.5f - fabsf(d)));
        }

        // Inside the dial: the last pixel either side of the centre on row y.
        int dialHalf(int y) {
            const int cy = y - CENTRE, r2 = 119 * 119 - cy * cy;
            if (r2 < 0) return -1; // the row misses the dial
            int h = (int) sqrtf((float) r2);
            while (h * h > r2) h--;
            while ((h + 1) * (h + 1) <= r2) h++;
            return h;
        }

        // The logo's disc over c, which hides the liquid behind the logo.
        uint16_t underDisc(uint16_t c, int x, int y) {
            const int lx = x - CENTRE, ly = y - TANK_LOGO_Y, l2 = lx * lx + ly * ly;
            if (l2 >= (DISC_R + 1) * (DISC_R + 1)) return c;
            if (l2 * 4 <= (2 * DISC_R - 1) * (2 * DISC_R - 1)) return BG; // a whole pixel inside
            return blend(BG, c, clamp01(DISC_R + 0.5f - sqrtf(l2)));
        }

        // One pixel of the tanks without the text, surface being where this
        // column's surface is (it moves when rippling).
        uint16_t tankPixel(const Tank &t, int x, int y, float surface) {
            const int cx = x - CENTRE, cy = y - CENTRE;
            if (cx * cx + cy * cy > 119 * 119) return BG;
            return underDisc(liquidAt(t, y - surface), x, y);
        }

        uint16_t withText(uint16_t c, uint8_t alpha) {
            return alpha ? blend256(WHITE, c, (alpha * 256 + 127) / 255) : c;
        }

        // The whole picture, surfaces flat. When mask is given it's filled
        // with the text's alpha for the ripple to use. With the surfaces
        // flat, a tank's colour only changes along a row at the dial's edge
        // and the logo's disc, so each row is worked out once.
        void paintTanks(LGFX_Sprite &spr, const Tanks &v, uint8_t *mask) {
            // The text first, white on black, so its alpha can be read back.
            // Only its band is cleared and read back: the rest of the layer
            // is written over whole by the pass below.
            spr.fillRect(0, TEXT_TOP, W, TEXT_BOTTOM - TEXT_TOP + 1, BG);
            spr.setClipRect(0, TEXT_TOP, W, TEXT_BOTTOM - TEXT_TOP + 1);
            for (int i = 0; i < 2; i++) {
                const Tank &t = v.tanks[i];
                text::draw(spr, t.percent, TANK_COLUMN[i], 122, 25, Align::MiddleCenter, TFT_WHITE, BG);
                text::draw(spr, i ? "WEEK" : "SESSION", TANK_COLUMN[i], 149, 8, Align::MiddleCenter, TFT_WHITE, BG);
                if (t.note.length()) text::draw(spr, t.note, TANK_COLUMN[i], 167, 9, Align::MiddleCenter, TFT_WHITE, BG);
            }
            spr.clearClipRect();
            // Each row is built in internal RAM, byte-swapped as the sprite
            // holds it: plain runs first (background, session's tank, the
            // gap, week's tank, background), then blending only round the
            // logo's disc and where there's text.
            static uint16_t line[W];
            static uint8_t maskLine[W];
            const uint16_t bg = __builtin_bswap16(BG);
            uint16_t *buf = (uint16_t *) spr.getBuffer();
            for (int y = 0; y < W; y++) {
                const bool hasText = y >= TEXT_TOP && y <= TEXT_BOTTOM;
                if (hasText) {
                    memcpy(line, buf + y * W, sizeof line);
                    for (int x = 0; x < W; x++) maskLine[x] = ((__builtin_bswap16(line[x]) >> 5) & 63) * 255 / 63; // green: 6 bits
                }

                const int half = dialHalf(y), left = CENTRE - half, right = CENTRE + half;
                for (int x = 0; x < W; x++) line[x] = bg;
                for (int i = 0; i < 2; i++) {
                    const uint16_t c = __builtin_bswap16(liquidAt(v.tanks[i], y - v.tanks[i].level));
                    const int from = max(left, i ? 122 : 0), to = min(right, i ? W - 1 : 118);
                    for (int x = from; x <= to; x++) line[x] = c;
                }
                if (abs(y - TANK_LOGO_Y) <= DISC_R) {
                    for (int x = CENTRE - DISC_R; x <= CENTRE + DISC_R; x++)
                        if (line[x] != bg) line[x] = __builtin_bswap16(underDisc(__builtin_bswap16(line[x]), x, y));
                }
                if (hasText) {
                    for (int x = 0; x < W; x++)
                        if (maskLine[x]) line[x] = __builtin_bswap16(withText(__builtin_bswap16(line[x]), maskLine[x]));
                }

                memcpy(buf + y * W, line, sizeof line);
                if (mask) {
                    if (hasText) memcpy(mask + y * W, maskLine, sizeof maskLine);
                    else memset(mask + y * W, 0, W);
                }
            }
        }

        // Redraws the rows round each surface with the ripple in them.
        void rippleTanks(LGFX_Sprite &spr, const Tanks &v, const uint8_t *mask, float seconds) {
            uint16_t *buf = (uint16_t *) spr.getBuffer();
            for (int i = 0; i < 2; i++) {
                const Tank &t = v.tanks[i];
                if (t.full) continue; // a full tank lies still
                const int x0 = i ? 122 : 0, x1 = i ? W - 1 : 118;
                const int y0 = max(0, (int) floorf(t.level - RIPPLE - 2)), y1 = min(W - 1, (int) ceilf(t.level + RIPPLE + 2));
                for (int x = x0; x <= x1; x++) {
                    const float surface = t.level + RIPPLE * sinf(x * 0.075f + seconds * 2.4f + i * 2.0f);
                    for (int y = y0; y <= y1; y++)
                        buf[y * W + x] = __builtin_bswap16(withText(tankPixel(t, x, y, surface), mask[y * W + x]));
                }
            }
        }

        // A reading: absent is fine (no reading yet), otherwise a number.
        int optionalReading(JsonObjectConst in, JsonObject out, const char *key, String &error) {
            JsonVariantConst v = in[key];
            if (v.isNull()) return 0;
            if (!v.is<float>() || !isfinite(v.as<float>())) {
                error = String("params.") + key + " must be a number";
                return 400;
            }
            out[key] = v.as<float>();
            return 0;
        }

        // A reset time in whole Unix seconds; a float would lose minutes.
        int optionalEpoch(JsonObjectConst in, JsonObject out, const char *key, String &error) {
            JsonVariantConst v = in[key];
            if (v.isNull()) return 0;
            if (!v.is<uint32_t>()) {
                error = String("params.") + key + " must be whole Unix seconds";
                return 400;
            }
            out[key] = v.as<uint32_t>();
            return 0;
        }
    }

    int ClaudeUsageControl::parse(JsonObjectConst in, JsonObject out, String &error) const {
        int status;
        if ((status = optionalChoice(in, out, "style", STYLES, error))) return status;
        if ((status = optionalBool(in, out, "animate", true, error))) return status;
        if ((status = optionalReading(in, out, "session", error))) return status;
        if ((status = optionalReading(in, out, "week", error))) return status;
        if ((status = optionalEpoch(in, out, "sessionResetsAt", error))) return status;
        return optionalEpoch(in, out, "weekResetsAt", error);
    }

    void ClaudeUsageControl::stripLive(JsonObject params) const {
        for (const char *k : LIVE) params.remove(k);
    }

    AfterWrite ClaudeUsageControl::applied(int screen, JsonObjectConst, JsonObjectConst stored) {
        Activity &a = activity[screen];
        if (!stored["session"].is<float>() && !stored["week"].is<float>()) return AfterWrite::Keep;
        const float session = stored["session"] | -1.0f, week = stored["week"] | -1.0f;
        if (a.seen && (session != a.session || week != a.week)) {
            a.changed = true;
            a.changedAt = millis();
        }
        a.seen = true;
        a.session = session;
        a.week = week;
        return AfterWrite::Keep;
    }

    // GET also says whether the readings are moving (the logo animates then).
    void ClaudeUsageControl::report(int screen, JsonObject params) {
        params["changing"] = active(screen, millis());
    }

    bool ClaudeUsageControl::active(int screen, uint32_t now) const {
        const Activity &a = activity[screen];
        return a.changed && now - a.changedAt < ACTIVE_MS;
    }

    uint32_t ClaudeUsageControl::refreshMs(int screen, JsonObjectConst p) const {
        return (p["animate"] | true) && active(screen, millis()) ? ANIMATE_MS : IDLE_MS;
    }

    void ClaudeUsageControl::draw(LGFX_Sprite &spr, JsonObjectConst p, const DrawContext &ctx) {
        const time_t now = timekeeper::isSet() ? time(nullptr) : 0;
        const Window session = window(p, "session", "sessionResetsAt", SESSION_SECONDS, now);
        const Window week = window(p, "week", "weekResetsAt", WEEK_SECONDS, now);
        const bool moving = (p["animate"] | true) && active(ctx.screen, ctx.nowMs);
        const char *style = p["style"] | "pace";
        const bool orbitStyle = strcmp(style, "orbit") == 0, tankStyle = strcmp(style, "tanks") == 0;

        // What the layer should show; the style is part of the key.
        Pace pv;
        Orbit ov;
        Tanks tv;
        String key = style;
        key += '|';
        if (orbitStyle) {
            ov = orbit(session, week);
            key += ov.key();
        } else if (tankStyle) {
            tv = tanks(session, week);
            key += tv.key();
        } else {
            pv = pace(session, week);
            key += pv.key();
        }
        uint8_t *textMask = tankStyle ? ensureMask(ctx.screen) : nullptr;
        auto paint = [&](LGFX_Sprite &s) {
            if (orbitStyle) paintOrbit(s, ov);
            else if (tankStyle) paintTanks(s, tv, textMask);
            else paintPace(s, pv);
        };
        if (ensureLayer(ctx.screen)) {
            if (key != layerKey[ctx.screen]) {
                paint(layer[ctx.screen]);
                layerKey[ctx.screen] = key;
            }
            memcpy(spr.getBuffer(), layer[ctx.screen].getBuffer(), display::WIDTH * display::HEIGHT * 2);
        } else {
            textMask = nullptr; // no layer: no ripple, since the frame starts from scratch
            paint(spr);         // slower, but the same picture
        }

        // The time is wrapped so the float keeps its precision on a board
        // that's been up for weeks; the jump once an hour is a single frame.
        const float seconds = (ctx.nowMs % 3600000UL) / 1000.0f;
        const uint16_t logoColor = !session.known && !week.known ? GREY : ORANGE;
        if (orbitStyle) {
            orbitPlanets(spr, ov, seconds, moving);
            logo(spr, CENTRE, ORBIT_LOGO_Y, ORBIT_LOGO_R, logoColor, seconds, moving, moving ? seconds * SPIN_DEG_PER_S : 0);
        } else if (tankStyle) {
            if (moving && textMask) rippleTanks(spr, tv, textMask, seconds);
            logo(spr, CENTRE, TANK_LOGO_Y, TANK_LOGO_R, logoColor, seconds, moving);
        } else {
            logo(spr, LOGO_X, LOGO_Y, LOGO_R, logoColor, seconds, moving);
        }
    }

    uint8_t *ClaudeUsageControl::ensureMask(int screen) {
        if (!mask[screen]) {
            mask[screen] = (uint8_t *) heap_caps_malloc(display::WIDTH * display::HEIGHT, MALLOC_CAP_SPIRAM);
            layerKey[screen] = ""; // repaint, so the new mask gets filled
        }
        return mask[screen];
    }

    bool ClaudeUsageControl::ensureLayer(int screen) {
        LGFX_Sprite &l = layer[screen];
        if (l.getBuffer()) return true;
        l.setColorDepth(16);
        l.setPsram(true);
        layerKey[screen] = ""; // nothing painted yet
        return l.createSprite(display::WIDTH, display::HEIGHT) != nullptr;
    }

}
