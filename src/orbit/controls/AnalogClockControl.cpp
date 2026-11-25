#include "AnalogClockControl.h"
#include "core/TimeKeeper.h"
#include "ui/Smooth.h"

namespace orbit {

    namespace {
        constexpr int CENTRE = 120;
        constexpr float FACE_RADIUS = 110; // inside the round bezel's visible area
        // The face's centre in continuous pixel coordinates: the middle of
        // pixel (120, 120), as smooth::arc draws round.
        constexpr float MID = CENTRE + 0.5f;

        const char *const CLOCK_FACES[] = {"classic", "platform", "minimal", nullptr};

        // #rrggbb as RGB565.
        constexpr uint16_t rgb(uint32_t hex) {
            return ((hex >> 8) & 0xF800) | ((hex >> 5) & 0x07E0) | ((hex >> 3) & 0x001F);
        }

        // ---- hands ----
        // A hand is an outline and up to two discs, in face-radius units (0 is
        // the centre, 1 the face's radius), pointing at 12 o'clock: x across
        // (right is positive), y along it (outwards is positive). The same
        // form clock face files will use, so built-in and uploaded hands are
        // drawn the same way.
        struct Disc {
            float along, radius;
        };
        struct Hand {
            uint8_t points = 0;
            float xy[smooth::MAX_POINTS][2];
            uint8_t discs = 0;
            Disc disc[2];
        };

        constexpr float unit(float px) { return px / FACE_RADIUS; }

        // A straight bar from r0 to r1 pixels along, half wide either side.
        Hand bar(float r0, float r1, float half) {
            Hand h;
            const float pts[4][2] = {{-half, r0}, {-half, r1}, {half, r1}, {half, r0}};
            for (const auto &p : pts) {
                h.xy[h.points][0] = unit(p[0]);
                h.xy[h.points][1] = unit(p[1]);
                h.points++;
            }
            return h;
        }

        // The mockup's baton: tapered from half0 to half1, round at both ends.
        Hand baton(float r0, float r1, float half0, float half1) {
            Hand h = bar(r0, r1, half0);
            h.xy[1][0] = unit(-half1);
            h.xy[2][0] = unit(half1);
            h.disc[h.discs++] = {unit(r0), unit(half0)};
            h.disc[h.discs++] = {unit(r1), unit(half1)};
            return h;
        }

        Hand withDisc(Hand h, float along, float radius) {
            h.disc[h.discs++] = {unit(along), unit(radius)};
            return h;
        }

        // Draws a hand turned angle degrees clockwise from 12 o'clock.
        void drawHand(LGFX_Sprite &spr, const Hand &h, float angle, uint16_t color) {
            const float a = angle * DEG_TO_RAD, s = sinf(a), c = cosf(a);
            float xy[smooth::MAX_POINTS * 2];
            for (int i = 0; i < h.points; i++) {
                const float across = h.xy[i][0] * FACE_RADIUS, along = h.xy[i][1] * FACE_RADIUS;
                xy[2 * i] = MID + along * s + across * c;
                xy[2 * i + 1] = MID - along * c + across * s;
            }
            smooth::polygon(spr, xy, h.points, color);
            for (int i = 0; i < h.discs; i++) {
                const float along = h.disc[i].along * FACE_RADIUS;
                smooth::disc(spr, MID + along * s, MID - along * c, h.disc[i].radius * FACE_RADIUS, color);
            }
        }

        // ---- clock faces ----
        enum class Dial { Classic, Platform, Dots };

        struct ClockFace {
            Dial dial;
            Hand hour, minute, second;
            bool seconds;
            float hub;          // pixels; the centre cap
            bool secondColorHub; // the cap takes the second hand's colour
            float pin;          // pixels; a dark pin on the cap, 0 for none
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
            Dial::Dots, baton(-14, 58, 9 * 0.55f, 9 * 0.4f), baton(-14, 88, 7 * 0.55f, 7 * 0.4f), Hand(), false, 5.5f, false, 2};

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
            drawHand(spr, bar(r0, r1, half), angle, color);
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
    }

    int AnalogClockControl::parse(JsonObjectConst in, JsonObject out, String &error) const {
        int status;
        if ((status = optionalChoice(in, out, "clockFace", CLOCK_FACES, error))) return status;
        if ((status = optionalColor(in, out, "background", TFT_BLACK, error))) return status;
        if ((status = optionalColor(in, out, "tickColor", TFT_WHITE, error))) return status;
        if ((status = optionalColor(in, out, "hourColor", TFT_WHITE, error))) return status;
        if ((status = optionalColor(in, out, "minuteColor", TFT_WHITE, error))) return status;
        return optionalColor(in, out, "secondColor", TFT_RED, error);
    }

    void AnalogClockControl::draw(LGFX_Sprite &spr, JsonObjectConst p, const DrawContext &ctx) {
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

        struct tm t;
        if (!timekeeper::now(t)) return; // just the dial until the clock is set

        const float second = t.tm_sec * 6.0f;
        const float minute = t.tm_min * 6.0f + t.tm_sec * 0.1f;
        const float hour = (t.tm_hour % 12) * 30.0f + t.tm_min * 0.5f;
        drawHand(spr, face.hour, hour, c.hour);
        drawHand(spr, face.minute, minute, c.minute);
        if (face.seconds) drawHand(spr, face.second, second, c.second);
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
