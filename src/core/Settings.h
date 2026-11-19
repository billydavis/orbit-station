#pragma once

// Board-wide settings, changed over the API (GET/POST /api/v1/settings) and
// saved in NVS. They're settings, not live data, so they're only written when
// one actually changes.

#include <Arduino.h>
#include <ArduinoJson.h>

namespace settings {

    struct Values {
        String timezone;      // IANA name ("America/Chicago") or a POSIX rule
        bool metric = false;  // date format now; weather units later
        String weatherApiKey; // Visual Crossing
        String weatherLocation; // default for weather screens without their own
        String tickerApiKey;  // twelvedata
    };

    void begin();
    Values get();

    // Applies the fields present in the request. Returns 0, or an HTTP status
    // with error set (nothing is changed then). saved is false if a changed
    // value couldn't be written to flash.
    int update(JsonObjectConst request, String &error, bool &saved);

    // For GET: keys are reported masked, never in full.
    void toJson(JsonObject out);

}
