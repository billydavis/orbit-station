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

    // An API write to one screen, for controls whose writes depend on what
    // the screen shows now (countdown's actions, custom's overlays).
    struct WriteContext {
        int screen;
        bool sameControl;        // the screen already shows this control
        JsonObjectConst current; // its params then (null otherwise)
    };

    // What the layout should do after a write was applied.
    enum class AfterWrite {
        Keep,    // the usual: the screen shows the new config
        Restore, // put back what the screen showed before (countdown's stop)
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
        virtual uint32_t refreshMs(int screen, JsonObjectConst params) const { return 0; }

        // ---- Optional, for controls with state of their own ----
        // All are called with the layout locked.

        // An API write to a known screen. Default: parse().
        virtual int parseWrite(const WriteContext &ctx, JsonObjectConst params, JsonObject out, String &error) {
            return parse(params, out, error);
        }
        // After an API write was applied; request is the params as sent.
        virtual AfterWrite applied(int screen, JsonObjectConst request, JsonObjectConst stored) { return AfterWrite::Keep; }
        // Adds live state (not params) to what GET reports.
        virtual void report(int screen, JsonObject params) {}
        // false: never saved. The screen is saved as what it showed before.
        virtual bool saved() const { return true; }
        // true: params are too big for the layout (drawings) and are saved
        // in a file of their own, written only when they change.
        virtual bool savedInFile() const { return false; }
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

    // Like optionalColor, but writes the key only if the client gave it (for
    // params GET reports only when set).
    int colorIfSet(JsonObjectConst in, JsonObject out, const char *key, String &error);

    // The named colours orbit-api accepts (same values as info-orbs').
    bool colorFromName(String name, uint16_t &out);

    // An RGB565 colour scaled towards black; level 255 = unchanged.
    inline uint16_t dim(uint16_t c, uint8_t level) {
        const uint16_t r = ((c >> 11) & 0x1F) * level / 255, g = ((c >> 5) & 0x3F) * level / 255, b = (c & 0x1F) * level / 255;
        return (r << 11) | (g << 5) | b;
    }

}
