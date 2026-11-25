#pragma once

// Local time from the internet (SNTP), kept by the ESP32's own clock.
// Timezones are IANA names ("America/Chicago"), turned into the POSIX rules
// the C library needs using a table built into the firmware, so no API key or
// timezone service is needed; daylight saving is handled by the rule.

#include <Arduino.h>
#include <time.h>

namespace timekeeper {

    // Starts syncing; call once WiFi is up (calling again is harmless).
    void startSync();
    // True once the clock has been set from the internet.
    bool isSet();
    // Local time now; false if the clock isn't set yet.
    bool now(struct tm &out);

    bool validTimezone(const String &tz);
    void setTimezone(const String &tz);

    // Local time now in another timezone (IANA name or POSIX rule), worked
    // out from its rule without changing the board's own (TZ is global, and
    // other tasks read it). ms is the millisecond within the second. False
    // if the clock isn't set or the zone is unknown.
    bool nowIn(const String &tz, struct tm &out, uint32_t *ms = nullptr);
    // The same for a given time; used by nowIn and the tests.
    bool localIn(const String &tz, time_t t, struct tm &out);

}
