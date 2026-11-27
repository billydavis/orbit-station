#include "PictureControl.h"
#include "makapix/Artwork.h"
#include "orbit/Pictures.h"
#include "orbit/Transition.h"
#include "ui/Text.h"
#include <LittleFS.h>

namespace orbit {

    namespace {
        constexpr int SIZE = 240;
        // Decoded size allowed for a GIF or WebP's frames (only the first is
        // shown so far).
        constexpr size_t DECODE_BUDGET = 1024 * 1024;
        const char *const ORDERS[] = {"list", "shuffle", nullptr};

        // A whole file in PSRAM; freed by the caller.
        uint8_t *readFile(const String &path, size_t &length) {
            File f = LittleFS.open(path, "r");
            if (!f) return nullptr;
            length = f.size();
            uint8_t *data = length ? (uint8_t *) heap_caps_malloc(length, MALLOC_CAP_SPIRAM) : nullptr;
            if (data && f.read(data, length) != length) {
                free(data);
                data = nullptr;
            }
            return data;
        }
    }

    int PictureControl::parse(JsonObjectConst in, JsonObject out, String &error) const {
        JsonVariantConst list = in["pictures"];
        JsonArray pictures = out["pictures"].to<JsonArray>();
        if (!list.isNull()) {
            if (!list.is<JsonArrayConst>()) {
                error = "params.pictures must be a list of picture hashes";
                return 400;
            }
            if (list.size() > MAX_PICTURES) {
                error = "params.pictures can have up to " + String(MAX_PICTURES) + " pictures";
                return 400;
            }
            for (JsonVariantConst v : list.as<JsonArrayConst>()) {
                const String hash = v.as<String>();
                if (!v.is<const char *>() || !pictures::validHash(hash)) {
                    error = "params.pictures: '" + hash + "' isn't a picture hash (16 lowercase hex digits)";
                    return 400;
                }
                pictures.add(hash);
            }
        }
        JsonVariantConst seconds = in["seconds"];
        if (!seconds.isNull() && !seconds.is<long>()) {
            error = "params.seconds must be a whole number";
            return 400;
        }
        out["seconds"] = constrain(seconds.isNull() ? 30L : seconds.as<long>(), 5L, 86400L);
        int status;
        if ((status = optionalChoice(in, out, "order", ORDERS, error))) {
            error = "unknown picture order '" + in["order"].as<String>() + "' (list or shuffle)";
            return status;
        }
        if ((status = optionalChoice(in, out, "transition", transition::CHOICES, error))) return status;
        return 0;
    }

    // Reads and decodes a picture into the screen's cache. False (with the
    // cache's problem set) if it can't.
    bool PictureControl::load(int screen, const String &hash) {
        Cache &c = cache[screen];
        c.hash = hash;
        c.problem = "";
        const uint32_t start = millis();
        size_t length = 0;
        uint8_t *data = readFile(pictures::path(hash), length);
        if (!data) {
            c.problem = "Can't read the picture";
            return false;
        }
        if (!c.sprite) {
            c.sprite = new LGFX_Sprite();
            c.sprite->setPsram(true);
            c.sprite->setColorDepth(16);
        }
        if (!c.sprite->getBuffer() && !c.sprite->createSprite(SIZE, SIZE)) {
            free(data);
            c.problem = "Out of memory";
            return false;
        }
        const char *format;
        String error;
        if (length >= 3 && data[0] == 0xFF && data[1] == 0xD8) {
            format = "jpeg";
            c.sprite->fillScreen(TFT_BLACK);
            if (!c.sprite->drawJpg(data, length, 0, 0)) error = "unreadable JPEG";
        } else {
            format = "png/gif/webp";
            auto art = makapix::decode(data, length, "", DECODE_BUDGET, error);
            if (art && (art->width != SIZE || art->height != SIZE || art->frames.empty())) {
                error = "not 240x240";
                art.reset();
            }
            if (art) {
                const uint16_t *src = art->frames[0];
                uint16_t *dst = (uint16_t *) c.sprite->getBuffer();
                for (int i = 0; i < SIZE * SIZE; i++) dst[i] = __builtin_bswap16(src[i]); // sprites hold byte-swapped RGB565
            }
        }
        free(data);
        if (error.length()) {
            c.problem = "Can't show this picture";
            Serial.printf("Picture: screen %d can't show %s: %s\n", screen, hash.c_str(), error.c_str());
            return false;
        }
        Serial.printf("Picture: screen %d shows %s (%s, %lu ms to load)\n", screen, hash.c_str(), format, (unsigned long) (millis() - start));
        return true;
    }

    void PictureControl::draw(LGFX_Sprite &spr, JsonObjectConst p, const DrawContext &ctx) {
        Cache &c = cache[ctx.screen];
        // The first picture the board holds.
        String wanted;
        JsonArrayConst list = p["pictures"];
        for (JsonVariantConst v : list) {
            const String hash = v.as<String>();
            if (pictures::has(hash)) {
                wanted = hash;
                break;
            }
        }
        if (wanted.length() && wanted != c.hash) load(ctx.screen, wanted);

        if (wanted.length() && c.hash == wanted && c.problem.isEmpty()) {
            memcpy(spr.getBuffer(), c.sprite->getBuffer(), SIZE * SIZE * 2);
            return;
        }
        spr.fillScreen(TFT_BLACK);
        const String message = wanted.length() ? c.problem : list.size() ? "Waiting for pictures" : "No pictures";
        text::draw(spr, message, 120, 120, 13, Align::MiddleCenter, 0x7BEF, TFT_BLACK);
    }

    void PictureControl::report(int screen, JsonObject params) {
        JsonArray out = params["picturesMissing"].to<JsonArray>();
        for (JsonVariantConst v : params["pictures"].as<JsonArrayConst>()) {
            if (!pictures::has(v.as<String>())) out.add(v.as<String>());
        }
    }

    uint32_t PictureControl::contentId(int screen) const {
        const Cache &c = cache[screen];
        if (c.hash.isEmpty() || c.problem.length()) return 0;
        return strtoul(c.hash.substring(0, 8).c_str(), nullptr, 16) | 1;
    }

}
