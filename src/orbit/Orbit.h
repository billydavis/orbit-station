#pragma once

// OrbIt: each of the five screens shows one control, chosen and configured
// through orbit-api v1 (/orbit/api/v1/...), the same API info-orbs' OrbIt
// widget serves, so existing clients keep working. See info-orbs'
// docs/orbit-api.md for the full spec.
//
// The layout (which control each screen shows, and its settings) is saved in
// NVS and restored at start-up. Live readings are never saved, and the
// layout is only written when its saved form actually changes.

#include <ArduinoJson.h>
#include <WebServer.h>

namespace orbit {

    constexpr const char *API_BASE = "/orbit/api/v1";

    // Restores the saved layout and starts drawing the screens (core 0).
    void begin();
    void registerRoutes(WebServer &server);

    // Controls built into this firmware, for the capabilities endpoints.
    void listControls(JsonArray out);

    // Average time a frame of this screen took to draw since the last call, in
    // microseconds (for the serial frame report).
    uint32_t takeDrawUs(int screen);

}
