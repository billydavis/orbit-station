#include "AnalogClockControl.h"
#include "core/Health.h"
#include "core/TimeKeeper.h"
#include "makapix/Artwork.h"
#include "orbit/Orbit.h"
#include "ui/Smooth.h"
#include <LittleFS.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>

namespace orbit {

    const char *const AnalogClockControl::CLOCK_FACES[] = {"classic", "platform", "minimal", nullptr};

    namespace {
        constexpr int CENTRE = 120;
        constexpr float FACE_RADIUS = 110; // inside the round bezel's visible area; 1 in clock face units
        // The face's centre in continuous pixel coordinates: the middle of
        // pixel (120, 120), as smooth::arc draws round.
        constexpr float MID = CENTRE + 0.5f;

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

        // Draws a shape turned angle degrees clockwise from 12 o'clock.
        void drawShape(LGFX_Sprite &spr, const Shape &h, float angle, uint16_t color) {
            const float a = angle * DEG_TO_RAD, s = sinf(a), c = cosf(a);
            if (h.points) {
                float xy[smooth::MAX_POINTS * 2];
                for (int i = 0; i < h.points; i++) {
                    const float across = h.xy[i][0] * FACE_RADIUS, along = h.xy[i][1] * FACE_RADIUS;
                    xy[2 * i] = MID + along * s + across * c;
                    xy[2 * i + 1] = MID - along * c + across * s;
                }
                smooth::polygon(spr, xy, h.points, color);
            }
            for (int i = 0; i < h.discs; i++) {
                const float along = h.disc[i].along * FACE_RADIUS;
                smooth::disc(spr, MID + along * s, MID - along * c, h.disc[i].radius * FACE_RADIUS, color);
            }
        }

        // Where the hands point, in degrees clockwise from 12 o'clock.
        struct Angles {
            float hour, minute, second;
        };

        bool angles(Angles &a) {
            struct tm t;
            if (!timekeeper::now(t)) return false;
            a.second = t.tm_sec * 6.0f;
            a.minute = t.tm_min * 6.0f + t.tm_sec * 0.1f;
            a.hour = (t.tm_hour % 12) * 30.0f + t.tm_min * 0.5f;
            return true;
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
        };
        struct Colors {
            uint16_t background, dial, hour, minute, second;
        };

        constexpr uint16_t PIN = rgb(0x111111);

        // From docs/mockups/analog-clock.html. classic keeps the board's own
        // geometry (hands 55/85/95 px) and its colour params.
        const ClockFace CLASSIC = {Dial::Classic, bar(0, 55, 1.0f), bar(0, 85, 0.75f), bar(0, 95, 0.5f), true, 4, false, 0};
        const ClockFace PLATFORM = {Dial::Platform,
                                    baton(-14, 58, 9 * 0.55f, 9 * 0.4f),
                                    baton(-14, 88, 7 * 0.55f, 7 * 0.4f),
                                    withDisc(bar(-26, 74, 1.2f), 74, 8.5f), // lollipop
                                    true,
                                    5.5f,
                                    true,
                                    2};
        const ClockFace MINIMAL = {
            Dial::Dots, baton(-14, 58, 9 * 0.55f, 9 * 0.4f), baton(-14, 88, 7 * 0.55f, 7 * 0.4f), Shape(), false, 5.5f, false, 2};

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
        constexpr int MAX_PARTS = 4;            // shapes in one hand or the cap
        constexpr int HASH_DIGITS = 16;

        // A shape of an uploaded clock face with its own colour.
        struct Part {
            Shape shape;
            uint16_t color;
        };
        struct Parts {
            uint8_t count = 0;
            Part part[MAX_PARTS];
        };

        void drawParts(LGFX_Sprite &spr, const Parts &p, float angle) {
            for (int i = 0; i < p.count; i++) drawShape(spr, p.part[i].shape, angle, p.part[i].color);
        }

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
        bool loaded = false;  // the hands below and the layer are from it
        bool failed = false;  // it couldn't be loaded; not tried again until a new upload
        Parts hour, minute, second, cap;
    };

    namespace {
        // Everything in a clock face file but the dial: the hands and the cap.
        bool parseHands(JsonDocument &doc, AnalogClockControl::FileFace &f, String &error) {
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
            return parseParts(hands["hour"], f.hour, "hands.hour", true, error) &&
                   parseParts(hands["minute"], f.minute, "hands.minute", true, error) &&
                   parseParts(hands["second"], f.second, "hands.second", false, error) &&
                   parseParts(doc["cap"], f.cap, "cap", false, error);
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
            std::unique_ptr<FileFace> parsed(new FileFace()); // ~2.6 KB: not on the loop's stack
            if (!parseHands(doc, *parsed, error) || !decodeDial(doc["dial"], error)) return 400;
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
    // it loads; otherwise the built-in one is shown.
    bool AnalogClockControl::useFile(int screen, const char *wanted) {
        FileFace &f = file(screen);
        if (heldHash(screen) != wanted || f.failed || !ensureLayer(screen)) return false;
        const String key = String("file|") + f.hash;
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
            } else if (parseHands(doc, *parsed, error)) {
                auto art = decodeDial(doc["dial"], error);
                if (art) {
                    f.hour = parsed->hour;
                    f.minute = parsed->minute;
                    f.second = parsed->second;
                    f.cap = parsed->cap;
                    // The decoder's pixels are in native byte order; sprites
                    // hold them swapped.
                    const uint16_t *src = art->frames[0];
                    uint16_t *dst = (uint16_t *) layer[screen].getBuffer();
                    for (int i = 0; i < display::WIDTH * display::HEIGHT; i++) dst[i] = __builtin_bswap16(src[i]);
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

    int AnalogClockControl::parse(JsonObjectConst in, JsonObject out, String &error) const {
        int status;
        if ((status = optionalChoice(in, out, "clockFace", CLOCK_FACES, error))) return status;
        if ((status = optionalColor(in, out, "background", TFT_BLACK, error))) return status;
        if ((status = optionalColor(in, out, "tickColor", TFT_WHITE, error))) return status;
        if ((status = optionalColor(in, out, "hourColor", TFT_WHITE, error))) return status;
        if ((status = optionalColor(in, out, "minuteColor", TFT_WHITE, error))) return status;
        if ((status = optionalColor(in, out, "secondColor", TFT_RED, error))) return status;
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

    void AnalogClockControl::draw(LGFX_Sprite &spr, JsonObjectConst p, const DrawContext &ctx) {
        const char *wanted = p["clockFaceHash"] | "";
        if (*wanted && useFile(ctx.screen, wanted)) {
            memcpy(spr.getBuffer(), layer[ctx.screen].getBuffer(), display::WIDTH * display::HEIGHT * 2);
            const FileFace &f = file(ctx.screen);
            Angles a;
            if (!angles(a)) return; // just the dial until the clock is set
            drawParts(spr, f.hour, a.hour);
            drawParts(spr, f.minute, a.minute);
            drawParts(spr, f.second, a.second);
            drawParts(spr, f.cap, 0);
            return;
        }

        const char *name = p["clockFace"] | "classic";
        const ClockFace &face = clockFace(name);
        const Colors c = colors(name, p);

        // What the layer holds: the clock face and its dial's colours.
        String key = name;
        key += '|';
        key += c.background;
        key += '|';
        key += c.dial;
        if (ensureLayer(ctx.screen)) {
            if (key != layerKey[ctx.screen]) {
                paintDial(layer[ctx.screen], face.dial, c);
                layerKey[ctx.screen] = key;
            }
            memcpy(spr.getBuffer(), layer[ctx.screen].getBuffer(), display::WIDTH * display::HEIGHT * 2);
        } else {
            paintDial(spr, face.dial, c); // no PSRAM for a layer: slower, same picture
        }

        Angles a;
        if (!angles(a)) return; // just the dial until the clock is set
        drawShape(spr, face.hour, a.hour, c.hour);
        drawShape(spr, face.minute, a.minute, c.minute);
        if (face.seconds) drawShape(spr, face.second, a.second, c.second);
        smooth::disc(spr, MID, MID, face.hub, face.secondColorHub && face.seconds ? c.second : c.hour);
        if (face.pin > 0) smooth::disc(spr, MID, MID, face.pin, PIN);
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
