#include "PictureControl.h"
#include "core/Health.h"
#include "makapix/Artwork.h"
#include "orbit/Pictures.h"
#include "orbit/Transition.h"
#include "ui/Text.h"
#include <LittleFS.h>
#include <algorithm>

namespace orbit {

    namespace {
        constexpr int SIZE = 240;
        // Decoded size allowed for a GIF or WebP's frames (only the first is
        // shown so far).
        constexpr size_t DECODE_BUDGET = 1024 * 1024;
        // How often a slideshow checks whether it's time to change.
        constexpr uint32_t TICK_MS = 250;
        const char *const ORDERS[] = {"list", "shuffle", nullptr};

        // ---- The loader: decodes the next picture off the render task ----

        struct Job {
            PictureControl::Screen *screen;
            int index;
        };
        QueueHandle_t jobs = nullptr;

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

        // Decodes a picture file into sprite. Returns "" or what went wrong.
        String decodeInto(LGFX_Sprite &sprite, const uint8_t *data, size_t length, const char *&format) {
            if (length >= 3 && data[0] == 0xFF && data[1] == 0xD8) {
                format = "jpeg";
                sprite.fillScreen(TFT_BLACK);
                return sprite.drawJpg(data, length, 0, 0) ? "" : "unreadable JPEG";
            }
            format = data[0] == 0x89 ? "png" : data[0] == 'G' ? "gif" : "webp";
            String error;
            auto art = makapix::decode(data, length, "", DECODE_BUDGET, error);
            if (!art) return error;
            if (art->width != SIZE || art->height != SIZE || art->frames.empty()) return "not 240x240";
            const uint16_t *src = art->frames[0];
            uint16_t *dst = (uint16_t *) sprite.getBuffer();
            for (int i = 0; i < SIZE * SIZE; i++) dst[i] = __builtin_bswap16(src[i]); // sprites hold byte-swapped RGB565
            return "";
        }

        void load(PictureControl::Screen &s, int index) {
            const uint32_t start = millis();
            PictureControl::Buffer &b = s.ahead;
            String error;
            const char *format = "?";
            if (!b.sprite) {
                b.sprite = new LGFX_Sprite();
                b.sprite->setPsram(true);
                b.sprite->setColorDepth(16);
            }
            if (!b.sprite->getBuffer() && !b.sprite->createSprite(SIZE, SIZE)) error = "out of memory";
            size_t length = 0;
            uint8_t *data = error.length() ? nullptr : readFile(pictures::path(b.hash), length);
            if (!error.length() && !data) error = "can't read the file";
            if (data) {
                error = decodeInto(*b.sprite, data, length, format);
                free(data);
            }
            s.problem = error;
            if (error.length()) {
                Serial.printf("Picture: screen %d can't show %s: %s\n", index, b.hash.c_str(), error.c_str());
            } else {
                Serial.printf("Picture: screen %d loaded %s (%s, %lu ms, loader stack %u bytes spare)\n", index, b.hash.c_str(), format,
                              (unsigned long) (millis() - start), (unsigned) uxTaskGetStackHighWaterMark(nullptr));
            }
            s.state = error.length() ? PictureControl::Failed : PictureControl::Ready;
        }

        void loaderTask(void *) {
            Job job;
            for (;;) {
                if (xQueueReceive(jobs, &job, portMAX_DELAY) == pdTRUE) load(*job.screen, job.index);
            }
        }

        void startLoader() {
            if (jobs) return;
            jobs = xQueueCreate(8, sizeof(Job));
            // Internal RAM. Decoding a 240x240 picture used at most ~3.4 KB
            // (lossless WebP the most); load() logs what's spare.
            xTaskCreatePinnedToCore(loaderTask, "pictures", 12288, nullptr, 1, nullptr, 1);
        }

        // Asks the loader for hash in the screen's ahead buffer.
        void request(PictureControl::Screen &s, int index, const String &hash) {
            startLoader();
            s.ahead.hash = hash;
            s.state = PictureControl::Loading;
            Job job = {&s, index};
            if (xQueueSend(jobs, &job, 0) != pdTRUE) s.state = PictureControl::Idle; // full: asked again next tick
        }

        bool contains(const std::vector<String> &v, const String &x) { return std::find(v.begin(), v.end(), x) != v.end(); }
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

    // The pictures the board holds, from the list, without repeats or ones
    // that failed; shuffled if asked.
    void PictureControl::rebuild(Screen &s, JsonObjectConst p, const String &key) {
        if (key != s.key || s.storeGeneration != pictures::generation()) s.failed.clear(); // something new to try
        s.key = key;
        s.storeGeneration = pictures::generation();
        s.upcoming = "";
        s.sequence.clear();
        for (JsonVariantConst v : p["pictures"].as<JsonArrayConst>()) {
            const String hash = v.as<String>();
            if (!contains(s.sequence, hash) && !contains(s.failed, hash) && pictures::has(hash)) s.sequence.push_back(hash);
        }
        if (strcmp(p["order"] | "list", "shuffle") == 0) {
            for (int i = s.sequence.size() - 1; i > 0; i--) std::swap(s.sequence[i], s.sequence[esp_random() % (i + 1)]);
        }
    }

    // The picture to show after the current one (the first if there's
    // none). A shuffle starts a new round, in a new order, after the last.
    String PictureControl::nextAfter(Screen &s, bool shuffle) {
        const int n = s.sequence.size();
        if (n == 0) return "";
        const auto at = std::find(s.sequence.begin(), s.sequence.end(), s.shown.hash);
        if (at == s.sequence.end()) return s.sequence[0];
        const int i = at - s.sequence.begin();
        if (i + 1 < n) return s.sequence[i + 1];
        if (shuffle && n > 1) {
            // A new round; never the picture showing now first.
            for (int k = n - 1; k > 0; k--) std::swap(s.sequence[k], s.sequence[esp_random() % (k + 1)]);
            if (s.sequence[0] == s.shown.hash) std::swap(s.sequence[0], s.sequence[1 + esp_random() % (n - 1)]);
        }
        return s.sequence[0];
    }

    uint32_t PictureControl::refreshMs(int screen, JsonObjectConst) const {
        const Screen &s = screens[screen];
        // Waiting for the loader, or a slideshow to run.
        return s.state == Loading || s.sequence.size() > 1 || (s.shown.hash.isEmpty() && !s.sequence.empty()) ? TICK_MS : 0;
    }

    void PictureControl::draw(LGFX_Sprite &spr, JsonObjectConst p, const DrawContext &ctx) {
        Screen &s = screens[ctx.screen];
        const bool shuffle = strcmp(p["order"] | "list", "shuffle") == 0;
        String key = p["order"] | "list";
        for (JsonVariantConst v : p["pictures"].as<JsonArrayConst>()) key += v.as<const char *>();
        if (key != s.key || s.storeGeneration != pictures::generation()) rebuild(s, p, key);

        if (s.state != Loading) {
            // The loader is done with ahead: a picture that failed drops out.
            if (s.state == Failed) {
                s.failed.push_back(s.ahead.hash);
                s.ahead.hash = "";
                s.state = Idle;
                rebuild(s, p, key);
            }
            // Decided once per change: a shuffle reorders when a round ends,
            // which would move the current picture and change the answer.
            if (s.upcoming.isEmpty() || !contains(s.sequence, s.upcoming)) s.upcoming = nextAfter(s, shuffle);
            const String next = s.upcoming;
            const bool current = contains(s.sequence, s.shown.hash);
            const bool due = !current || ctx.nowMs - s.shownAt >= p["seconds"].as<uint32_t>() * 1000UL;
            if (next.length() && next != s.shown.hash) {
                if (s.state == Ready && s.ahead.hash == next) {
                    if (due) {
                        std::swap(s.shown, s.ahead); // the old picture stays ahead, in case it's next again
                        s.shownAt = ctx.nowMs;
                        const String after = s.upcoming = nextAfter(s, shuffle);
                        if (after.length() && after != s.shown.hash && after != s.ahead.hash) request(s, ctx.screen, after);
                    }
                } else {
                    request(s, ctx.screen, next);
                }
            } else if (!current && next.isEmpty()) {
                s.shown.hash = ""; // nothing left to show
            }
        }

        const String &showing = s.shown.hash;
        if (showing.length() && contains(s.sequence, showing)) {
            if (ctx.pictureClean && s.drawn[ctx.picture] == showing) return; // already there
            memcpy(spr.getBuffer(), s.shown.sprite->getBuffer(), SIZE * SIZE * 2);
            s.drawn[ctx.picture] = showing;
            return;
        }
        s.drawn[ctx.picture] = "";
        spr.fillScreen(TFT_BLACK);
        JsonArrayConst list = p["pictures"];
        String message;
        if (list.size() == 0) message = "No pictures";
        else if (s.sequence.empty()) message = s.failed.empty() ? "Waiting for pictures" : "Can't show these pictures";
        if (message.length()) text::draw(spr, message, 120, 120, 13, Align::MiddleCenter, 0x7BEF, TFT_BLACK);
        // Otherwise the first picture is loading: black for a moment.
    }

    void PictureControl::report(int screen, JsonObject params) {
        JsonArray out = params["picturesMissing"].to<JsonArray>();
        for (JsonVariantConst v : params["pictures"].as<JsonArrayConst>()) {
            if (!pictures::has(v.as<String>())) out.add(v.as<String>());
        }
    }

    uint32_t PictureControl::contentId(int screen) const {
        const String &hash = screens[screen].shown.hash;
        if (hash.isEmpty()) return 0;
        return strtoul(hash.substring(0, 8).c_str(), nullptr, 16) | 1;
    }

}
