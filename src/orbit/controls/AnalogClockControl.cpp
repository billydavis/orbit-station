#include "AnalogClockControl.h"
#include "core/Health.h"
#include "core/TimeKeeper.h"
#include "makapix/Artwork.h"
#include "orbit/Orbit.h"
#include "ui/Smooth.h"
#include "ui/Text.h"
#include <LittleFS.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>
#include <sys/time.h>

namespace orbit {

    const char *const AnalogClockControl::CLOCK_FACES[] = {"classic", "platform", "minimal", nullptr};

    namespace {
        constexpr int CENTRE = 120;
        constexpr float FACE_RADIUS = 110; // inside the round bezel's visible area; 1 in clock face units
        // The face's centre in continuous pixel coordinates: the middle of
        // pixel (120, 120), as smooth::arc draws round.
        constexpr float MID = CENTRE + 0.5f;

        const char *const MOTIONS[] = {"tick", "sweep", nullptr};
        constexpr uint32_t TICK_MS = 1000, SWEEP_MS = 100; // the sweep runs at about 10 fps

        // #rrggbb as RGB565.
        constexpr uint16_t rgb(uint32_t hex) {
            return ((hex >> 8) & 0xF800) | ((hex >> 5) & 0x07E0) | ((hex >> 3) & 0x001F);
        }

        // ---- shapes ----
        // An outline and up to two discs, in clock face units (0 is the
        // centre, 1 the face's radius, 110 px), pointing at 12 o'clock: x
        // across (right is positive), y along (outwards is positive).
        struct Disc {
            float along, radius;
        };
        struct Shape {
            uint8_t points = 0;
            float xy[smooth::MAX_POINTS][2];
            uint8_t discs = 0;
            Disc disc[2];
        };

        constexpr float unit(float px) { return px / FACE_RADIUS; }

        // A straight bar from r0 to r1 pixels along, half wide either side.
        Shape bar(float r0, float r1, float half) {
            Shape h;
            const float pts[4][2] = {{-half, r0}, {-half, r1}, {half, r1}, {half, r0}};
            for (const auto &p : pts) {
                h.xy[h.points][0] = unit(p[0]);
                h.xy[h.points][1] = unit(p[1]);
                h.points++;
            }
            return h;
        }

        // The mockup's baton: tapered from half0 to half1, round at both ends.
        Shape baton(float r0, float r1, float half0, float half1) {
            Shape h = bar(r0, r1, half0);
            h.xy[1][0] = unit(-half1);
            h.xy[2][0] = unit(half1);
            h.disc[h.discs++] = {unit(r0), unit(half0)};
            h.disc[h.discs++] = {unit(r1), unit(half1)};
            return h;
        }

        Shape withDisc(Shape h, float along, float radius) {
            h.disc[h.discs++] = {unit(along), unit(radius)};
            return h;
        }

        // A triangle pointing out: tip at r1 pixels, base at r0, half wide.
        Shape arrow(float r0, float r1, float half) {
            Shape h;
            const float pts[3][2] = {{-half, r0}, {0, r1}, {half, r0}};
            for (const auto &p : pts) {
                h.xy[h.points][0] = unit(p[0]);
                h.xy[h.points][1] = unit(p[1]);
                h.points++;
            }
            return h;
        }

    }

    // The pixels the hands covered on each row of one picture, x0 to x1
    // (none when x0 > x1), so the next frame drawn into that picture puts
    // back only those from the layer.
    struct AnalogClockControl::Spans {
        // Up to three stretches a row: a seconds ring crosses most rows at
        // both ends, with the hands in between.
        static constexpr int PER_ROW = 3;
        uint8_t count[display::HEIGHT];
        uint8_t x0[display::HEIGHT][PER_ROW], x1[display::HEIGHT][PER_ROW];

        void clear() { memset(count, 0, sizeof count); }

        // Row y from a to b, a pixel wider either side for the smooth edges.
        void add(int y, float a, float b) {
            if (y < 0 || y >= display::HEIGHT) return;
            const int lo = max(0, (int) floorf(fminf(a, b)) - 1), hi = min(display::WIDTH - 1, (int) floorf(fmaxf(a, b)) + 1);
            if (lo <= hi) insert(y, lo, hi);
        }

        // Joins the stretches it overlaps or touches; with no room for
        // another, joins the nearest one.
        void insert(int y, int lo, int hi) {
            uint8_t &n = count[y];
            uint8_t *l = x0[y], *h = x1[y];
            for (int i = 0; i < n;) {
                if (lo <= h[i] + 1 && hi + 1 >= l[i]) {
                    lo = min<int>(lo, l[i]), hi = max<int>(hi, h[i]);
                    l[i] = l[n - 1], h[i] = h[n - 1];
                    n--;
                    i = 0; // the joined stretch may now reach another
                } else {
                    i++;
                }
            }
            if (n < PER_ROW) {
                l[n] = lo, h[n] = hi;
                n++;
                return;
            }
            int nearest = 0, gap = INT32_MAX;
            for (int i = 0; i < n; i++) {
                const int g = lo > h[i] ? lo - h[i] : l[i] - hi;
                if (g < gap) gap = g, nearest = i;
            }
            const int nl = min<int>(lo, l[nearest]), nh = max<int>(hi, h[nearest]);
            l[nearest] = l[n - 1], h[nearest] = h[n - 1];
            n--;
            insert(y, nl, nh);
        }

        // A polygon covers, on each row, everything from the leftmost to the
        // rightmost place its edges cross that row's strip. Not each edge's
        // crossing on its own: a thin hand lying nearly flat has its two
        // long edges cross a row several pixels apart, with the filled
        // inside between them.
        void addPolygon(const float *xy, int n) {
            float minY = xy[1], maxY = xy[1];
            for (int i = 1; i < n; i++) minY = fminf(minY, xy[2 * i + 1]), maxY = fmaxf(maxY, xy[2 * i + 1]);
            const int yTop = max(0, (int) floorf(minY)), yBottom = min(display::HEIGHT - 1, (int) floorf(maxY));
            for (int y = yTop; y <= yBottom; y++) {
                float lo = INFINITY, hi = -INFINITY;
                for (int i = 0, j = n - 1; i < n; j = i++) {
                    float ax = xy[2 * j], ay = xy[2 * j + 1], bx = xy[2 * i], by = xy[2 * i + 1];
                    if (ay > by) std::swap(ax, bx), std::swap(ay, by);
                    if (by < y || ay > y + 1) continue; // misses this row
                    float a = ax, b = bx;
                    if (by - ay >= 1e-3f) {
                        const float t0 = fmaxf(0, (y - ay) / (by - ay)), t1 = fminf(1, (y + 1 - ay) / (by - ay));
                        a = ax + (bx - ax) * t0, b = ax + (bx - ax) * t1;
                    }
                    lo = fminf(lo, fminf(a, b)), hi = fmaxf(hi, fmaxf(a, b));
                }
                if (lo <= hi) add(y, lo, hi);
            }
        }

        void addDisc(float cx, float cy, float r) {
            for (int y = (int) floorf(cy - r - 1); y <= (int) floorf(cy + r + 1); y++) add(y, cx - r - 1, cx + r + 1);
        }

        // The whole band between radii inner and outer round (cx, cy): on
        // each row, its left and right pieces (or one across the top and
        // bottom, where the band has no hole).
        void addRing(float cx, float cy, float inner, float outer) {
            for (int y = (int) floorf(cy - outer); y <= (int) floorf(cy + outer); y++) {
                const float a = fabsf(y - cy), b = fabsf(y + 1 - cy);
                const float near = (cy >= y && cy <= y + 1) ? 0 : fminf(a, b), far = fmaxf(a, b);
                if (outer * outer <= near * near) continue;
                const float xo = sqrtf(outer * outer - near * near);
                if (inner * inner <= far * far) {
                    add(y, cx - xo, cx + xo);
                } else {
                    const float xi = sqrtf(inner * inner - far * far);
                    add(y, cx - xo, cx - xi);
                    add(y, cx + xi, cx + xo);
                }
            }
        }
    };

    struct AnalogClockControl::Frames {
        Spans spans[2];
        uint32_t layerGen[2] = {};
        bool valid[2] = {};
    };

    namespace {
        using Spans = AnalogClockControl::Spans;

        // Draws a shape turned angle degrees clockwise from 12 o'clock, and
        // adds what it covers to spans if given.
        void drawShape(LGFX_Sprite &spr, const Shape &h, float angle, uint16_t color, Spans *spans = nullptr) {
            const float a = angle * DEG_TO_RAD, s = sinf(a), c = cosf(a);
            if (h.points) {
                float xy[smooth::MAX_POINTS * 2];
                for (int i = 0; i < h.points; i++) {
                    const float across = h.xy[i][0] * FACE_RADIUS, along = h.xy[i][1] * FACE_RADIUS;
                    xy[2 * i] = MID + along * s + across * c;
                    xy[2 * i + 1] = MID - along * c + across * s;
                }
                smooth::polygon(spr, xy, h.points, color);
                if (spans) spans->addPolygon(xy, h.points);
            }
            for (int i = 0; i < h.discs; i++) {
                const float along = h.disc[i].along * FACE_RADIUS, r = h.disc[i].radius * FACE_RADIUS;
                const float x = MID + along * s, y = MID - along * c;
                smooth::disc(spr, x, y, r, color);
                if (spans) spans->addDisc(x, y, r);
            }
        }

        // A shape with its own colour; a hand is a few of them, drawn in order.
        constexpr int MAX_PARTS = 4;
        struct Part {
            Shape shape;
            uint16_t color;
        };
        struct Parts {
            uint8_t count = 0;
            Part part[MAX_PARTS];
        };

        void drawParts(LGFX_Sprite &spr, const Parts &p, float angle, Spans *spans = nullptr) {
            for (int i = 0; i < p.count; i++) drawShape(spr, p.part[i].shape, angle, p.part[i].color, spans);
        }

        // ---- time ----
        // The clock's time: local to tz ("" for the board's own timezone),
        // with the millisecond for a sweeping second hand. One reading of
        // the clock, so the second and the millisecond agree.
        struct Now {
            struct tm t;
            uint32_t ms;
            time_t utc;
        };

        bool clockNow(const char *tz, Now &n) {
            if (!timekeeper::isSet()) return false;
            struct timeval tv;
            gettimeofday(&tv, nullptr);
            n.ms = tv.tv_usec / 1000;
            n.utc = tv.tv_sec;
            if (*tz) return timekeeper::localIn(tz, n.utc, n.t);
            localtime_r(&n.utc, &n.t);
            return true;
        }

        // Where the hands point, in degrees clockwise from 12 o'clock.
        struct Angles {
            float hour, minute, second;
        };

        Angles anglesAt(const Now &n, bool sweep) {
            const float sec = n.t.tm_sec + (sweep ? n.ms / 1000.0f : 0);
            return {(n.t.tm_hour % 12) * 30.0f + n.t.tm_min * 0.5f + sec / 120.0f, n.t.tm_min * 6.0f + sec * 0.1f, sec * 6.0f};
        }

        // The second zone's hand goes round once a day.
        float zoneAngle(const struct tm &t) { return (t.tm_hour + t.tm_min / 60.0f) * 15.0f; }

        // "America/New_York" -> "NEW YORK", "UTC" -> "UTC".
        String zoneName(const String &tz) {
            String name = tz.substring(tz.lastIndexOf('/') + 1);
            name.replace('_', ' ');
            name.toUpperCase();
            return name;
        }

        const char *const WEEKDAYS[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};

        // ---- what's written on the dial ----
        // Positions in pixels of the 240x240 picture (the middle of the text
        // or window), sizes as the font's pixel size.
        struct TextSpot {
            bool on = false;
            int16_t x = CENTRE, y = CENTRE;
            uint8_t px = 12;
            uint16_t color = 0xFFFF;
        };
        struct DateWindow {
            TextSpot text;
            uint8_t w = 30, h = 22;
            bool filled = false, bordered = false;
            uint16_t background = 0, border = 0;
        };

        // A ring round the rim that fills with the seconds, from 12 o'clock
        // to where the second hand is (the mockup's: radius 116, 3 px).
        struct SecondsRing {
            bool on = false;
            float radius = 116, half = 1.5f; // pixels
            uint16_t color = 0;
        };

        void drawRing(LGFX_Sprite &spr, const SecondsRing &ring, float secondAngle, Spans *spans) {
            if (!ring.on) return;
            smooth::arc(spr, CENTRE, CENTRE, ring.radius, ring.half, secondAngle, ring.color);
            // The whole band, whatever the angle: next time it may be longer or shorter.
            if (spans) spans->addRing(MID, MID, ring.radius - ring.half - 1.5f, ring.radius + ring.half + 1.5f);
        }

        // The second zone's hand and name where a clock face doesn't give
        // its own: an orange arrow on a 24-hour scale (the mockup's).
        constexpr uint16_t ZONE_COLOR = rgb(0xd97757);
        Parts defaultZoneHand() {
            Parts p;
            p.part[p.count++] = {bar(0, 92, 1.0f), ZONE_COLOR};
            p.part[p.count++] = {arrow(90, 104, 6), ZONE_COLOR};
            return p;
        }
        const Parts DEFAULT_ZONE_HAND = defaultZoneHand();
        TextSpot defaultZoneLabel() {
            TextSpot s;
            s.on = true;
            s.y = 170;
            s.px = 11;
            s.color = ZONE_COLOR;
            return s;
        }
        const TextSpot DEFAULT_ZONE_LABEL = defaultZoneLabel();

    }

    // What the extras show today: part of the layer's key, so the layer is
    // repainted when the day changes (or the second zone's name).
    struct AnalogClockControl::Extras {
        bool timeSet = false;
        Now now{};
        String zoneLabel; // "" for no second zone
    };

    namespace {
        using Extras = AnalogClockControl::Extras;

        String extrasKey(const Extras &e) {
            String k = e.zoneLabel;
            if (e.timeSet) {
                k += '|';
                k += e.now.t.tm_mday;
                k += e.now.t.tm_wday;
            }
            return k;
        }

        void paintExtras(LGFX_Sprite &spr, const Extras &e, const DateWindow &date, const TextSpot &weekday,
                         const TextSpot &zoneLabel) {
            if (e.timeSet && date.text.on) {
                const int x = date.text.x - date.w / 2, y = date.text.y - date.h / 2;
                if (date.filled) spr.fillRect(x, y, date.w, date.h, date.background);
                if (date.bordered) spr.drawRect(x, y, date.w, date.h, date.border);
                const String day(e.now.t.tm_mday);
                if (date.filled)
                    text::drawPx(spr, day, date.text.x, date.text.y, date.text.px, Align::MiddleCenter, date.text.color, date.background);
                else
                    text::drawOver(spr, day, date.text.x, date.text.y, date.text.px, Align::MiddleCenter, date.text.color);
            }
            if (e.timeSet && weekday.on)
                text::drawOver(spr, WEEKDAYS[e.now.t.tm_wday], weekday.x, weekday.y, weekday.px, Align::MiddleCenter, weekday.color);
            if (e.zoneLabel.length() && zoneLabel.on)
                text::drawOver(spr, e.zoneLabel, zoneLabel.x, zoneLabel.y, zoneLabel.px, Align::MiddleCenter, zoneLabel.color);
        }

        // ---- built-in clock faces ----
        enum class Dial { Classic, Platform, Dots };

        struct ClockFace {
            Dial dial;
            Shape hour, minute, second;
            bool seconds;
            float hub;           // pixels; the centre cap
            bool secondColorHub; // the cap takes the second hand's colour
            float pin;           // pixels; a dark pin on the cap, 0 for none
            bool weekday;        // the weekday under 12, in the dial's colour
        };
        struct Colors {
            uint16_t background, dial, hour, minute, second;
        };

        constexpr uint16_t PIN = rgb(0x111111);

        // From docs/mockups/analog-clock.html. classic keeps the board's own
        // geometry (hands 55/85/95 px) and its colour params.
        const ClockFace CLASSIC = {Dial::Classic, bar(0, 55, 1.0f), bar(0, 85, 0.75f), bar(0, 95, 0.5f), true, 4, false, 0, false};
        const ClockFace PLATFORM = {Dial::Platform,
                                    baton(-14, 58, 9 * 0.55f, 9 * 0.4f),
                                    baton(-14, 88, 7 * 0.55f, 7 * 0.4f),
                                    withDisc(bar(-26, 74, 1.2f), 74, 8.5f), // lollipop
                                    true,
                                    5.5f,
                                    true,
                                    2,
                                    false};
        const ClockFace MINIMAL = {Dial::Dots,
                                   baton(-14, 58, 9 * 0.55f, 9 * 0.4f),
                                   baton(-14, 88, 7 * 0.55f, 7 * 0.4f),
                                   Shape(),
                                   false,
                                   5.5f,
                                   false,
                                   2,
                                   true};

        const ClockFace &clockFace(const char *name) {
            if (strcmp(name, "platform") == 0) return PLATFORM;
            if (strcmp(name, "minimal") == 0) return MINIMAL;
            return CLASSIC;
        }

        Colors colors(const char *name, JsonObjectConst p) {
            if (strcmp(name, "platform") == 0) return {TFT_WHITE, TFT_BLACK, TFT_BLACK, TFT_BLACK, rgb(0xe8423a)};
            if (strcmp(name, "minimal") == 0) return {TFT_BLACK, rgb(0x6e6a64), rgb(0xf3ead8), rgb(0xf3ead8), 0};
            return {p["background"].as<uint16_t>(), p["tickColor"].as<uint16_t>(), p["hourColor"].as<uint16_t>(),
                    p["minuteColor"].as<uint16_t>(), p["secondColor"].as<uint16_t>()};
        }

        // A mark on the dial: a bar from r0 to r1 pixels out, at angle.
        void mark(LGFX_Sprite &spr, float angle, float r0, float r1, float half, uint16_t color) {
            drawShape(spr, bar(r0, r1, half), angle, color);
        }

        void paintDial(LGFX_Sprite &spr, Dial dial, const Colors &c) {
            spr.fillScreen(c.background);
            switch (dial) {
            case Dial::Classic:
                smooth::arc(spr, CENTRE, CENTRE, FACE_RADIUS, 0.5f, 360, c.dial);
                for (int i = 0; i < 12; i++) mark(spr, i * 30.0f, FACE_RADIUS - 10, FACE_RADIUS, 0.5f, c.dial);
                break;
            case Dial::Platform:
                for (int i = 0; i < 60; i++) {
                    if (i % 5) mark(spr, i * 6.0f, 99, 108, 1.25f, c.dial);
                    else mark(spr, i * 6.0f, 78, 108, 3.5f, c.dial);
                }
                break;
            case Dial::Dots:
                for (int i = 0; i < 12; i++) {
                    const float a = i * 30.0f * DEG_TO_RAD;
                    smooth::disc(spr, MID + 100 * sinf(a), MID - 100 * cosf(a), i % 3 ? 3 : 5.5f, c.dial);
                }
                break;
            }
        }

        // ---- clock face files ----
        constexpr size_t MAX_FILE = 256 * 1024; // a ~190 KB PNG, as base64
        constexpr int HASH_DIGITS = 16;

        String filePath(int screen) { return "/orbit-clockface" + String(screen) + ".json"; }
        String newPath(int screen) { return "/orbit-clockface" + String(screen) + ".new"; }

        String hashOf(const uint8_t *data, size_t length) {
            uint8_t digest[32];
            mbedtls_sha256_ret(data, length, digest, 0);
            char hex[HASH_DIGITS + 1];
            for (int i = 0; i < HASH_DIGITS / 2; i++) sprintf(hex + 2 * i, "%02x", digest[i]);
            return String(hex);
        }

        // A whole file in PSRAM; freed by the caller.
        uint8_t *readFile(const String &path, size_t &length) {
            File f = LittleFS.open(path, "r");
            if (!f) return nullptr;
            length = f.size();
            uint8_t *data = length ? (uint8_t *) heap_caps_malloc(length, MALLOC_CAP_SPIRAM) : nullptr;
            if (data && f.read(data, length) != length) {
                free(data);
                data = nullptr;
            }
            f.close();
            return data;
        }

        bool unitNumber(JsonVariantConst v, float limit, float &out) {
            if (!v.is<float>()) return false;
            out = v.as<float>();
            return isfinite(out) && fabsf(out) <= limit;
        }

        bool wholeNumber(JsonVariantConst v, int lo, int hi, int &out) {
            if (!v.is<int>()) return false;
            out = v.as<int>();
            return out >= lo && out <= hi;
        }

        bool parseColor(JsonVariantConst v, uint16_t &out) {
            const char *s = v.as<const char *>();
            if (!s || strlen(s) != 7 || s[0] != '#') return false;
            for (int i = 1; i < 7; i++)
                if (!isxdigit((unsigned char) s[i])) return false;
            out = rgb(strtoul(s + 1, nullptr, 16));
            return true;
        }

        // One part: {"color": "#rrggbb", "points": [[x, y], ...]} and/or
        // "disc": [along, radius].
        bool parsePart(JsonObjectConst in, Part &out, const String &where, String &error) {
            out = Part(); // no points or discs yet: they're counted up below
            if (!parseColor(in["color"], out.color)) {
                error = where + ".color must be \"#rrggbb\"";
                return false;
            }
            Shape &s = out.shape;
            JsonVariantConst points = in["points"], disc = in["disc"];
            if (points.isNull() && disc.isNull()) {
                error = where + " needs points or a disc";
                return false;
            }
            if (!points.isNull()) {
                JsonArrayConst a = points.as<JsonArrayConst>();
                if (a.isNull() || a.size() < 3 || a.size() > smooth::MAX_POINTS) {
                    error = where + ".points must be 3 to " + String(smooth::MAX_POINTS) + " points";
                    return false;
                }
                for (JsonVariantConst v : a) {
                    JsonArrayConst p = v.as<JsonArrayConst>();
                    if (p.size() != 2 || !unitNumber(p[0], 1.5f, s.xy[s.points][0]) || !unitNumber(p[1], 1.5f, s.xy[s.points][1])) {
                        error = where + ".points must be [x, y] pairs between -1.5 and 1.5";
                        return false;
                    }
                    s.points++;
                }
            }
            if (!disc.isNull()) {
                JsonArrayConst d = disc.as<JsonArrayConst>();
                Disc &c = s.disc[0];
                if (d.size() != 2 || !unitNumber(d[0], 1.5f, c.along) || !unitNumber(d[1], 0.5f, c.radius) || c.radius <= 0) {
                    error = where + ".disc must be [along, radius] (radius above 0, at most 0.5)";
                    return false;
                }
                s.discs = 1;
            }
            return true;
        }

        bool parseParts(JsonVariantConst in, Parts &out, const String &where, bool required, String &error) {
            out.count = 0;
            if (in.isNull()) {
                if (required) error = where + " is required";
                return !required;
            }
            JsonArrayConst a = in.as<JsonArrayConst>();
            if (a.isNull() || a.size() < 1 || a.size() > MAX_PARTS) {
                error = where + " must be 1 to " + String(MAX_PARTS) + " parts";
                return false;
            }
            for (JsonVariantConst v : a) {
                JsonObjectConst p = v.as<JsonObjectConst>();
                if (p.isNull() || !parsePart(p, out.part[out.count], where + "[" + out.count + "]", error)) {
                    if (error.isEmpty()) error = where + " must hold objects";
                    return false;
                }
                out.count++;
            }
            return true;
        }

        // {"x": 120, "y": 72, "size": 14, "color": "#rrggbb"}; absent is off.
        bool parseTextSpot(JsonVariantConst in, TextSpot &out, const String &where, String &error) {
            out = TextSpot();
            if (in.isNull()) return true;
            int x, y, px;
            if (!wholeNumber(in["x"], 0, 239, x) || !wholeNumber(in["y"], 0, 239, y)) {
                error = where + ".x and .y must be whole pixels, 0 to 239";
                return false;
            }
            if (!wholeNumber(in["size"], 6, 48, px)) {
                error = where + ".size must be 6 to 48 pixels";
                return false;
            }
            if (!parseColor(in["color"], out.color)) {
                error = where + ".color must be \"#rrggbb\"";
                return false;
            }
            out.on = true;
            out.x = x, out.y = y, out.px = px;
            return true;
        }

        // {"radius": 116, "width": 3, "color": "#rrggbb"}, pixels; absent is none.
        bool parseRing(JsonVariantConst in, SecondsRing &out, String &error) {
            out = SecondsRing();
            if (in.isNull()) return true;
            float radius, width;
            if (!unitNumber(in["radius"], 120, radius) || radius < 10) {
                error = "secondsRing.radius must be 10 to 120 pixels";
                return false;
            }
            if (!unitNumber(in["width"], 12, width) || width < 1) {
                error = "secondsRing.width must be 1 to 12 pixels";
                return false;
            }
            if (!parseColor(in["color"], out.color)) {
                error = "secondsRing.color must be \"#rrggbb\"";
                return false;
            }
            out.on = true;
            out.radius = radius, out.half = width / 2;
            return true;
        }

        // A text spot plus the window's size and optional fill and border.
        bool parseDateWindow(JsonVariantConst in, DateWindow &out, String &error) {
            out = DateWindow();
            if (!parseTextSpot(in, out.text, "date", error)) return false;
            if (!out.text.on) return true;
            int w = out.w, h = out.h;
            if ((!in["width"].isNull() && !wholeNumber(in["width"], 8, 120, w)) ||
                (!in["height"].isNull() && !wholeNumber(in["height"], 8, 120, h))) {
                error = "date.width and .height must be 8 to 120 pixels";
                return false;
            }
            out.w = w, out.h = h;
            if (!in["background"].isNull() && !(out.filled = parseColor(in["background"], out.background))) {
                error = "date.background must be \"#rrggbb\"";
                return false;
            }
            if (!in["border"].isNull() && !(out.bordered = parseColor(in["border"], out.border))) {
                error = "date.border must be \"#rrggbb\"";
                return false;
            }
            return true;
        }

        // The dial: a base64 240x240 PNG, decoded.
        std::shared_ptr<makapix::Artwork> decodeDial(JsonVariantConst in, String &error) {
            const char *b64 = in.as<const char *>();
            if (!b64) {
                error = "dial must be a base64 PNG";
                return nullptr;
            }
            const size_t inLength = strlen(b64);
            size_t length = 0;
            uint8_t *png = (uint8_t *) heap_caps_malloc(inLength / 4 * 3 + 3, MALLOC_CAP_SPIRAM);
            if (!png) {
                error = "not enough memory to decode the dial";
                return nullptr;
            }
            std::shared_ptr<makapix::Artwork> art;
            static const uint8_t SIGNATURE[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
            if (mbedtls_base64_decode(png, inLength / 4 * 3 + 3, &length, (const uint8_t *) b64, inLength) != 0) {
                error = "dial isn't valid base64";
            } else if (length < 8 || memcmp(png, SIGNATURE, 8) != 0) {
                error = "dial must be a PNG";
            } else {
                art = makapix::decode(png, length, "png", display::WIDTH * display::HEIGHT * 2, error);
                if (art && (art->width != display::WIDTH || art->height != display::HEIGHT || art->frames.size() != 1)) {
                    error = "dial must be one 240x240 picture, not " + String(art->width) + "x" + String(art->height);
                    art.reset();
                }
                if (!art && error.isEmpty()) error = "dial couldn't be decoded";
            }
            free(png);
            return art;
        }
    }

    // ---- uploaded clock faces ----

    struct AnalogClockControl::FileFace {
        bool checked = false; // hash is known
        String hash;          // of the file held; "" for none
        bool loaded = false;  // the parts below and the layer are from it
        bool failed = false;  // it couldn't be loaded; not tried again until a new upload
        Parts hour, minute, second, zone, cap;
        DateWindow date;
        TextSpot weekday, zoneLabel;
        SecondsRing ring;
    };

    namespace {
        // Everything in a clock face file but the dial.
        bool parseFace(JsonDocument &doc, AnalogClockControl::FileFace &f, String &error) {
            if ((doc["version"] | 0) != AnalogClockControl::FILE_VERSION) {
                error = "version must be " + String(AnalogClockControl::FILE_VERSION);
                return false;
            }
            JsonVariantConst id = doc["id"];
            if (!id.isNull() && (!id.is<const char *>() || strlen(id.as<const char *>()) > 64)) {
                error = "id must be a string of at most 64 characters";
                return false;
            }
            JsonVariantConst hands = doc["hands"];
            if (!parseParts(hands["hour"], f.hour, "hands.hour", true, error) ||
                !parseParts(hands["minute"], f.minute, "hands.minute", true, error) ||
                !parseParts(hands["second"], f.second, "hands.second", false, error) ||
                !parseParts(hands["zone"], f.zone, "hands.zone", false, error) || !parseParts(doc["cap"], f.cap, "cap", false, error) ||
                !parseDateWindow(doc["date"], f.date, error) || !parseTextSpot(doc["weekday"], f.weekday, "weekday", error) ||
                !parseTextSpot(doc["zoneLabel"], f.zoneLabel, "zoneLabel", error) || !parseRing(doc["secondsRing"], f.ring, error))
                return false;
            // Only a clock face with a zone hand takes a second time zone: one
            // without shows neither hand nor name, whatever the screen's zone2.
            if (!f.zone.count) f.zoneLabel.on = false;
            else if (!f.zoneLabel.on) f.zoneLabel = DEFAULT_ZONE_LABEL;
            return true;
        }
    }

    AnalogClockControl::FileFace &AnalogClockControl::file(int screen) {
        if (!files[screen]) files[screen] = new FileFace();
        return *files[screen];
    }

    const String &AnalogClockControl::heldHash(int screen) {
        FileFace &f = file(screen);
        if (!f.checked && mountFiles()) {
            size_t length;
            uint8_t *data = readFile(filePath(screen), length);
            f.hash = data ? hashOf(data, length) : "";
            free(data);
            f.checked = true;
        }
        return f.hash;
    }

    int AnalogClockControl::check(int screen, const String &body, String &hash, String &id, String &error) {
        if (body.length() > MAX_FILE) {
            error = "clock face file too big (max " + String(MAX_FILE / 1024) + " KB)";
            return 413;
        }
        if (!mountFiles()) {
            error = "no file system to store clock faces in";
            return 503;
        }
        {
            JsonDocument doc;
            if (deserializeJson(doc, body)) {
                error = "malformed JSON body";
                return 400;
            }
            std::unique_ptr<FileFace> parsed(new FileFace()); // several KB: not on the loop's stack
            if (!parseFace(doc, *parsed, error) || !decodeDial(doc["dial"], error)) return 400;
            id = doc["id"] | "";
        }
        hash = hashOf((const uint8_t *) body.c_str(), body.length());
        // Writing flash can take a second or two, so the watchdog waits.
        health::pause(true);
        File f = LittleFS.open(newPath(screen), "w");
        const bool written = f && f.write((const uint8_t *) body.c_str(), body.length()) == body.length();
        if (f) f.close();
        health::pause(false);
        if (!written) {
            LittleFS.remove(newPath(screen));
            error = "couldn't store the clock face (file system full?)";
            return 507;
        }
        return 0;
    }

    int AnalogClockControl::install(int screen, const String &hash, String &error) {
        LittleFS.remove(filePath(screen));
        if (!LittleFS.rename(newPath(screen), filePath(screen))) {
            error = "couldn't store the clock face";
            file(screen).checked = false; // whatever is there now
            return 500;
        }
        FileFace &f = file(screen);
        f.checked = true;
        f.hash = hash;
        f.loaded = f.failed = false;
        layerKey[screen] = ""; // repaint, whichever clock face it shows
        return 0;
    }

    // Shows the uploaded clock face if the board holds the one asked for and
    // it loads; otherwise the built-in one is shown. The layer is the dial
    // picture with the extras painted on, so it's read again from the file
    // when they change (once a day).
    bool AnalogClockControl::useFile(int screen, const char *wanted, const Extras &extras) {
        FileFace &f = file(screen);
        if (heldHash(screen) != wanted || f.failed || !ensureLayer(screen)) return false;
        const String key = "file|" + f.hash + "|" + extrasKey(extras);
        if (f.loaded && layerKey[screen] == key) return true;

        String error;
        size_t length;
        uint8_t *data = readFile(filePath(screen), length);
        if (data) {
            JsonDocument doc;
            // Parsed apart from f, so a file that fails halfway leaves no
            // half-read hands behind.
            std::unique_ptr<FileFace> parsed(new FileFace());
            if (deserializeJson(doc, (const char *) data, length)) {
                error = "unreadable JSON";
            } else if (parseFace(doc, *parsed, error)) {
                auto art = decodeDial(doc["dial"], error);
                if (art) {
                    f.hour = parsed->hour;
                    f.minute = parsed->minute;
                    f.second = parsed->second;
                    f.zone = parsed->zone;
                    f.cap = parsed->cap;
                    f.date = parsed->date;
                    f.weekday = parsed->weekday;
                    f.zoneLabel = parsed->zoneLabel;
                    f.ring = parsed->ring;
                    // The decoder's pixels are in native byte order; sprites
                    // hold them swapped.
                    const uint16_t *src = art->frames[0];
                    uint16_t *dst = (uint16_t *) layer[screen].getBuffer();
                    for (int i = 0; i < display::WIDTH * display::HEIGHT; i++) dst[i] = __builtin_bswap16(src[i]);
                    paintExtras(layer[screen], extras, f.date, f.weekday, f.zoneLabel);
                    layerGen[screen]++;
                }
            }
            free(data);
        } else {
            error = "couldn't read it";
        }
        if (error.length()) {
            f.failed = true;
            health::event("OrbIt: screen %d's clock face file not shown (%s)", screen, error.c_str());
            return false;
        }
        f.loaded = true;
        layerKey[screen] = key;
        return true;
    }

    // ---- the control ----

    namespace {
        int optionalZone(JsonObjectConst in, JsonObject out, const char *key, String &error) {
            if (int status = optionalString(in, out, key, "", 64, error)) return status;
            const String tz = out[key].as<String>();
            if (tz.length() && !timekeeper::validTimezone(tz)) {
                error = String("params.") + key + " '" + tz + "' isn't a known timezone";
                return 400;
            }
            return 0;
        }
    }

    int AnalogClockControl::parse(JsonObjectConst in, JsonObject out, String &error) const {
        int status;
        if ((status = optionalChoice(in, out, "clockFace", CLOCK_FACES, error))) return status;
        if ((status = optionalColor(in, out, "background", TFT_BLACK, error))) return status;
        if ((status = optionalColor(in, out, "tickColor", TFT_WHITE, error))) return status;
        if ((status = optionalColor(in, out, "hourColor", TFT_WHITE, error))) return status;
        if ((status = optionalColor(in, out, "minuteColor", TFT_WHITE, error))) return status;
        if ((status = optionalColor(in, out, "secondColor", TFT_RED, error))) return status;
        if ((status = optionalBool(in, out, "seconds", true, error))) return status;
        if ((status = optionalChoice(in, out, "motion", MOTIONS, error))) return status;
        if ((status = optionalBool(in, out, "secondsRing", false, error))) return status;
        if ((status = optionalZone(in, out, "timeZone", error))) return status;
        if ((status = optionalZone(in, out, "zone2", error))) return status;
        if ((status = optionalString(in, out, "clockFaceId", "", 64, error))) return status;
        if ((status = optionalString(in, out, "clockFaceHash", "", HASH_DIGITS, error))) return status;
        for (const char *c = out["clockFaceHash"].as<const char *>(); *c; c++) {
            if (!isxdigit((unsigned char) *c) || isupper((unsigned char) *c)) {
                error = "params.clockFaceHash must be lower-case hex";
                return 400;
            }
        }
        return 0;
    }

    void AnalogClockControl::report(int screen, JsonObject params) {
        const String &held = heldHash(screen);
        if (held.length()) params["clockFaceHeld"] = held;
        else params["clockFaceHeld"] = nullptr;
    }

    // About 10 fps while a second hand sweeps; once a second otherwise.
    uint32_t AnalogClockControl::refreshMs(int screen, JsonObjectConst p) const {
        if (!(p["seconds"] | true) || strcmp(p["motion"] | "tick", "sweep") != 0) return TICK_MS;
        const char *wanted = p["clockFaceHash"] | "";
        const FileFace *f = files[screen];
        if (*wanted && f && f->loaded && f->hash == wanted) return f->second.count || f->ring.on ? SWEEP_MS : TICK_MS;
        return clockFace(p["clockFace"] | "classic").seconds || (p["secondsRing"] | false) ? SWEEP_MS : TICK_MS;
    }

    void AnalogClockControl::draw(LGFX_Sprite &spr, JsonObjectConst p, const DrawContext &ctx) {
        const bool seconds = p["seconds"] | true, sweep = strcmp(p["motion"] | "tick", "sweep") == 0;
        const String zone2 = p["zone2"] | "";
        Extras extras;
        extras.timeSet = clockNow(p["timeZone"] | "", extras.now);
        struct tm zoneTime;
        const bool zoneShown = zone2.length() && extras.timeSet && timekeeper::localIn(zone2, extras.now.utc, zoneTime);
        if (zoneShown) extras.zoneLabel = zoneName(zone2);
        const Angles a = anglesAt(extras.now, sweep);

        const char *wanted = p["clockFaceHash"] | "";
        if (*wanted && useFile(ctx.screen, wanted, extras)) {
            Spans *spans = copyLayer(spr, ctx);
            if (!extras.timeSet) return; // just the dial until the clock is set
            const FileFace &f = file(ctx.screen);
            if (seconds) drawRing(spr, f.ring, a.second, spans);
            if (zoneShown) drawParts(spr, f.zone, zoneAngle(zoneTime), spans);
            drawParts(spr, f.hour, a.hour, spans);
            drawParts(spr, f.minute, a.minute, spans);
            if (seconds) drawParts(spr, f.second, a.second, spans);
            drawParts(spr, f.cap, 0, spans);
            return;
        }

        const char *name = p["clockFace"] | "classic";
        const ClockFace &face = clockFace(name);
        const Colors c = colors(name, p);
        TextSpot weekday;
        if (face.weekday) {
            weekday.on = true;
            weekday.y = 72;
            weekday.px = 14;
            weekday.color = c.dial;
        }

        // What the layer holds: the clock face, its dial's colours and the extras.
        String key = name;
        key += '|';
        key += c.background;
        key += '|';
        key += c.dial;
        key += '|';
        key += extrasKey(extras);
        auto paint = [&](LGFX_Sprite &s) {
            paintDial(s, face.dial, c);
            paintExtras(s, extras, DateWindow(), weekday, DEFAULT_ZONE_LABEL);
        };
        Spans *spans = nullptr;
        if (ensureLayer(ctx.screen)) {
            if (key != layerKey[ctx.screen]) {
                paint(layer[ctx.screen]);
                layerKey[ctx.screen] = key;
                layerGen[ctx.screen]++;
            }
            spans = copyLayer(spr, ctx);
        } else {
            paint(spr); // no PSRAM for a layer: slower, same picture
            if (frames[ctx.screen] && ctx.picture >= 0) frames[ctx.screen]->valid[ctx.picture] = false;
        }

        if (!extras.timeSet) return; // just the dial until the clock is set
        SecondsRing ring;
        ring.on = seconds && (p["secondsRing"] | false);
        ring.color = face.seconds ? c.second : c.hour; // minimal has no second hand to match
        drawRing(spr, ring, a.second, spans);
        if (zoneShown) drawParts(spr, DEFAULT_ZONE_HAND, zoneAngle(zoneTime), spans);
        drawShape(spr, face.hour, a.hour, c.hour, spans);
        drawShape(spr, face.minute, a.minute, c.minute, spans);
        if (face.seconds && seconds) drawShape(spr, face.second, a.second, c.second, spans);
        const uint16_t hub = face.secondColorHub && face.seconds && seconds ? c.second : c.hour;
        smooth::disc(spr, MID, MID, face.hub, hub);
        if (face.pin > 0) smooth::disc(spr, MID, MID, face.pin, PIN);
        if (spans) spans->addDisc(MID, MID, face.hub);
    }

    // Puts the layer into the frame: only the rows' spans the hands covered
    // last time, if this picture still holds that frame and the layer hasn't
    // changed since; otherwise all of it (a 115 KB copy, ~5 ms). Returns
    // where to record what this frame's hands cover, or nullptr.
    AnalogClockControl::Spans *AnalogClockControl::copyLayer(LGFX_Sprite &spr, const DrawContext &ctx) {
        const int s = ctx.screen, p = ctx.picture;
        uint16_t *dst = (uint16_t *) spr.getBuffer();
        const uint16_t *src = (const uint16_t *) layer[s].getBuffer();
        if (!frames[s]) frames[s] = new Frames();
        if (p < 0 || p > 1 || !frames[s]) {
            memcpy(dst, src, display::WIDTH * display::HEIGHT * 2);
            return nullptr;
        }
        Frames &f = *frames[s];
        if (ctx.pictureClean && f.valid[p] && f.layerGen[p] == layerGen[s]) {
            const Spans &old = f.spans[p];
            for (int y = 0; y < display::HEIGHT; y++) {
                for (int k = 0; k < old.count[y]; k++) {
                    const int at = y * display::WIDTH + old.x0[y][k];
                    memcpy(dst + at, src + at, (old.x1[y][k] - old.x0[y][k] + 1) * 2);
                }
            }
        } else {
            memcpy(dst, src, display::WIDTH * display::HEIGHT * 2);
        }
        f.spans[p].clear();
        f.valid[p] = true;
        f.layerGen[p] = layerGen[s];
        return &f.spans[p];
    }

    bool AnalogClockControl::ensureLayer(int screen) {
        LGFX_Sprite &l = layer[screen];
        if (l.getBuffer()) return true;
        l.setColorDepth(16);
        l.setPsram(true);
        layerKey[screen] = ""; // nothing painted yet
        return l.createSprite(display::WIDTH, display::HEIGHT) != nullptr;
    }

}
