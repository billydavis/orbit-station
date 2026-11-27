#include "Orbit.h"
#include "Control.h"
#include "Pictures.h"
#include "Transition.h"
#include "controls/AnalogClockControl.h"
#include "controls/BlankControl.h"
#include "controls/ClaudeUsageControl.h"
#include "controls/CountdownControl.h"
#include "controls/CustomControl.h"
#include "controls/GaugeControl.h"
#include "controls/MakapixControl.h"
#include "controls/PictureControl.h"
#include "controls/ScreensaverControl.h"
#include "controls/SysMonitorControl.h"
#include "controls/TickerControl.h"
#include "controls/TimeControl.h"
#include "controls/WeatherControl.h"
#include "data/Feeds.h"
#include "core/Health.h"
#include "display/Display.h"
#include "net/Network.h"
#include "ui/StatusScreen.h"
#include <LittleFS.h>
#include <Preferences.h>
#include <uri/UriBraces.h>

namespace orbit {

    namespace {
        constexpr int NUM_SCREENS = display::NUM_SCREENS;
        const char *NOT_SAVED_WARNING = "applied, but it could not be saved - it will not survive a reboot";

        // ---- Controls ----
        BlankControl blankControl;
        TimeControl timeControl;
        AnalogClockControl analogClockControl;
        GaugeControl gaugeControl;
        SysMonitorControl sysMonitorControl;
        CountdownControl countdownControl;
        CustomControl customControl;
        ScreensaverControl screensaverControl;
        WeatherControl weatherControl;
        TickerControl tickerControl;
        MakapixControl makapixControl;
        ClaudeUsageControl claudeUsageControl;
        PictureControl pictureControl;
        Control *const CONTROLS[] = {&blankControl,      &timeControl,        &analogClockControl, &gaugeControl,
                                     &sysMonitorControl, &countdownControl,   &customControl,      &screensaverControl,
                                     &weatherControl,    &tickerControl,      &makapixControl,     &claudeUsageControl,
                                     &pictureControl};
        constexpr int BLANK = 0;

        // Every control orbit-api v1 defines. Ones not in CONTROLS yet are
        // reported as "not built" (501), not "unknown" (400), as info-orbs does.
        const char *const V1_CONTROLS[] = {"blank", "time", "analogClock", "gauge", "sysMonitor", "screensaver",
                                           "countdown", "weather", "ticker", "custom", "asteroids"};

        int findControl(const String &name) {
            for (int i = 0; i < (int) (sizeof(CONTROLS) / sizeof(CONTROLS[0])); i++) {
                if (name == CONTROLS[i]->name()) return i;
            }
            return -1;
        }

        bool isV1Control(const String &name) {
            for (const char *c : V1_CONTROLS) {
                if (name == c) return true;
            }
            return false;
        }

        // ---- Layout ----
        struct Slot {
            int control = BLANK;
            JsonDocument params;
            uint32_t updatedAt = 0; // millis() of the last API write; 0 after a restart
            bool dirty = true;      // needs drawing now
            uint32_t lastDraw = 0;
            // What the screen showed before a control that isn't saved (a
            // countdown) took it over: put back on stop, and saved in its place.
            int previous = -1;
            JsonDocument previousParams;
            // Per picture: it holds exactly what the control last drew into
            // it (DrawContext::pictureClean). Cleared whenever the control or
            // its params change, or something else draws into the picture.
            bool pictureClean[2] = {false, false};
        };
        Slot slots[NUM_SCREENS];
        SemaphoreHandle_t layoutLock; // the API (core 1) writes while the renderer (core 0) reads
        TaskHandle_t renderHandle;

        // Longest waits for and holds of the layout lock, by the renderer and
        // by everyone else (the API): since the last frame report, and since boot.
        LockStats lockWindow, lockBoot;

        void noteMax(uint32_t &slot, uint32_t us) {
            if (us > slot) slot = us;
        }

        struct Lock {
            bool render;
            uint32_t held;
            Lock() : render(xTaskGetCurrentTaskHandle() == renderHandle) {
                const uint32_t asked = micros();
                xSemaphoreTake(layoutLock, portMAX_DELAY);
                held = micros();
                const uint32_t waited = held - asked;
                noteMax(render ? lockWindow.renderWaitUs : lockWindow.apiWaitUs, waited);
                noteMax(render ? lockBoot.renderWaitUs : lockBoot.apiWaitUs, waited);
            }
            ~Lock() {
                const uint32_t hold = micros() - held;
                xSemaphoreGive(layoutLock);
                noteMax(render ? lockWindow.renderHoldUs : lockWindow.apiHoldUs, hold);
                noteMax(render ? lockBoot.renderHoldUs : lockBoot.apiHoldUs, hold);
            }
        };

        // Checks one {control, params} entry. For an API write, screen is the
        // target (the layout must be locked) so a control can check the write
        // against what the screen shows; -1 when restoring the saved layout.
        // Returns 0, or 400/501 with error.
        int parseConfig(JsonObjectConst entry, int screen, int &control, JsonDocument &params, String &error) {
            if (!entry["control"].is<const char *>()) {
                error = "missing or invalid 'control'";
                return 400;
            }
            String name = entry["control"].as<String>();
            JsonVariantConst p = entry["params"];
            JsonDocument alias;
            if (name == "asteroids") {
                // A control of its own before it became a screensaver effect;
                // still accepted from older clients and saved layouts.
                name = "screensaver";
                if (p.is<JsonObjectConst>()) alias.set(p);
                alias["effect"] = "asteroids";
                p = alias.as<JsonVariantConst>();
            }
            control = findControl(name);
            if (control < 0) {
                if (isV1Control(name)) {
                    error = "control '" + name + "' isn't built into this firmware - see GET " + API_BASE + "/capabilities";
                    return 501;
                }
                error = "unknown control '" + name + "'";
                return 400;
            }
            if (!p.isNull() && !p.is<JsonObjectConst>()) {
                error = "'params' must be an object";
                return 400;
            }
            params.clear();
            JsonObject out = params.to<JsonObject>();
            if (screen < 0) return CONTROLS[control]->parse(p.as<JsonObjectConst>(), out, error);
            const bool same = slots[screen].control == control;
            WriteContext ctx = {screen, same, same ? slots[screen].params.as<JsonObjectConst>() : JsonObjectConst()};
            return CONTROLS[control]->parseWrite(ctx, p.as<JsonObjectConst>(), out, error);
        }

        void show(int screen, int control, const JsonDocument &params) {
            Slot &s = slots[screen];
            s.control = control;
            s.params = params;
            s.dirty = true;
            s.pictureClean[0] = s.pictureClean[1] = false;
        }

        // Applies an API write. Call with the layout locked.
        void applyWrite(int screen, int control, const JsonDocument &params, JsonObjectConst request) {
            Slot &s = slots[screen];
            Control *next = CONTROLS[control];
            if (!next->saved() && CONTROLS[s.control]->saved()) {
                // Taking over the screen: remember what was there.
                s.previous = s.control;
                s.previousParams = s.params;
            } else if (next->saved()) {
                s.previous = -1;
                s.previousParams.clear();
            }
            show(screen, control, params);
            s.updatedAt = millis();
            if (next->applied(screen, request, s.params.as<JsonObjectConst>()) == AfterWrite::Restore) {
                const int back = s.previous >= 0 ? s.previous : BLANK;
                JsonDocument backParams = s.previousParams;
                s.previous = -1;
                s.previousParams.clear();
                show(screen, back, backParams);
                CONTROLS[back]->applied(screen, JsonObjectConst(), s.params.as<JsonObjectConst>());
            }
        }

        void reportSlot(int screen, JsonObject out) {
            const Slot &s = slots[screen];
            out["screen"] = screen;
            out["control"] = CONTROLS[s.control]->name();
            // Always an object, {} for a control without params, never null.
            JsonObject p = out["params"].to<JsonObject>();
            p.set(s.params.as<JsonObjectConst>());
            CONTROLS[s.control]->report(screen, p);
            out["updatedAt"] = s.updatedAt;
        }

        // ---- Saving ----
        // The layout (which control each screen shows, and its settings) is
        // one JSON blob in NVS. Drawings are too big for NVS (about 8 KB to
        // spare) and go in LittleFS files instead. Each is written only when
        // its saved form changes.
        String savedLayout;               // what's in NVS
        String savedDrawing[NUM_SCREENS]; // what's in each drawing file ("" = none)
        bool filesReady = false;

        String drawingPath(int screen) { return "/orbit-draw" + String(screen) + ".json"; }

        // What a screen is saved as: its control, unless that's never saved
        // (countdown); then what it showed before.
        void savedConfig(int screen, int &control, JsonObjectConst &params) {
            const Slot &s = slots[screen];
            if (CONTROLS[s.control]->saved()) {
                control = s.control;
                params = s.params.as<JsonObjectConst>();
            } else if (s.previous >= 0) {
                control = s.previous;
                params = s.previousParams.as<JsonObjectConst>();
            } else {
                control = BLANK;
                params = JsonObjectConst();
            }
        }

        // Writes whatever changed. Call with the layout locked. Returns false
        // if something couldn't be written.
        bool persistLayout() {
            bool ok = true;
            JsonDocument doc;
            JsonArray screens = doc["screens"].to<JsonArray>();
            for (int i = 0; i < NUM_SCREENS; i++) {
                int control;
                JsonObjectConst params;
                savedConfig(i, control, params);
                JsonObject e = screens.add<JsonObject>();
                e["control"] = CONTROLS[control]->name();

                String drawing;
                if (CONTROLS[control]->savedInFile()) {
                    serializeJson(params, drawing);
                } else {
                    JsonObject p = e["params"].to<JsonObject>();
                    p.set(params);
                    CONTROLS[control]->stripLive(p); // live readings never go to flash
                }
                if (drawing != savedDrawing[i]) {
                    bool written = mountFiles();
                    if (written && drawing.length()) {
                        File f = LittleFS.open(drawingPath(i), "w");
                        written = f && f.print(drawing) == drawing.length();
                        if (f) f.close();
                        if (!written) LittleFS.remove(drawingPath(i)); // never leave half a drawing
                    } else if (written) {
                        LittleFS.remove(drawingPath(i));
                    }
                    if (written) {
                        savedDrawing[i] = drawing;
                        Serial.printf("OrbIt: saved screen %d's drawing (%u bytes)\n", i, (unsigned) drawing.length());
                    } else {
                        health::event("OrbIt: couldn't save screen %d's drawing", i);
                        ok = false;
                    }
                }
            }
            String layout;
            serializeJson(doc, layout);
            if (layout != savedLayout) {
                Preferences prefs;
                const bool written = prefs.begin("orbit", false) && prefs.putString("layout", layout) == layout.length();
                prefs.end();
                if (written) {
                    savedLayout = layout;
                    Serial.println("OrbIt: saved layout to NVS");
                } else {
                    health::event("OrbIt: couldn't save the layout");
                    ok = false;
                }
            }
            return ok;
        }

        void setDefaultLayout() {
            // A fresh board: the time with date and weekday on the middle screen.
            JsonDocument params;
            params["showDate"] = true;
            params["showDay"] = true;
            params["format24Hour"] = false;
            show(2, findControl("time"), params);
        }

        void loadLayout() {
            Preferences prefs;
            prefs.begin("orbit", true);
            const String saved = prefs.getString("layout", "");
            prefs.end();
            JsonDocument doc;
            if (saved.isEmpty() || deserializeJson(doc, saved) || !doc["screens"].is<JsonArray>()) {
                if (saved.length()) Serial.println("OrbIt: saved layout unreadable, using the default");
                setDefaultLayout();
                return;
            }
            int i = 0;
            for (JsonObject entry : doc["screens"].as<JsonArray>()) {
                if (i >= NUM_SCREENS) break;
                const int c = findControl(entry["control"] | "");
                if (c >= 0 && CONTROLS[c]->savedInFile() && mountFiles()) {
                    // The drawing is in its own file.
                    File f = LittleFS.open(drawingPath(i), "r");
                    if (f) {
                        savedDrawing[i] = f.readString();
                        f.close();
                        JsonDocument drawing;
                        if (!deserializeJson(drawing, savedDrawing[i])) entry["params"] = drawing;
                    }
                }
                int control;
                JsonDocument params;
                String error;
                if (parseConfig(entry, -1, control, params, error) == 0) {
                    show(i, control, params);
                } else {
                    Serial.printf("OrbIt: saved screen %d not restored (%s)\n", i, error.c_str());
                }
                i++;
            }
            savedLayout = saved;
            Serial.println("OrbIt: restored the saved layout");
        }

        // ---- Drawing (core 0) ----
        // Screen 1 shows the network status while the board isn't online, and
        // for a while after it comes online so its address can be read.
        constexpr uint32_t SHOW_ADDRESS_MS = 15000;
        constexpr uint32_t STATUS_REFRESH_MS = 250;

        bool statusShowing() {
            static uint32_t onlineSince = 0;
            if (net::status().state != net::State::Connected) {
                onlineSince = 0;
                return true;
            }
            if (onlineSince == 0) onlineSince = millis() | 1;
            return millis() - onlineSince < SHOW_ADDRESS_MS;
        }

        uint32_t drawUs[NUM_SCREENS], drawCount[NUM_SCREENS]; // for the serial frame report
        volatile bool stallRequested = false;

        void renderTask(void *) {
            bool statusWasShowing = false;
            for (;;) {
                health::beat(health::Task::Render);
                if (stallRequested) {
                    // Watchdog test (serial z): stop responding for a while.
                    stallRequested = false;
                    vTaskDelay(pdMS_TO_TICKS(15000));
                }
                // Screens the display core needs whole again (re-init, refresh).
                if (const uint32_t wanted = display::takeFrameRequests()) {
                    for (int i = 0; i < NUM_SCREENS; i++) {
                        if (wanted & (1u << i)) slots[i].dirty = true;
                    }
                }
                const bool status = statusShowing();
                if (statusWasShowing && !status) slots[0].dirty = true; // back to screen 1's own content
                statusWasShowing = status;
                uint32_t sleepMs = 20; // until the next screen is due

                for (int i = 0; i < NUM_SCREENS; i++) {
                    Slot &s = slots[i];
                    const uint32_t now = millis();
                    uint32_t every;
                    bool dirty;
                    if (i == 0 && status) {
                        every = STATUS_REFRESH_MS;
                        dirty = false;
                    } else {
                        Lock lock;
                        every = CONTROLS[s.control]->refreshMs(i, s.params.as<JsonObjectConst>());
                        dirty = s.dirty;
                        if (transition::active(i)) every = every ? min(every, transition::FRAME_MS) : transition::FRAME_MS;
                    }
                    const uint32_t since = now - s.lastDraw;
                    if (!dirty && !(every && since >= every)) {
                        if (every) sleepMs = min(sleepMs, every - since);
                        continue;
                    }

                    const uint32_t started = micros();
                    LGFX_Sprite &spr = display::beginFrame(i);
                    const int picture = display::pictureIndex(i);
                    uint32_t content = 0;
                    char choice[16] = "none";
                    {
                        Lock lock;
                        if (i == 0 && status) {
                            ui::drawStatus(spr);
                            s.pictureClean[picture] = false;
                        } else {
                            DrawContext ctx = {i, now, picture, s.pictureClean[picture]};
                            CONTROLS[s.control]->draw(spr, s.params.as<JsonObjectConst>(), ctx);
                            // Set with the lock held, so a write that changes
                            // the control right after can't be marked clean.
                            s.pictureClean[picture] = true;
                            s.dirty = false;
                            content = CONTROLS[s.control]->contentId(i);
                            strlcpy(choice, s.params["transition"] | "none", sizeof(choice));
                        }
                        // Keep a steady cadence: count from when this frame was
                        // due, unless it's fallen a whole frame behind.
                        s.lastDraw = every && since < 2 * every ? s.lastDraw + every : now;
                    }
                    // Outside the lock: the API shouldn't wait for this.
                    if (i == 0 && status) {
                        transition::cancel(i);
                    } else if (transition::apply(i, spr, content, choice)) {
                        Lock lock;
                        s.pictureClean[picture] = false; // the change being played is drawn over it
                    }
                    display::endFrame(i);
                    drawUs[i] += micros() - started;
                    drawCount[i]++;
                    if (every) sleepMs = min(sleepMs, every);
                }
                vTaskDelay(pdMS_TO_TICKS(max(sleepMs, (uint32_t) 1)));
            }
        }

        // ---- orbit-api ----
        WebServer *server;

        void sendJson(int code, const JsonDocument &doc) {
            String body;
            serializeJson(doc, body);
            server->send(code, "application/json", body);
        }

        void sendError(int code, const String &message) {
            JsonDocument doc;
            doc["error"] = message;
            sendJson(code, doc);
        }

        bool readBody(JsonDocument &doc) {
            if (!server->hasArg("plain")) {
                sendError(400, "missing JSON body");
                return false;
            }
            if (deserializeJson(doc, server->arg("plain"))) {
                sendError(400, "malformed JSON body");
                return false;
            }
            return true;
        }

        void handleGetScreens() {
            JsonDocument doc;
            JsonArray screens = doc["screens"].to<JsonArray>();
            Lock lock;
            for (int i = 0; i < NUM_SCREENS; i++) reportSlot(i, screens.add<JsonObject>());
            sendJson(200, doc);
        }

        void handleGetScreen(int screen) {
            JsonDocument doc;
            Lock lock;
            reportSlot(screen, doc.to<JsonObject>());
            sendJson(200, doc);
        }

        void handlePostScreen(int screen) {
            JsonDocument body;
            if (!readBody(body)) return;
            if (!body.is<JsonObject>()) {
                sendError(400, "body must be a JSON object");
                return;
            }
            Lock lock;
            int control;
            JsonDocument params;
            String error;
            if (int status = parseConfig(body.as<JsonObjectConst>(), screen, control, params, error)) {
                sendError(status, error);
                return;
            }
            applyWrite(screen, control, params, body["params"].as<JsonObjectConst>());
            JsonDocument doc;
            if (!persistLayout()) doc["warning"] = NOT_SAVED_WARNING;
            reportSlot(screen, doc.to<JsonObject>());
            sendJson(200, doc);
        }

        void handlePostScreens() {
            JsonDocument body;
            if (!readBody(body)) return;
            if (!body["screens"].is<JsonArray>()) {
                sendError(400, "body must be {\"screens\": [...]}");
                return;
            }
            // Check every entry before applying any, so a bad one can't leave
            // half a batch applied.
            Lock lock;
            int screens[NUM_SCREENS], controls[NUM_SCREENS];
            JsonDocument params[NUM_SCREENS];
            JsonObjectConst requests[NUM_SCREENS];
            int count = 0;
            for (JsonObjectConst entry : body["screens"].as<JsonArrayConst>()) {
                if (count >= NUM_SCREENS) {
                    sendError(400, "too many entries in 'screens' (max " + String(NUM_SCREENS) + ")");
                    return;
                }
                if (!entry["screen"].is<int>()) {
                    sendError(400, "each entry requires an integer 'screen'");
                    return;
                }
                const int screen = entry["screen"];
                if (screen < 0 || screen >= NUM_SCREENS) {
                    sendError(400, "'screen' must be between 0 and " + String(NUM_SCREENS - 1));
                    return;
                }
                String error;
                if (int status = parseConfig(entry, screen, controls[count], params[count], error)) {
                    sendError(status, "screen " + String(screen) + ": " + error);
                    return;
                }
                requests[count] = entry["params"].as<JsonObjectConst>();
                screens[count++] = screen;
            }
            for (int i = 0; i < count; i++) applyWrite(screens[i], controls[i], params[i], requests[i]);
            JsonDocument doc;
            if (!persistLayout()) doc["warning"] = NOT_SAVED_WARNING;
            JsonArray out = doc["screens"].to<JsonArray>();
            for (int i = 0; i < NUM_SCREENS; i++) reportSlot(i, out.add<JsonObject>());
            sendJson(200, doc);
        }

        // Stores a clock face file for a screen (see AnalogClockControl). If
        // the screen shows the analog clock, it switches to the new clock
        // face; otherwise a later write picks it with clockFaceHash.
        void handlePutClockFace(int screen) {
            if (!server->hasArg("plain")) {
                sendError(400, "missing JSON body");
                return;
            }
            String hash, id, error;
            size_t bytes;
            {
                // Checked and written without the lock: decoding and writing
                // flash take a while, and the screens keep drawing meanwhile.
                const String body = server->arg("plain");
                bytes = body.length();
                if (int status = analogClockControl.check(screen, body, hash, id, error)) {
                    sendError(status, error);
                    return;
                }
            }
            Lock lock;
            if (int status = analogClockControl.install(screen, hash, error)) {
                sendError(status, error);
                return;
            }
            Slot &s = slots[screen];
            JsonDocument doc;
            const bool shown = CONTROLS[s.control] == &analogClockControl;
            if (shown) {
                s.params["clockFaceId"] = id;
                s.params["clockFaceHash"] = hash;
                s.updatedAt = millis();
                s.dirty = true;
                s.pictureClean[0] = s.pictureClean[1] = false;
                if (!persistLayout()) doc["warning"] = NOT_SAVED_WARNING;
            }
            doc["screen"] = screen;
            doc["clockFaceHash"] = hash;
            doc["bytes"] = bytes;
            doc["shown"] = shown;
            sendJson(200, doc);
        }

        // ---- Pictures (see Pictures.h) ----

        // The pictures the screens list, which the store must keep.
        std::vector<String> picturesInUse() {
            std::vector<String> out;
            Lock lock;
            for (Slot &s : slots) {
                for (JsonVariantConst v : s.params["pictures"].as<JsonArrayConst>()) {
                    if (v.is<const char *>()) out.push_back(v.as<String>());
                }
            }
            return out;
        }

        // The WebServer's own connection: client() hands out a copy, and a
        // timeout set on that wouldn't reach the WebServer's reads.
        struct LiveClient : WebServer {
            static WiFiClient &of(WebServer &s) { return s.*(&LiveClient::_currentClient); }
        };

        // The WebServer reads a raw body HTTP_RAW_BUFLEN bytes at a time and
        // waits for a whole buffer each time, up to the connection's 5 s
        // timeout for each gap. That's cut to 2 s (a client gone quiet that
        // long has most likely gone), and once less than a buffer is left,
        // to 250 ms, since the rest is already on its way; otherwise the
        // last, shorter piece would always sit out the timeout.
        void setReadWait(size_t received) {
            const size_t length = server->clientContentLength();
            const bool last = received < length && length - received < HTTP_RAW_BUFLEN;
            static_cast<Stream &>(LiveClient::of(*server)).setTimeout(last ? 250 : 2000);
        }

        // PUT .../pictures/{hash}: the body arrives here a chunk at a time,
        // straight from the connection to flash.
        void handlePictureBody() {
            HTTPRaw &raw = server->raw();
            switch (raw.status) {
            case RAW_START:
                setReadWait(0);
                pictures::begin(server->pathArg(0), server->clientContentLength(), picturesInUse());
                if ((size_t) server->clientContentLength() > pictures::MAX_BYTES) {
                    // Too big to be worth reading: answer now and hang up
                    // (the server's own connection; stopping a copy leaves
                    // it open). The reads then fail and RAW_ABORTED tidies
                    // up.
                    sendError(413, "the picture is " + String(server->clientContentLength()) + " bytes; the most is " +
                                       String(pictures::MAX_BYTES) + " (1 MB)");
                    LiveClient::of(*server).stop();
                }
                break;
            case RAW_WRITE:
                pictures::write(raw.buf, raw.currentSize);
                setReadWait(raw.totalSize);
                break;
            case RAW_ABORTED:
                pictures::abort();
                break;
            case RAW_END:
                break;
            }
        }

        // ...then, with all of it in, this answers.
        void handlePutPicture() {
            if (!pictures::active()) {
                if (server->client().connected()) sendError(400, "send the picture file itself as the body, not a form");
                return;
            }
            JsonDocument doc;
            String error;
            const int status = pictures::end(doc.to<JsonObject>(), error);
            if (status != 200) {
                sendError(status, error);
                return;
            }
            // Screens that were waiting for it show it now.
            const String hash = doc["hash"];
            Lock lock;
            for (Slot &s : slots) {
                for (JsonVariantConst v : s.params["pictures"].as<JsonArrayConst>()) {
                    if (hash == v.as<const char *>()) {
                        s.dirty = true;
                        s.pictureClean[0] = s.pictureClean[1] = false;
                    }
                }
            }
            sendJson(200, doc);
        }

        void handleGetPictures() {
            JsonDocument doc;
            pictures::list(doc.to<JsonObject>(), picturesInUse());
            sendJson(200, doc);
        }

        // GET .../pictures/{hash}: the file itself, as stored.
        void handleGetPicture() {
            const String hash = server->pathArg(0);
            if (!pictures::has(hash)) {
                sendError(404, "the board doesn't have picture " + hash);
                return;
            }
            File f = LittleFS.open(pictures::path(hash), "r");
            uint8_t sig[4] = {};
            f.read(sig, 4);
            f.seek(0);
            const char *type = sig[0] == 0xFF ? "image/jpeg" : sig[0] == 0x89 ? "image/png" : sig[0] == 'G' ? "image/gif" : "image/webp";
            health::pause(true); // a big file over a slow link outlasts the loop watchdog
            server->streamFile(f, type);
            health::pause(false);
            f.close();
        }

        void handleDeletePicture() {
            String error;
            const String hash = server->pathArg(0);
            const int status = pictures::remove(hash, picturesInUse(), error);
            if (status != 200) {
                sendError(status, error);
                return;
            }
            JsonDocument doc;
            doc["removed"] = hash;
            sendJson(200, doc);
        }

        // Ticker screens only: fetch the price now, whatever the interval.
        void handleRefresh(int screen) {
            Lock lock;
            if (CONTROLS[slots[screen].control] != &tickerControl) {
                sendError(400, "screen " + String(screen) + " isn't a ticker");
                return;
            }
            feeds::refreshQuote(slots[screen].params["symbol"].as<String>());
            JsonDocument doc;
            reportSlot(screen, doc.to<JsonObject>());
            sendJson(200, doc);
        }

        // Made-up readings on every weather screen, until the next fetch.
        void handleWeatherPreview() {
            JsonDocument body;
            if (!readBody(body)) return;
            if (!body.is<JsonObject>()) {
                sendError(400, "body must be a JSON object");
                return;
            }
            float values[3];
            const char *keys[] = {"temperature", "high", "low"};
            JsonDocument doc;
            for (int i = 0; i < 3; i++) {
                JsonVariantConst v = body[keys[i]];
                if (!v.isNull() && !v.is<float>()) {
                    sendError(400, String(keys[i]) + " must be a number");
                    return;
                }
                values[i] = v.isNull() ? NAN : v.as<float>();
                if (!v.isNull()) doc[keys[i]] = values[i];
            }
            feeds::previewWeather(values[0], values[1], values[2]);
            Lock lock;
            for (Slot &s : slots) {
                if (CONTROLS[s.control] == &weatherControl) s.dirty = true;
            }
            sendJson(200, doc);
        }

        void handleCapabilities() {
            JsonDocument doc;
            listControls(doc["controls"].to<JsonArray>());
            JsonArray effects = doc["screensaverEffects"].to<JsonArray>();
            for (int i = 0; ScreensaverControl::EFFECTS[i]; i++) effects.add(ScreensaverControl::EFFECTS[i]);
            JsonArray transitions = doc["transitions"].to<JsonArray>();
            for (int i = 0; transition::CHOICES[i]; i++) transitions.add(transition::CHOICES[i]);
            // Clock face files can be uploaded (PUT .../screens/{n}/clock-face)
            // in this version of the file format; builtIn are the clockFace
            // names.
            JsonObject clockFaces = doc["clockFaces"].to<JsonObject>();
            clockFaces["fileVersion"] = AnalogClockControl::FILE_VERSION;
            JsonArray builtIn = clockFaces["builtIn"].to<JsonArray>();
            for (int i = 0; AnalogClockControl::CLOCK_FACES[i]; i++) builtIn.add(AnalogClockControl::CLOCK_FACES[i]);
            // Pictures can be uploaded (PUT .../pictures/{hash}) for the
            // picture control.
            pictures::capabilities(doc["pictures"].to<JsonObject>());
            doc["api"] = String(API_BASE) + "/screens";
            sendJson(200, doc);
        }
    }

    void begin() {
        layoutLock = xSemaphoreCreateMutex();
        {
            Lock lock;
            loadLayout();
        }
        lockWindow = lockBoot = LockStats(); // restoring at start-up isn't the API
        xTaskCreatePinnedToCore(renderTask, "orbit", 8192, nullptr, 1, &renderHandle, 0);
    }

    LockStats takeLockStats() {
        const LockStats s = lockWindow;
        lockWindow = LockStats();
        return s;
    }

    LockStats lockStatsSinceBoot() { return lockBoot; }

    void registerRoutes(WebServer &s) {
        server = &s;
        const String screensUri = String(API_BASE) + "/screens";
        s.on(screensUri, HTTP_GET, handleGetScreens);
        s.on(screensUri, HTTP_POST, handlePostScreens);
        for (int i = 0; i < NUM_SCREENS; i++) {
            const String uri = screensUri + "/" + i;
            s.on(uri, HTTP_GET, [i] { handleGetScreen(i); });
            s.on(uri, HTTP_POST, [i] { handlePostScreen(i); });
            s.on(uri + "/refresh", HTTP_POST, [i] { handleRefresh(i); });
            s.on(uri + "/clock-face", HTTP_PUT, [i] { handlePutClockFace(i); });
        }
        s.on(String(API_BASE) + "/weather/preview", HTTP_POST, handleWeatherPreview);
        s.on(String(API_BASE) + "/capabilities", HTTP_GET, handleCapabilities);
        const String picturesUri = String(API_BASE) + "/pictures";
        s.on(picturesUri, HTTP_GET, handleGetPictures);
        s.on(UriBraces(picturesUri + "/{}"), HTTP_PUT, handlePutPicture, handlePictureBody);
        s.on(UriBraces(picturesUri + "/{}"), HTTP_GET, handleGetPicture);
        s.on(UriBraces(picturesUri + "/{}"), HTTP_DELETE, handleDeletePicture);
    }

    uint32_t takeDrawUs(int screen) {
        const uint32_t avg = drawCount[screen] ? drawUs[screen] / drawCount[screen] : 0;
        drawUs[screen] = drawCount[screen] = 0;
        return avg;
    }

    void stallForTest() { stallRequested = true; }

    bool mountFiles() {
        if (!filesReady) {
            // Formats the data partition on first use.
            filesReady = LittleFS.begin(true);
            if (!filesReady) Serial.println("OrbIt: couldn't mount LittleFS; drawings, clock faces and pictures won't be saved");
        }
        return filesReady;
    }

    void listControls(JsonArray out) {
        for (Control *c : CONTROLS) out.add(c->name());
    }

}
