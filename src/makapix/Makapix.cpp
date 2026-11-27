#include "Makapix.h"
#include "core/Health.h"
#include "display/Display.h"
#include "net/NetLock.h"
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <time.h>

namespace makapix {

    namespace {
        const char *BASE = "https://makapix.club/api";
        constexpr int SCREENS = display::NUM_SCREENS;
        constexpr size_t ART_BUDGET = 1024 * 1024;     // decoded bytes per artwork (all frames)
        // Makapix allows files up to 5 MiB, but anything that decodes within
        // ART_BUDGET is at most about twice that (a 32-bit BMP, an
        // incompressible lossless WebP).
        constexpr size_t MAX_DOWNLOAD = 2 * ART_BUDGET + 64 * 1024;
        // PSRAM left free for everything else while art downloads and
        // decodes. Past PSRAM, allocations fall back to internal RAM, which
        // HTTPS needs.
        constexpr size_t PSRAM_RESERVE = 1024 * 1024;
        constexpr uint32_t IN_USE_MS = 10000;            // a screen that stopped asking is let go
        constexpr uint32_t POLL_PAIRING_MS = 10000;      // credentials endpoint: 30/min/IP
        constexpr uint32_t RETRY_MS = 60000;
        constexpr uint32_t VIEW_GAP_MS = 5500;           // Makapix accepts one view per 5 s

        enum class Pairing : uint8_t { Unpaired, Pairing, Paired };

        struct Post {
            uint32_t id;
            String url, format, owner;
            size_t bytes; // decoded size, as Makapix reports it
        };

        struct FeedState {
            Feed feed;
            std::vector<Post> posts;
            size_t next = 0;
            String cursor;
            bool exhausted = false;
            uint32_t failedAt = 0;
            String problem;
        };

        struct ScreenState {
            Feed feed;
            ArtworkPtr art;
            uint32_t since = 0, wantedAt = 0;
            int feedIndex = -1;
            String problem;
        };

        SemaphoreHandle_t lock;
        struct Lock {
            Lock() { xSemaphoreTake(lock, portMAX_DELAY); }
            ~Lock() { xSemaphoreGive(lock); }
        };

        // Pairing (the key and token are secrets: NVS only, never reported whole)
        Pairing pairing = Pairing::Unpaired;
        String playerKey, apiToken, code, pairingProblem;
        uint32_t codeExpiresAt = 0, lastPoll = 0, provisionFailedAt = 0;
        bool forgetRequested = false;

        ScreenState screens[SCREENS];
        std::vector<FeedState> feeds;
        std::vector<uint32_t> viewQueue;
        uint32_t lastView = 0;

        // Signed, because wantedAt can be 1 ms ahead of now: it's stored with
        // its low bit set so that it's never 0.
        bool inUse(const ScreenState &s, uint32_t now) {
            return s.wantedAt && (int32_t) (now - s.wantedAt) < (int32_t) IN_USE_MS;
        }

        bool anyInUse() {
            const uint32_t now = millis();
            for (const ScreenState &s : screens) {
                if (inUse(s, now)) return true;
            }
            return false;
        }

        void savePairing() {
            Preferences p;
            p.begin("makapix", false);
            p.putString("key", playerKey);
            p.putString("token", apiToken);
            p.end();
        }

        // ---- HTTP ----
        // Returns the HTTP status (or <= 0), with the reply parsed into doc.
        int request(const char *method, const String &url, const String &body, JsonDocument *doc, bool auth) {
            net::HttpsTurn turn;
            HTTPClient http;
            http.setConnectTimeout(8000);
            http.setTimeout(10000);
            if (!http.begin(url)) return -1;
            http.addHeader("Content-Type", "application/json");
            if (auth) http.addHeader("Authorization", "Bearer " + apiToken);
            const int code = strcmp(method, "GET") == 0 ? http.GET() : http.POST(body);
            if (code > 0 && doc) {
                if (deserializeJson(*doc, http.getString())) doc->clear();
            }
            http.end();
            return code;
        }

        // A player RPC call; renews a rejected token once.
        int rpc(const JsonDocument &req, JsonDocument &reply) {
            String body;
            serializeJson(req, body);
            int code = request("POST", String(BASE) + "/player/rpc", body, &reply, true);
            if (code == 401) {
                JsonDocument rotated;
                const int rc = request("POST", String(BASE) + "/player/" + playerKey + "/token/rotate", "{}", &rotated, false);
                if (rc == 200 && rotated["api_token"].is<const char *>()) {
                    apiToken = rotated["api_token"].as<String>();
                    savePairing();
                    health::event("Makapix: token renewed");
                    code = request("POST", String(BASE) + "/player/rpc", body, &reply, true);
                } else if (rc == 404) {
                    // The player was deleted on makapix.club: pair again.
                    health::event("Makapix: player no longer registered, pairing again");
                    forgetRequested = true;
                }
            }
            return code;
        }

        // ---- Pairing ----
        void pair() {
            if (pairing == Pairing::Paired) return;
            const uint32_t now = millis();
            if (pairing == Pairing::Unpaired || (int32_t) (now - codeExpiresAt) > 0) {
                if (provisionFailedAt && now - provisionFailedAt < RETRY_MS) return;
                JsonDocument reply;
                const int rc = request("POST", String(BASE) + "/player/provision",
                                       "{\"device_model\":\"orbit-station-5x240\",\"firmware_version\":\"" ORBIT_STATION_VERSION "\"}", &reply, false);
                if (rc != 200 && rc != 201) {
                    provisionFailedAt = now;
                    pairingProblem = rc == 429 ? "Makapix busy, retrying" : "Can't reach Makapix";
                    health::event("Makapix: provisioning failed (HTTP %d)", rc);
                    return;
                }
                provisionFailedAt = 0;
                playerKey = reply["player_key"].as<String>();
                code = reply["registration_code"].as<String>();
                codeExpiresAt = now + 14 * 60 * 1000; // valid 15 minutes
                pairing = Pairing::Pairing;
                pairingProblem = "";
                lastPoll = now;
                health::event("Makapix: register this board at makapix.club with code %s", code.c_str());
                return;
            }
            if (now - lastPoll < POLL_PAIRING_MS) return;
            lastPoll = now;
            JsonDocument creds;
            const int rc = request("GET", String(BASE) + "/player/" + playerKey + "/credentials", "", &creds, false);
            if (rc != 200) return; // 404 until it's registered
            if (!creds["api_token"].is<const char *>()) {
                pairingProblem = "Registered, but no token came back";
                return;
            }
            apiToken = creds["api_token"].as<String>();
            savePairing();
            Lock l;
            pairing = Pairing::Paired;
            code = "";
            health::event("Makapix: paired");
        }

        // ---- Feeds ----
        int feedFor(const Feed &f) {
            for (size_t i = 0; i < feeds.size(); i++) {
                if (feeds[i].feed.sameFeed(f)) return i;
            }
            FeedState fs;
            fs.feed = f;
            feeds.push_back(fs);
            return feeds.size() - 1;
        }

        bool refill(FeedState &fs) {
            JsonDocument req, reply;
            req["request_type"] = "query_posts";
            req["channel"] = fs.feed.channel;
            if (fs.feed.user.length()) req["user_handle"] = fs.feed.user;
            if (fs.feed.hashtag.length()) req["hashtag"] = fs.feed.hashtag;
            req["sort"] = fs.feed.sort;
            if (fs.feed.sort == "random") req["random_seed"] = (int32_t) (esp_random() & 0x7FFFFFFF);
            if (fs.cursor.length()) req["cursor"] = fs.cursor;
            req["limit"] = 50;
            JsonArray fields = req["include_fields"].to<JsonArray>();
            for (const char *f : {"owner_handle", "width", "height", "frame_count"}) fields.add(f);

            const int rc = rpc(req, reply);
            if (rc != 200 || !reply["success"]) {
                fs.problem = rc == 404 ? "User not found" : reply["error"].is<const char *>() ? reply["error"].as<String>() : "Makapix unavailable";
                health::event("Makapix: query failed (HTTP %d): %s", rc, fs.problem.c_str());
                return false;
            }
            fs.posts.clear();
            fs.next = 0;
            for (JsonObject p : reply["posts"].as<JsonArray>()) {
                if (p["kind"] != "artwork" || !p["art_url"].is<const char *>()) continue;
                // Skip what can't fit before downloading it.
                const size_t w = p["width"] | 0, h = p["height"] | 0, n = max((int) (p["frame_count"] | 1), 1);
                const size_t bytes = w * h * 2 * n;
                if (bytes > ART_BUDGET) continue;
                fs.posts.push_back({p["post_id"].as<uint32_t>(), p["art_url"].as<String>(), p["native_format"].as<String>(),
                                    p["owner_handle"] | "", bytes ? bytes : ART_BUDGET});
            }
            fs.cursor = reply["has_more"] ? reply["next_cursor"].as<String>() : String();
            fs.problem = fs.posts.empty() ? "Nothing to show in this channel" : "";
            return !fs.posts.empty();
        }

        // ---- Downloads ----
        // True while downloads fail for want of a connection (not a bad file).
        bool waitingForNetwork = false;

        // True while art is skipped for want of PSRAM.
        bool waitingForMemory = false;

        // Whether need more bytes of PSRAM can be used, one block of block
        // bytes among them, and still leave PSRAM_RESERVE free.
        bool roomFor(size_t need, size_t block) {
            return heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >= need + PSRAM_RESERVE &&
                   heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) >= block;
        }

        // Why fetchArt failed, when it wasn't this artwork's fault.
        enum class Shortage : uint8_t { None, Network, Memory };

        // Whether data starts as one of the formats decode() reads (PNG,
        // GIF, WebP, BMP) does. A file that does but won't decode is the
        // artwork's problem; one that doesn't is someone else's reply.
        bool looksLikeImage(const uint8_t *d, int size) {
            if (size < 12) return false;
            return !memcmp(d, "\x89PNG", 4) || !memcmp(d, "GIF8", 4) ||
                   (!memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4)) || !memcmp(d, "BM", 2);
        }

        // shortage is Network when the download failed for want of a
        // connection (DNS, connect, timeout) or something other than
        // Makapix answered, Memory when there wasn't room for it; None for a
        // problem with this artwork.
        std::shared_ptr<Artwork> fetchArt(const Post &p, String &error, Shortage &shortage) {
            shortage = Shortage::None;
            if (!roomFor(p.bytes, 0)) {
                shortage = Shortage::Memory;
                error = "not enough memory";
                return nullptr;
            }
            uint8_t *buf = nullptr;
            int size = 0;
            {
            net::HttpsTurn turn; // released before decoding, which can take a while
            HTTPClient http;
            http.setConnectTimeout(8000);
            http.setTimeout(15000);
            if (!http.begin(p.url)) {
                error = "bad art URL";
                return nullptr;
            }
            const int code = http.GET();
            if (code != 200) {
                http.end();
                if (code < 0) shortage = Shortage::Network; // HTTPClient's own errors: no connection at all
                error = "download failed (HTTP " + String(code) + ")";
                return nullptr;
            }
            size = http.getSize();
            if (size <= 0 || (size_t) size > MAX_DOWNLOAD) {
                http.end();
                error = "unexpected file size";
                return nullptr;
            }
            if (!roomFor(size + p.bytes, size)) {
                http.end();
                shortage = Shortage::Memory;
                error = "not enough memory";
                return nullptr;
            }
            buf =(uint8_t *) heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
            if (!buf) {
                http.end();
                error = "out of memory";
                return nullptr;
            }
            WiFiClient *stream = http.getStreamPtr();
            int got = 0;
            const uint32_t start = millis();
            while (got < size && millis() - start < 20000) {
                const int n = stream->read(buf + got, size - got);
                if (n > 0) got += n;
                else delay(5);
            }
            http.end();
            if (got != size) {
                free(buf);
                error = "download cut short";
                return nullptr;
            }
            }
            if (!looksLikeImage(buf, size)) {
                // Something on the way answered instead of Makapix: a router
                // blocking the board, a captive portal, a proxy's error page.
                // Not this artwork's fault, so keep it for later.
                shortage = Shortage::Network;
                error = "got something other than art";
                if (buf[0] == '<') error += " (a web page)";
                free(buf);
                return nullptr;
            }
            std::shared_ptr<Artwork> art = decode(buf, size, p.format, ART_BUDGET, error);
            free(buf);
            if (art) {
                art->postId = p.id;
                art->owner = p.owner;
            }
            return art;
        }

        void reportView(uint32_t postId, const Feed &f) {
            const time_t t = time(nullptr);
            if (t < 1700000000) return; // clock not set
            char iso[24];
            strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", gmtime(&t));
            JsonDocument body;
            body["post_id"] = postId;
            body["timestamp"] = iso;
            body["timezone"] = "";
            body["intent"] = "channel";
            body["play_order"] = f.sort == "random" ? 2 : f.sort == "created_at" ? 1 : 0;
            body["channel"] = f.channel;
            body["channel_user_sqid"] = nullptr;
            if (f.hashtag.length()) body["channel_hashtag"] = f.hashtag;
            else body["channel_hashtag"] = nullptr;
            String s;
            serializeJson(body, s);
            net::HttpsTurn turn;
            HTTPClient http;
            http.setTimeout(8000);
            if (http.begin(String(BASE) + "/player/events/view")) {
                http.addHeader("Content-Type", "application/json");
                http.addHeader("Authorization", "Bearer " + apiToken);
                http.POST(s);
                http.end();
            }
        }

        // One screen's next artwork. Returns true if it did work.
        bool serveOne() {
            int pick = -1;
            Feed feed;
            int fi;
            {
                Lock l;
                const uint32_t now = millis();
                for (int i = 0; i < SCREENS; i++) {
                    ScreenState &s = screens[i];
                    if (!inUse(s, now)) {
                        s.art.reset(); // let go of art nobody's showing
                        continue;
                    }
                    if (!s.art || now - s.since >= s.feed.changeMs) {
                        pick = i;
                        break;
                    }
                }
                if (pick < 0) return false;
                if (feeds.size() > 10) feeds.clear(); // feeds nobody uses any more
                feed = screens[pick].feed;
                fi = feedFor(feed);
            }
            FeedState &fs = feeds[fi];
            if (fs.failedAt && millis() - fs.failedAt < RETRY_MS) return false;
            if (fs.next >= fs.posts.size() && !refill(fs)) {
                fs.failedAt = millis();
                Lock l;
                screens[pick].problem = fs.problem;
                return true;
            }
            fs.failedAt = 0;
            const Post post = fs.posts[fs.next++];
            String error;
            Shortage shortage;
            auto art = fetchArt(post, error, shortage);
            if (!art && shortage == Shortage::Memory) {
                // Other screens' art fills PSRAM. Keep showing what's there and
                // try a later (perhaps smaller) post in a while.
                fs.failedAt = millis();
                if (!waitingForMemory) {
                    health::event("Makapix: not enough memory for post %u (%u KB PSRAM free), retrying every %lus",
                                  post.id, (unsigned) (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                                  (unsigned long) (RETRY_MS / 1000));
                    waitingForMemory = true;
                }
                return true;
            }
            if (!art && shortage == Shortage::Network) {
                // Not this artwork's fault: keep it for later instead of skipping
                // through the whole queue while the network is down.
                fs.next--;
                fs.failedAt = millis();
                if (!waitingForNetwork) {
                    health::event("Makapix: can't reach Makapix (%s), retrying every %lus", error.c_str(),
                                  (unsigned long) (RETRY_MS / 1000));
                    waitingForNetwork = true;
                }
                return true;
            }
            if (!art) {
                health::event("Makapix: skipped post %u: %s", post.id, error.c_str());
                return true; // the next post will be tried on the next pass
            }
            if (waitingForNetwork) {
                health::event("Makapix: reachable again");
                waitingForNetwork = false;
            }
            waitingForMemory = false;
            Serial.printf("Makapix: screen %d shows post %u (%dx%d, %u frames) by %s\n", pick, post.id, art->width, art->height,
                          (unsigned) art->frames.size(), post.owner.c_str());
            {
                Lock l;
                ScreenState &s = screens[pick];
                if (s.feed.sameFeed(feed)) {
                    s.art = art;
                    s.since = millis();
                    s.problem = "";
                }
                viewQueue.push_back(post.id);
                if (viewQueue.size() > 10) viewQueue.erase(viewQueue.begin());
            }
            if (millis() - lastView >= VIEW_GAP_MS) {
                reportView(post.id, feed);
                lastView = millis();
                Lock l;
                viewQueue.clear();
            }
            return true;
        }

        void task(void *) {
            for (;;) {
                vTaskDelay(pdMS_TO_TICKS(200));
                health::beat(health::Task::Makapix);
                if (forgetRequested) {
                    forgetRequested = false;
                    Preferences p;
                    p.begin("makapix", false);
                    p.clear();
                    p.end();
                    Lock l;
                    pairing = Pairing::Unpaired;
                    playerKey = apiToken = code = "";
                    feeds.clear();
                    for (ScreenState &s : screens) s.art.reset();
                    health::event("Makapix: pairing forgotten");
                }
                if (WiFi.status() != WL_CONNECTED || !anyInUse()) continue;
                if (pairing != Pairing::Paired) {
                    pair();
                    continue;
                }
                serveOne();
            }
        }
    }

    void begin() {
        lock = xSemaphoreCreateMutex();
        Preferences p;
        p.begin("makapix", true);
        playerKey = p.getString("key", "");
        apiToken = p.getString("token", "");
        p.end();
        if (playerKey.length() && apiToken.length()) pairing = Pairing::Paired;
        // libwebp's lossless decoder wants a deep stack.
        xTaskCreatePinnedToCore(task, "makapix", 32768, nullptr, 1, nullptr, 1);
    }

    Showing show(int screen, const Feed &feed) {
        Lock l;
        ScreenState &s = screens[screen];
        if (!s.feed.sameFeed(feed)) {
            s.art.reset(); // a different feed: start afresh
            s.problem = "";
        }
        s.feed = feed;
        s.wantedAt = millis() | 1;
        Showing out;
        out.art = s.art;
        out.since = s.since;
        if (pairing != Pairing::Paired) {
            out.code = code;
            out.message = pairingProblem.length() ? pairingProblem : code.length() ? String("Register at makapix.club") : String("Connecting to Makapix");
        } else if (!s.art) {
            out.message = s.problem.length() ? s.problem : String("Loading art");
        }
        return out;
    }

    void statusJson(JsonObject out) {
        Lock l;
        out["state"] = pairing == Pairing::Paired ? "paired" : pairing == Pairing::Pairing ? "pairing" : "unpaired";
        if (pairing == Pairing::Pairing) {
            out["registrationCode"] = code;
            out["codeExpiresInSeconds"] = max((int32_t) (codeExpiresAt - millis()) / 1000, (int32_t) 0);
        }
        if (playerKey.length() >= 4) out["playerKey"] = "****" + playerKey.substring(playerKey.length() - 4);
        if (pairingProblem.length()) out["problem"] = pairingProblem;
    }

    void forget() { forgetRequested = true; }

}
