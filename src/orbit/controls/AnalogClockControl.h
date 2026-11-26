#pragma once

#include "display/Display.h"
#include "orbit/Control.h"

namespace orbit {

    // An analog clock: a dial and hour, minute and second hands (hour and
    // minute move smoothly), drawn anti-aliased. Hands are outlines filled
    // with smooth edges.
    //
    // params (all optional, all settings):
    //   clockFace ("classic")    a built-in clock face, also shown when an
    //                            uploaded one is missing or doesn't match:
    //                            classic: circle, 12 ticks, thin hands (the
    //                            info-orbs geometry), coloured by the params
    //                            below.
    //                            platform: bold bars, black batons and a red
    //                            lollipop second hand on white.
    //                            minimal: twelve dots, cream batons, no
    //                            second hand, on black.
    //   background (black), tickColor (white), hourColor (white),
    //   minuteColor (white), secondColor (red): classic's colours.
    //   seconds (true)           show the second hand (if the clock face has one)
    //   motion ("tick")          tick: the second hand jumps once a second.
    //                            sweep: it moves smoothly (about 10 fps; costs
    //                            drawing time on core 0).
    //   secondsRing (false)      built-in clock faces: a ring round the rim
    //                            filling with the seconds (a clock face file
    //                            has its own). Hidden with the second hand.
    //   timeZone ("")            the hands show this timezone (IANA name);
    //                            "" for the board's own.
    //   zone2 ("")               a second timezone: a hand going round once a
    //                            day, and the city's name on the dial.
    //   clockFaceId ("")         the uploaded clock face's name, for clients.
    //   clockFaceHash ("")       show the uploaded clock face with this hash
    //                            (see below); "" for the built-in one.
    //
    // GET also reports clockFaceHeld: the hash of the clock face file the
    // board holds for this screen, or null. A client compares it with the
    // hash it would upload and sends the file again if they differ.
    //
    // Clock face files (docs/clock-faces.md): PUT /screens/{n}/clock-face
    // with a JSON body holding the dial as a base64 240x240 PNG and the hands
    // as outlines. The board checks it and stores the body byte for byte in
    // LittleFS (/orbit-clockface{n}.json), written only on upload. Its hash
    // is the first 16 hex digits of the SHA-256 of the body.
    class AnalogClockControl : public Control {
    public:
        static const char *const CLOCK_FACES[]; // built-in names, nullptr-terminated
        static constexpr int FILE_VERSION = 1;

        const char *name() const override { return "analogClock"; }
        int parse(JsonObjectConst params, JsonObject out, String &error) const override;
        void draw(LGFX_Sprite &spr, JsonObjectConst params, const DrawContext &ctx) override;
        uint32_t refreshMs(int screen, JsonObjectConst params) const override;
        void report(int screen, JsonObject params) override;

        // Upload, in two halves. check() validates a clock face file and
        // writes it beside the real one; it's slow (decoding, writing flash),
        // so call it without the layout lock. Then install() puts it in
        // place, with the layout locked. Both return an HTTP status (0 when
        // fine) and set error. hash is the file's, id its "id" ("" if none).
        int check(int screen, const String &body, String &hash, String &id, String &error);
        int install(int screen, const String &hash, String &error);

        // Per-screen state of uploaded clock faces, and what the date,
        // weekday and second zone show now; both live in the .cpp.
        struct FileFace;
        struct Extras;
        // Per picture, what the hands covered (see copyLayer).
        struct Spans;
        struct Frames;

    private:
        // The parts that stay still (background and dial, or an uploaded
        // dial picture), kept in PSRAM (115 KB a screen, made the first
        // time a screen shows a clock) and repainted only when they change.
        // Each frame copies it and draws the hands on top.
        LGFX_Sprite layer[display::NUM_SCREENS];
        String layerKey[display::NUM_SCREENS];

        FileFace *files[display::NUM_SCREENS] = {};
        Frames *frames[display::NUM_SCREENS] = {};
        uint32_t layerGen[display::NUM_SCREENS] = {}; // counts repaints of the layer

        bool ensureLayer(int screen);
        FileFace &file(int screen);
        const String &heldHash(int screen);
        bool useFile(int screen, const char *wanted, const Extras &extras);
        Spans *copyLayer(LGFX_Sprite &spr, const DrawContext &ctx);
    };

}
