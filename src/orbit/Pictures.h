#pragma once

// The picture store: finished 240x240 pictures (JPEG, PNG, GIF or WebP) that
// clients upload for the picture control, one LittleFS file each, named by
// its hash and shared by every screen that shows it.
//
// Files no screen uses stay as a cache and are removed, oldest first, only
// when an upload needs their room. A file a screen uses is never removed.
//
// An upload arrives in chunks while the WebServer reads the body (on the
// Arduino loop): begin, write for each chunk, then end or abort. One at a
// time, since the WebServer serves one request at a time.

#include <ArduinoJson.h>
#include <vector>

namespace pictures {

    constexpr size_t MAX_BYTES = 1024 * 1024;
    constexpr int SIZE = 240;
    // Left free for drawings and clock face files, which pictures never
    // crowd out.
    constexpr size_t RESERVE = 128 * 1024;

    // 16 lowercase hex digits: the start of the file's SHA-256.
    bool validHash(const String &hash);
    bool has(const String &hash);
    String path(const String &hash);

    // inUse: the hashes screens show, which eviction must keep. Pauses the
    // loop watchdog until end() or abort().
    void begin(const String &hash, size_t length, const std::vector<String> &inUse);
    void write(const uint8_t *data, size_t length);
    void abort();
    // Whether an upload has begun and not yet ended.
    bool active();
    // Checks and stores the file. Returns the HTTP status; on 200 out
    // describes it, otherwise error says what's wrong.
    int end(JsonObject out, String &error);

    // The pictures held and the room left.
    void list(JsonObject out, const std::vector<String> &inUse);
    // Returns the HTTP status.
    int remove(const String &hash, const std::vector<String> &inUse, String &error);

    void capabilities(JsonObject out);

    // Goes up whenever a picture is stored or removed, so screens know when
    // to look at the store again.
    uint32_t generation();

}
