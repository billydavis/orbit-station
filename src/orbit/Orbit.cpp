#include "Orbit.h"
#include "Control.h"
#include "controls/AnalogClockControl.h"
#include "controls/BlankControl.h"
#include "controls/GaugeControl.h"
#include "controls/TimeControl.h"
#include "display/Display.h"
#include "net/Network.h"
#include "ui/StatusScreen.h"
#include <Preferences.h>

namespace orbit {

    namespace {
        constexpr int NUM_SCREENS = display::NUM_SCREENS;
        const char *NOT_SAVED_WARNING = "applied, but it could not be saved - it will not survive a reboot";

        // ---- Controls ----
        BlankControl blankControl;
        TimeControl timeControl;
        AnalogClockControl analogClockControl;
        GaugeControl gaugeControl;
        Control *const CONTROLS[] = {&blankControl, &timeControl, &analogClockControl, &gaugeControl};
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
        };
        Slot slots[NUM_SCREENS];
        SemaphoreHandle_t layoutLock; // the API (core 1) writes while the renderer (core 0) reads
        String savedLayout;           // what's in NVS now, to skip writing when nothing changed

        struct Lock {
            Lock() { xSemaphoreTake(layoutLock, portMAX_DELAY); }
            ~Lock() { xSemaphoreGive(layoutLock); }
        };

        // Checks one {control, params} entry. Returns 0, or 400/501 with error.
        int parseConfig(JsonObjectConst entry, int &control, JsonDocument &params, String &error) {
            if (!entry["control"].is<const char *>()) {
                error = "missing or invalid 'control'";
                return 400;
            }
            const String name = entry["control"].as<String>();
            control = findControl(name);
            if (control < 0) {
                if (isV1Control(name)) {
                    error = "control '" + name + "' isn't built into this firmware - see GET " + API_BASE + "/capabilities";
                    return 501;
                }
                error = "unknown control '" + name + "'";
                return 400;
            }
            JsonVariantConst p = entry["params"];
            if (!p.isNull() && !p.is<JsonObjectConst>()) {
                error = "'params' must be an object";
                return 400;
            }
            params.clear();
            JsonObject out = params.to<JsonObject>();
            return CONTROLS[control]->parse(p.as<JsonObjectConst>(), out, error);
        }

        // Call with the layout locked.
        void applyConfig(int screen, int control, const JsonDocument &params, bool fromApi) {
            Slot &s = slots[screen];
            s.control = control;
            s.params = params;
            if (fromApi) s.updatedAt = millis();
            s.dirty = true;
        }

        void reportSlot(int screen, JsonObject out) {
            const Slot &s = slots[screen];
            out["screen"] = screen;
            out["control"] = CONTROLS[s.control]->name();
            // Always an object, {} for a control without params, never null.
            out["params"].to<JsonObject>().set(s.params.as<JsonObjectConst>());
            out["updatedAt"] = s.updatedAt;
        }

        // ---- Saving ----
        // The layout as saved: GET's shape without updatedAt or live readings.
        // Call with the layout locked.
        String layoutForSaving() {
            JsonDocument doc;
            JsonArray screens = doc["screens"].to<JsonArray>();
            for (int i = 0; i < NUM_SCREENS; i++) {
                JsonObject e = screens.add<JsonObject>();
                e["control"] = CONTROLS[slots[i].control]->name();
                JsonObject p = e["params"].to<JsonObject>();
                p.set(slots[i].params.as<JsonObjectConst>());
                CONTROLS[slots[i].control]->stripLive(p);
            }
            String out;
            serializeJson(doc, out);
            return out;
        }

        // Writes the layout if its saved form changed. Call with the layout
        // locked. Returns false if it couldn't be written.
        bool persistLayout() {
            const String layout = layoutForSaving();
            if (layout == savedLayout) return true;
            Preferences prefs;
            if (!prefs.begin("orbit", false)) return false;
            const bool ok = prefs.putString("layout", layout) == layout.length();
            prefs.end();
            if (ok) {
                savedLayout = layout;
                Serial.println("OrbIt: saved layout to NVS");
            } else {
                Serial.println("OrbIt: couldn't save the layout");
            }
            return ok;
        }

        void setDefaultLayout() {
            // A fresh board: the time with date and weekday on the middle screen.
            JsonDocument params;
            params["showDate"] = true;
            params["showDay"] = true;
            params["format24Hour"] = false;
            applyConfig(2, findControl("time"), params, false);
        }

        void loadLayout() {
            Preferences prefs;
            prefs.begin("orbit", true);
            const String saved = prefs.getString("layout", "");
            prefs.end();
            if (saved.isEmpty()) {
                setDefaultLayout();
                return;
            }
            JsonDocument doc;
            if (deserializeJson(doc, saved) || !doc["screens"].is<JsonArray>()) {
                Serial.println("OrbIt: saved layout unreadable, using the default");
                setDefaultLayout();
                return;
            }
            int i = 0;
            for (JsonObjectConst entry : doc["screens"].as<JsonArrayConst>()) {
                if (i >= NUM_SCREENS) break;
                int control;
                JsonDocument params;
                String error;
                if (parseConfig(entry, control, params, error) == 0) {
                    applyConfig(i, control, params, false);
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

        void renderTask(void *) {
            bool statusWasShowing = false;
            for (;;) {
                const uint32_t now = millis();
                const bool status = statusShowing();
                if (statusWasShowing && !status) slots[0].dirty = true; // back to screen 1's own content
                statusWasShowing = status;

                for (int i = 0; i < NUM_SCREENS; i++) {
                    Slot &s = slots[i];
                    bool due;
                    if (i == 0 && status) {
                        due = now - s.lastDraw >= STATUS_REFRESH_MS;
                    } else {
                        Lock lock;
                        const uint32_t every = CONTROLS[s.control]->refreshMs(s.params.as<JsonObjectConst>());
                        due = s.dirty || (every && now - s.lastDraw >= every);
                    }
                    if (!due) continue;

                    LGFX_Sprite &spr = display::beginFrame(i);
                    {
                        Lock lock;
                        if (i == 0 && status) {
                            ui::drawStatus(spr);
                        } else {
                            DrawContext ctx = {i, now};
                            CONTROLS[s.control]->draw(spr, s.params.as<JsonObjectConst>(), ctx);
                            s.dirty = false;
                        }
                        s.lastDraw = now;
                    }
                    display::endFrame(i);
                }
                vTaskDelay(pdMS_TO_TICKS(20));
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
            int control;
            JsonDocument params;
            String error;
            if (int status = parseConfig(body.as<JsonObjectConst>(), control, params, error)) {
                sendError(status, error);
                return;
            }
            JsonDocument doc;
            Lock lock;
            applyConfig(screen, control, params, true);
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
            int screens[NUM_SCREENS], controls[NUM_SCREENS];
            JsonDocument params[NUM_SCREENS];
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
                if (int status = parseConfig(entry, controls[count], params[count], error)) {
                    sendError(status, "screen " + String(screen) + ": " + error);
                    return;
                }
                screens[count++] = screen;
            }
            JsonDocument doc;
            Lock lock;
            for (int i = 0; i < count; i++) applyConfig(screens[i], controls[i], params[i], true);
            if (!persistLayout()) doc["warning"] = NOT_SAVED_WARNING;
            JsonArray out = doc["screens"].to<JsonArray>();
            for (int i = 0; i < NUM_SCREENS; i++) reportSlot(i, out.add<JsonObject>());
            sendJson(200, doc);
        }

        void handleCapabilities() {
            JsonDocument doc;
            listControls(doc["controls"].to<JsonArray>());
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
        xTaskCreatePinnedToCore(renderTask, "orbit", 8192, nullptr, 1, nullptr, 0);
    }

    void registerRoutes(WebServer &s) {
        server = &s;
        const String screensUri = String(API_BASE) + "/screens";
        s.on(screensUri, HTTP_GET, handleGetScreens);
        s.on(screensUri, HTTP_POST, handlePostScreens);
        for (int i = 0; i < NUM_SCREENS; i++) {
            const String uri = screensUri + "/" + i;
            s.on(uri, HTTP_GET, [i] { handleGetScreen(i); });
            s.on(uri, HTTP_POST, [i] { handlePostScreen(i); });
        }
        s.on(String(API_BASE) + "/capabilities", HTTP_GET, handleCapabilities);
    }

    void listControls(JsonArray out) {
        for (Control *c : CONTROLS) out.add(c->name());
    }

}
