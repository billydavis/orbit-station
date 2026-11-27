#include "Pictures.h"
#include "Orbit.h"
#include "core/Health.h"
#include <LittleFS.h>
#include <algorithm>
#include <atomic>
#include <mbedtls/sha256.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace pictures {

    namespace {
        const char *DIR = "/pictures";
        const char *PARTIAL = "/pictures/.upload"; // the file being received
        std::atomic<uint32_t> changes{0};

        // LittleFS rounds each file up to 4 KB blocks and needs a few of
        // its own while writing.
        constexpr size_t SLACK = 16 * 1024;

        struct Upload {
            bool active = false;
            String hash;
            size_t length = 0, received = 0;
            bool held = false; // already stored: the body is read and dropped
            int status = 0;    // the first error's HTTP status, 0 if none
            String error;
            int fd = -1; // the partial file, written with plain POSIX calls
            size_t written = 0;
            mbedtls_sha256_context sha;
        } up;

        String vfs(const String &path) { return "/littlefs" + path; }

        // LittleFS.exists() logs an error for every file that isn't there.
        bool exists(const String &path) {
            struct stat st;
            return stat(vfs(path).c_str(), &st) == 0;
        }

        void closePartial() {
            if (up.fd >= 0) close(up.fd);
            up.fd = -1;
        }

        void removePartial() {
            if (exists(PARTIAL)) LittleFS.remove(PARTIAL);
        }

        void fail(int status, const String &error) {
            if (up.status) return; // the first error is the one to report
            up.status = status;
            up.error = error;
            closePartial();
            removePartial();
        }

        // Writes a chunk of the body to the partial file, as it comes: a
        // flash write stalls both cores (cache off), so bigger batches only
        // make fewer, longer hitches on the screens.
        bool writeChunk(const uint8_t *data, size_t length) {
            size_t done = 0;
            while (done < length) {
                const ssize_t n = ::write(up.fd, data + done, length - done);
                if (n <= 0) {
                    const int e = errno;
                    fail(500, "writing the picture to flash failed after " + String(up.written + done) + " bytes (errno " + String(e) + ")");
                    return false;
                }
                done += n;
            }
            up.written += done;
            return true;
        }

        bool used(const String &hash, const std::vector<String> &inUse) {
            return std::find(inUse.begin(), inUse.end(), hash) != inUse.end();
        }

        size_t freeBytes() {
            const size_t total = LittleFS.totalBytes(), usedBytes = LittleFS.usedBytes();
            return total > usedBytes ? total - usedBytes : 0;
        }

        struct Held {
            String hash;
            size_t bytes;
            time_t written;
        };

        std::vector<Held> held() {
            std::vector<Held> out;
            File dir = LittleFS.open(DIR);
            if (!dir || !dir.isDirectory()) return out;
            for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
                const String name = f.name(); // just the name in this core
                if (validHash(name)) out.push_back({name, f.size(), f.getLastWrite()});
            }
            return out;
        }

        // Removes pictures no screen uses, oldest first, until need bytes
        // are free. False if that isn't enough.
        bool makeRoom(size_t need, const std::vector<String> &inUse) {
            if (freeBytes() >= need) return true;
            std::vector<Held> spare = held();
            spare.erase(std::remove_if(spare.begin(), spare.end(), [&](const Held &h) { return used(h.hash, inUse); }), spare.end());
            std::sort(spare.begin(), spare.end(), [](const Held &a, const Held &b) { return a.written < b.written; });
            for (const Held &h : spare) {
                if (freeBytes() >= need) break;
                LittleFS.remove(path(h.hash));
                changes++;
                Serial.printf("Pictures: removed %s (%u KB, unused) to make room\n", h.hash.c_str(), (unsigned) (h.bytes / 1024));
            }
            return freeBytes() >= need;
        }

        uint16_t be16(const uint8_t *p) { return p[0] << 8 | p[1]; }
        uint16_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }
        uint32_t le24(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16; }

        // The format and size from the file's header. Returns the format's
        // name, or nullptr with error set.
        const char *identify(File &f, int &w, int &h, String &error) {
            uint8_t b[30] = {};
            f.seek(0);
            const size_t n = f.read(b, sizeof b);
            if (n >= 24 && !memcmp(b, "\x89PNG\r\n\x1a\n", 8) && !memcmp(b + 12, "IHDR", 4)) {
                w = b[16] << 24 | b[17] << 16 | b[18] << 8 | b[19];
                h = b[20] << 24 | b[21] << 16 | b[22] << 8 | b[23];
                return "png";
            }
            if (n >= 10 && (!memcmp(b, "GIF87a", 6) || !memcmp(b, "GIF89a", 6))) {
                w = le16(b + 6), h = le16(b + 8);
                return "gif";
            }
            if (n >= 30 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WEBP", 4)) {
                if (!memcmp(b + 12, "VP8 ", 4)) {
                    w = le16(b + 26) & 0x3FFF, h = le16(b + 28) & 0x3FFF;
                } else if (!memcmp(b + 12, "VP8L", 4) && b[20] == 0x2F) {
                    const uint32_t bits = b[21] | b[22] << 8 | b[23] << 16 | (uint32_t) b[24] << 24;
                    w = (bits & 0x3FFF) + 1, h = (bits >> 14 & 0x3FFF) + 1;
                } else if (!memcmp(b + 12, "VP8X", 4)) {
                    w = le24(b + 24) + 1, h = le24(b + 27) + 1;
                } else {
                    error = "a WebP file of a kind this board can't read";
                    return nullptr;
                }
                return "webp";
            }
            if (n >= 4 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) {
                // Walk the markers to the frame header.
                size_t at = 2;
                for (int guard = 0; guard < 64; guard++) {
                    uint8_t m[9];
                    f.seek(at);
                    if (f.read(m, sizeof m) != sizeof m || m[0] != 0xFF) break;
                    const uint8_t type = m[1];
                    if (type == 0xC0 || type == 0xC1) {
                        h = be16(m + 5), w = be16(m + 7);
                        return "jpeg";
                    }
                    if (type == 0xC2 || type == 0xC6 || type == 0xCA || type == 0xCE) {
                        error = "progressive JPEGs can't be drawn; save it as baseline";
                        return nullptr;
                    }
                    if (type >= 0xC3 && type <= 0xCF && type != 0xC4 && type != 0xC8 && type != 0xCC) {
                        error = "a kind of JPEG this board can't draw (lossless or arithmetic); save it as baseline";
                        return nullptr;
                    }
                    if (type == 0xD9 || type == 0xDA) break; // end, or image data with no frame header
                    at += 2 + be16(m + 2);
                }
                error = "a JPEG with no frame header";
                return nullptr;
            }
            error = "not a JPEG, PNG, GIF or WebP file";
            return nullptr;
        }
    }

    bool validHash(const String &hash) {
        if (hash.length() != 16) return false;
        for (char c : hash) {
            if (!isdigit(c) && !(c >= 'a' && c <= 'f')) return false;
        }
        return true;
    }

    String path(const String &hash) { return String(DIR) + "/" + hash; }

    bool has(const String &hash) { return validHash(hash) && orbit::mountFiles() && exists(path(hash)); }

    void begin(const String &hash, size_t length, const std::vector<String> &inUse) {
        if (up.active) abort();
        up = Upload();
        up.active = true;
        up.hash = hash;
        up.length = length;
        mbedtls_sha256_init(&up.sha);
        // Receiving and writing a big file outlasts the loop watchdog.
        health::pause(true);
        if (!validHash(hash)) return fail(400, "the picture's name must be 16 lowercase hex digits (the start of its SHA-256)");
        if (length == 0) return fail(400, "missing body: send the picture file itself, with its Content-Length");
        if (length > MAX_BYTES) return fail(413, "the picture is " + String(length) + " bytes; the most is " + String(MAX_BYTES) + " (1 MB)");
        if (!orbit::mountFiles()) return fail(500, "the board's file system isn't available");
        removePartial(); // a partial file from an upload cut short
        if (exists(path(hash))) {
            up.held = true;
            return;
        }
        if (!makeRoom(length + SLACK + RESERVE, inUse)) {
            return fail(507, "not enough room: " + String(freeBytes() / 1024) + " KB free, " +
                                 String((length + SLACK + RESERVE) / 1024) + " KB needed (" + String(RESERVE / 1024) +
                                 " KB is kept for drawings and clock faces)");
        }
        LittleFS.mkdir(DIR);
        up.fd = open(vfs(PARTIAL).c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (up.fd < 0) return fail(500, "couldn't create the picture's file (errno " + String(errno) + ")");
        mbedtls_sha256_starts_ret(&up.sha, 0);
    }

    void write(const uint8_t *data, size_t length) {
        up.received += length;
        if (!up.active || up.status || up.held) return;
        mbedtls_sha256_update_ret(&up.sha, data, length);
        writeChunk(data, length);
    }

    void abort() {
        if (!up.active) return;
        closePartial();
        removePartial();
        mbedtls_sha256_free(&up.sha);
        if (!up.status) Serial.printf("Pictures: upload of %s cut short after %u of %u bytes\n", up.hash.c_str(), (unsigned) up.received, (unsigned) up.length);
        up = Upload();
        health::pause(false);
    }

    bool active() { return up.active; }

    int end(JsonObject out, String &error) {
        const bool hashing = !up.held && !up.status;
        uint8_t digest[32];
        if (hashing) mbedtls_sha256_finish_ret(&up.sha, digest);
        mbedtls_sha256_free(&up.sha);
        if (!up.status && !up.held && up.received != up.length) fail(400, "the body ended early");
        if (hashing && !up.status) {
            char hex[17];
            for (int i = 0; i < 8; i++) sprintf(hex + 2 * i, "%02x", digest[i]);
            if (up.hash != hex) fail(400, "the file's hash is " + String(hex) + ", not " + up.hash);
        }
        closePartial();
        int w = 0, h = 0;
        const char *format = nullptr;
        if (!up.status) {
            File f = LittleFS.open(up.held ? path(up.hash) : String(PARTIAL), "r");
            String why = "couldn't read the picture back";
            if (f) format = identify(f, w, h, why);
            f.close();
            if (!format) fail(400, why);
            else if (w != SIZE || h != SIZE) fail(400, "the picture is " + String(w) + "x" + String(h) + "; it must be 240x240");
        }
        if (!up.status && !up.held) {
            if (exists(path(up.hash))) LittleFS.remove(path(up.hash));
            if (!LittleFS.rename(PARTIAL, path(up.hash))) fail(500, "couldn't store the picture's file");
            else {
                changes++;
                Serial.printf("Pictures: stored %s (%s, %u KB)\n", up.hash.c_str(), format, (unsigned) (up.length / 1024));
            }
        }
        const int status = up.status ? up.status : 200;
        if (status == 200) {
            out["hash"] = up.hash;
            out["bytes"] = up.length;
            out["format"] = format;
            out["stored"] = !up.held; // false: the board already had it
        } else {
            error = up.error;
        }
        up = Upload();
        health::pause(false);
        return status;
    }

    void list(JsonObject out, const std::vector<String> &inUse) {
        JsonArray arr = out["pictures"].to<JsonArray>();
        size_t total = 0;
        if (orbit::mountFiles()) {
            for (const Held &h : held()) {
                JsonObject p = arr.add<JsonObject>();
                p["hash"] = h.hash;
                p["bytes"] = h.bytes;
                p["inUse"] = used(h.hash, inUse);
                total += h.bytes;
            }
        }
        out["bytes"] = total;
        // What the next upload could have, counting the room unused pictures
        // would give up.
        size_t spare = 0;
        for (JsonObject p : arr) {
            if (!p["inUse"]) spare += p["bytes"].as<size_t>();
        }
        const size_t room = freeBytes() + spare;
        out["roomBytes"] = room > RESERVE + SLACK ? room - RESERVE - SLACK : 0;
        out["maxBytes"] = MAX_BYTES;
    }

    int remove(const String &hash, const std::vector<String> &inUse, String &error) {
        if (!validHash(hash)) {
            error = "the picture's name must be 16 lowercase hex digits";
            return 400;
        }
        if (!has(hash)) {
            error = "the board doesn't have picture " + hash;
            return 404;
        }
        if (used(hash, inUse)) {
            error = "a screen shows picture " + hash + "; take it off the screen first";
            return 409;
        }
        if (!LittleFS.remove(path(hash))) {
            error = "couldn't remove picture " + hash;
            return 500;
        }
        changes++;
        return 200;
    }

    uint32_t generation() { return changes; }

    void capabilities(JsonObject out) {
        out["maxBytes"] = MAX_BYTES;
        out["width"] = SIZE;
        out["height"] = SIZE;
        JsonArray formats = out["formats"].to<JsonArray>();
        for (const char *f : {"jpeg", "png", "gif", "webp"}) formats.add(f);
    }

}
