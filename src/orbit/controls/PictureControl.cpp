#include "PictureControl.h"
#include "makapix/Artwork.h"
#include "orbit/Pictures.h"
#include "orbit/Transition.h"
#include "ui/Text.h"
#include <LittleFS.h>
#include <algorithm>
#include "src/webp/demux.h"

namespace orbit {

    namespace {
        constexpr int SIZE = 240;
        constexpr size_t PICTURE_BYTES = SIZE * SIZE * 2;
        // Decoded size allowed for a PNG (one 240x240 frame).
        constexpr size_t DECODE_BUDGET = 256 * 1024;
        // An animated GIF stays in PSRAM while it plays; with less than this
        // free after reading it, it shows its first frame instead.
        constexpr size_t KEEP_FREE = 1024 * 1024;
        // How often a slideshow checks whether it's time to change.
        constexpr uint32_t TICK_MS = 250;
        // The quickest an animation plays (a frame takes 20-30 ms to decode).
        constexpr uint32_t MIN_FRAME_MS = 30;
        const char *const ORDERS[] = {"list", "shuffle", nullptr};

        // ---- The loader: decodes pictures and frames off the render task ----

        struct Job {
            PictureControl::Screen *screen;
            int index;
            bool frame; // an animation's next frame, not the next picture
        };
        QueueHandle_t jobs = nullptr;

        LGFX_Sprite *newSprite() {
            auto *s = new LGFX_Sprite();
            s->setPsram(true);
            s->setColorDepth(16);
            return s;
        }

        bool ensureBuffer(LGFX_Sprite *&s) {
            if (!s) s = newSprite();
            return s->getBuffer() || s->createSprite(SIZE, SIZE);
        }

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

        uint16_t swapped565(uint8_t r, uint8_t g, uint8_t b) { return __builtin_bswap16(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)); }

        // A WebP's first frame (all of a still one), laid on black.
        String decodeWebp(uint16_t *dst, const uint8_t *data, size_t length) {
            WebPData webp = {data, length};
            WebPAnimDecoderOptions opts;
            if (!WebPAnimDecoderOptionsInit(&opts)) return "WebP decoder unavailable";
            opts.color_mode = MODE_RGBA;
            opts.use_threads = 0;
            WebPAnimDecoder *dec = WebPAnimDecoderNew(&webp, &opts);
            if (!dec) return "not a readable WebP";
            String error;
            WebPAnimInfo info;
            uint8_t *rgba;
            int timestamp;
            if (!WebPAnimDecoderGetInfo(dec, &info)) {
                error = "not a readable WebP";
            } else if (info.canvas_width != SIZE || info.canvas_height != SIZE) {
                error = "not 240x240";
            } else if (!WebPAnimDecoderGetNext(dec, &rgba, &timestamp)) {
                error = "WebP decode failed";
            } else {
                for (int i = 0; i < SIZE * SIZE; i++, rgba += 4) {
                    const uint8_t a = rgba[3];
                    dst[i] = a == 255 ? swapped565(rgba[0], rgba[1], rgba[2]) : swapped565(rgba[0] * a / 255, rgba[1] * a / 255, rgba[2] * a / 255);
                }
            }
            WebPAnimDecoderDelete(dec);
            return error;
        }

        // Reads and decodes the picture the screen's ahead buffer names. An
        // animated GIF keeps its file and decoder there, to play on.
        void loadPicture(PictureControl::Screen &s, int index) {
            const uint32_t start = millis();
            PictureControl::Buffer &b = s.ahead;
            b.player.reset();
            b.delay = 0;
            String error;
            const char *format = "?";
            size_t length = 0;
            uint8_t *data = nullptr;
            if (!ensureBuffer(b.sprite)) error = "out of memory";
            else if (!(data = readFile(pictures::path(b.hash), length))) error = "can't read the file";
            uint16_t *dst = data ? (uint16_t *) b.sprite->getBuffer() : nullptr;
            if (!data) {
            } else if (data[0] == 0xFF && data[1] == 0xD8) {
                format = "jpeg";
                b.sprite->fillScreen(TFT_BLACK);
                if (!b.sprite->drawJpg(data, length, 0, 0)) error = "unreadable JPEG";
            } else if (data[0] == 'G') {
                format = "gif";
                memset(dst, 0, PICTURE_BYTES);
                const bool room = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >= KEEP_FREE;
                auto player = std::unique_ptr<GifPlayer>(new GifPlayer());
                if (player->open(data, length, SIZE, SIZE, error)) {
                    data = nullptr; // the player has it now
                    b.delay = max(player->next(dst), MIN_FRAME_MS);
                    if (!player->single() && room) {
                        b.player = std::move(player);
                        format = "animated gif";
                    } else if (!room) {
                        format = "gif, first frame only (low on memory)";
                    }
                } else {
                    data = nullptr; // freed with the player
                }
            } else if (data[0] == 'R') {
                format = "webp";
                error = decodeWebp(dst, data, length);
            } else {
                format = "png";
                auto art = makapix::decode(data, length, "", DECODE_BUDGET, error);
                if (art && (art->width != SIZE || art->height != SIZE || art->frames.empty())) error = "not 240x240";
                else if (art) {
                    const uint16_t *src = art->frames[0];
                    for (int i = 0; i < SIZE * SIZE; i++) dst[i] = __builtin_bswap16(src[i]); // sprites hold byte-swapped RGB565
                }
            }
            free(data);
            if (error.length()) {
                Serial.printf("Picture: screen %d can't show %s: %s\n", index, b.hash.c_str(), error.c_str());
            } else {
                Serial.printf("Picture: screen %d loaded %s (%s, %lu ms, loader stack %u bytes spare)\n", index, b.hash.c_str(), format,
                              (unsigned long) (millis() - start), (unsigned) uxTaskGetStackHighWaterMark(nullptr));
            }
            s.state = error.length() ? PictureControl::Failed : PictureControl::Ready;
        }

        // The shown animation's next frame: a GIF frame draws over the one
        // before, so it starts from a copy of what's showing.
        void loadFrame(PictureControl::Screen &s) {
            if (!s.shown.player || !ensureBuffer(s.frame)) {
                s.frameState = PictureControl::Failed;
                return;
            }
            uint16_t *dst = (uint16_t *) s.frame->getBuffer();
            memcpy(dst, s.shown.sprite->getBuffer(), PICTURE_BYTES);
            s.frameDelay = max(s.shown.player->next(dst), MIN_FRAME_MS);
            s.frameState = PictureControl::Ready;
        }

        void loaderTask(void *) {
            Job job;
            for (;;) {
                if (xQueueReceive(jobs, &job, portMAX_DELAY) != pdTRUE) continue;
                if (job.frame) loadFrame(*job.screen);
                else loadPicture(*job.screen, job.index);
            }
        }

        void startLoader() {
            if (jobs) return;
            jobs = xQueueCreate(12, sizeof(Job)); // each screen asks for one picture and one frame at most
            // Internal RAM. Decoding a 240x240 picture or GIF frame used at
            // most ~3.4 KB (lossless WebP the most); loadPicture() logs it.
            xTaskCreatePinnedToCore(loaderTask, "pictures", 12288, nullptr, 1, nullptr, 1);
        }

        // Asks the loader for hash in the screen's ahead buffer.
        void request(PictureControl::Screen &s, int index, const String &hash) {
            startLoader();
            s.ahead.hash = hash;
            s.state = PictureControl::Loading;
            Job job = {&s, index, false};
            if (xQueueSend(jobs, &job, 0) != pdTRUE) s.state = PictureControl::Idle; // full: asked again next tick
        }

        void requestFrame(PictureControl::Screen &s, int index) {
            startLoader();
            s.frameState = PictureControl::Loading;
            Job job = {&s, index, true};
            if (xQueueSend(jobs, &job, 0) != pdTRUE) s.frameState = PictureControl::Idle;
        }

        bool contains(const std::vector<String> &v, const String &x) { return std::find(v.begin(), v.end(), x) != v.end(); }

        // FNV-1a over the order and the pictures: checked on every draw (an
        // animation draws ~40 times a second), so it allocates nothing.
        uint32_t keyOf(JsonObjectConst p) {
            uint32_t h = 2166136261u;
            auto mix = [&h](const char *str) {
                for (; str && *str; str++) h = (h ^ (uint8_t) *str) * 16777619u;
                h = (h ^ '|') * 16777619u;
            };
            mix(p["order"] | "list");
            for (JsonVariantConst v : p["pictures"].as<JsonArrayConst>()) mix(v.as<const char *>());
            return h;
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

    // The pictures the board holds, from the list, without repeats or ones
    // that failed; shuffled if asked.
    void PictureControl::rebuild(Screen &s, JsonObjectConst p, uint32_t key) {
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
        if (s.shown.player && contains(s.sequence, s.shown.hash)) {
            // Playing: wake when this frame's time is up, or soon if the
            // next isn't decoded yet.
            if (s.frameState != Ready) return 10;
            const uint32_t on = millis() - s.frameAt;
            return on >= s.shown.delay ? 1 : min(s.shown.delay - on, TICK_MS);
        }
        // Waiting for the loader, or a slideshow to run.
        return s.state == Loading || s.sequence.size() > 1 || (s.shown.hash.isEmpty() && !s.sequence.empty()) ? TICK_MS : 0;
    }

    void PictureControl::draw(LGFX_Sprite &spr, JsonObjectConst p, const DrawContext &ctx) {
        Screen &s = screens[ctx.screen];
        const bool shuffle = strcmp(p["order"] | "list", "shuffle") == 0;
        const uint32_t key = keyOf(p);
        if (key != s.key || s.storeGeneration != pictures::generation()) rebuild(s, p, key);

        // ---- The next picture ----
        // Not while the loader decodes a frame from the picture showing.
        if (s.state != Loading && s.frameState != Loading) {
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
            const String &next = s.upcoming;
            const bool current = contains(s.sequence, s.shown.hash);
            const bool due = !current || ctx.nowMs - s.shownAt >= p["seconds"].as<uint32_t>() * 1000UL;
            if (next.length() && next != s.shown.hash) {
                if (s.state == Ready && s.ahead.hash == next) {
                    if (due) {
                        // The old picture stays ahead (an animation paused where
                        // it was), in case it's next again.
                        if (s.shown.player && ctx.nowMs != s.shownAt) {
                            Serial.printf("Picture: screen %d played %s at %.1f fps\n", ctx.screen, s.shown.hash.c_str(),
                                          (s.frameSeq - s.seqAtShow) * 1000.0f / (ctx.nowMs - s.shownAt));
                        }
                        std::swap(s.shown, s.ahead);
                        s.shownAt = s.frameAt = ctx.nowMs;
                        s.seqAtShow = s.frameSeq + 1;
                        s.frameSeq++;
                        s.frameState = Idle; // a frame decoded for the old picture
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
        const bool show = showing.length() && contains(s.sequence, showing);

        // ---- The next frame of an animation ----
        if (show && s.shown.player) {
            if (s.frameState == Failed) s.frameState = Idle;
            if (s.frameState == Ready && ctx.nowMs - s.frameAt >= s.shown.delay) {
                std::swap(s.shown.sprite, s.frame);
                s.shown.delay = s.frameDelay;
                s.frameAt = ctx.nowMs;
                s.frameSeq++;
                s.frameState = Idle;
            }
            if (s.frameState == Idle) requestFrame(s, ctx.screen);
        }

        if (show) {
            if (ctx.pictureClean && s.drawn[ctx.picture] == showing && s.drawnSeq[ctx.picture] == s.frameSeq) return; // already there
            memcpy(spr.getBuffer(), s.shown.sprite->getBuffer(), PICTURE_BYTES);
            s.drawn[ctx.picture] = showing;
            s.drawnSeq[ctx.picture] = s.frameSeq;
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
