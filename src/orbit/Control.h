#pragma once

// One kind of screen content in the orbit-api ("time", "gauge", ...).
//
// A control checks the params a client sends, and draws the whole picture of
// a screen from them. It never talks to the display: the display core works
// out what changed and sends only that. So a control can redraw everything
// every time without flicker or extra cost on the wire.

#include <ArduinoJson.h>
#include <LovyanGFX.hpp>

namespace orbit {

    struct DrawContext {
        int screen;     // 0-4
        uint32_t nowMs; // millis() for this frame
    };

    class Control {
    public:
        virtual ~Control() = default;

        // The orbit-api name.
        virtual const char *name() const = 0;

        // Checks params and writes the complete, normalised set (defaults
        // filled in) to out, which is what GET reports. Returns 0, or an HTTP
        // status (400) with error set.
        virtual int parse(JsonObjectConst params, JsonObject out, String &error) const = 0;

        // Removes live readings (a gauge's value...) from params before the
        // layout is saved, so value-only updates never write to flash. Only
        // settings are saved. Controls without live data leave this alone.
        virtual void stripLive(JsonObject params) const {}

        // Draws the whole picture.
        virtual void draw(LGFX_Sprite &spr, JsonObjectConst params, const DrawContext &ctx) = 0;

        // How often the picture can change by itself, in ms (a clock: 1000).
        // 0 = only when its params change.
        virtual uint32_t refreshMs(JsonObjectConst params) const { return 0; }
    };

    // ---- Shared param helpers ----
    // Each reads one optional param from in, writes it (or the fallback, if it
    // was left out) to out, and returns 0, or 400 with error set.

    int optionalBool(JsonObjectConst in, JsonObject out, const char *key, bool fallback, String &error);
    int optionalNumber(JsonObjectConst in, JsonObject out, const char *key, float fallback, String &error);
    int optionalString(JsonObjectConst in, JsonObject out, const char *key, const char *fallback, size_t maxLength, String &error);
    // One of a fixed set of names; choices ends with nullptr.
    int optionalChoice(JsonObjectConst in, JsonObject out, const char *key, const char *const *choices, String &error);
    // A colour: an RGB565 integer (0-65535) or a name ("cyan"); reported as
    // the integer, so what GET returns can be sent straight back. null and ""
    // mean the default.
    int optionalColor(JsonObjectConst in, JsonObject out, const char *key, uint16_t fallback, String &error);

    // The named colours orbit-api accepts (same values as info-orbs').
    bool colorFromName(String name, uint16_t &out);

}
