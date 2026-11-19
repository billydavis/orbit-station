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

}
