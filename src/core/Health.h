#pragma once

// Keeps the board healthy unattended.
//
// Watchdog: each long-running task calls beat() as it works. If one stops
// beating for longer than its deadline, the board records which and why,
// and restarts. The Arduino loop itself is on the chip's hardware watchdog.
//
// Memory: if the largest free block of internal RAM stays too small for an
// HTTPS connection for minutes, the board restarts rather than limping on
// with every fetch failing.
//
// Events: the last few dozen notable events (restarts and why, failures,
// memory warnings) are kept in memory for GET /api/v1/log, and printed on
// serial. The reason for the last restart survives the restart.

#include <Arduino.h>
#include <ArduinoJson.h>

namespace health {

    enum class Task : uint8_t { Display, Render, Feeds, Makapix, Count };

    void begin();
    // Called from the Arduino loop: checks heartbeats and memory.
    void loop();

    void beat(Task task);
    // Suspends the watchdog (an update over WiFi blocks the loop for a while).
    void pause(bool paused);

    // Records why, then restarts.
    [[noreturn]] void restart(const char *why);

    // Adds a line to the event log (and serial). printf-style.
    void event(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

    void toJson(JsonObject out); // for /api/v1/system
    void logJson(JsonArray out); // for /api/v1/log, oldest first

}
