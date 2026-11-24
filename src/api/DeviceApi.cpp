#include "DeviceApi.h"
#include "core/BuildStamp.h"
#include "core/Health.h"
#include "core/Settings.h"
#include "display/Display.h"
#include "makapix/Makapix.h"
#include "net/Network.h"
#include "orbit/Orbit.h"
#include <ArduinoJson.h>
#include <WiFi.h>

namespace api {

    namespace {
        WebServer *server;
        const char *const BUILT = BUILD_STAMP;

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

        void handleSystem() {
            const net::Status n = net::status();
            JsonDocument doc;
            doc["hostname"] = String(n.hostname) + ".local";
            doc["ip"] = WiFi.localIP().toString();
            doc["mac"] = WiFi.macAddress();
            doc["ssid"] = WiFi.SSID();
            doc["rssi"] = WiFi.RSSI();
            doc["uptimeSeconds"] = millis() / 1000;
            doc["freeHeap"] = ESP.getFreeHeap();
            doc["minFreeHeap"] = ESP.getMinFreeHeap();
            doc["largestFreeBlock"] = ESP.getMaxAllocHeap(); // HTTPS needs about 17 KB in one piece
            doc["freePsram"] = ESP.getFreePsram();
            doc["minFreePsram"] = ESP.getMinFreePsram();
            doc["largestFreePsramBlock"] = ESP.getMaxAllocPsram();
            doc["firmware"] = "Orbit Station " ORBIT_STATION_VERSION;
            doc["firmwareBuilt"] = BUILT;
            doc["firmwareCommit"] = BUILD_COMMIT;
            doc["firmwareElfSha"] = elfSha();
            // Longest wait for and hold of the screen layout lock since boot.
            const orbit::LockStats l = orbit::lockStatsSinceBoot();
            JsonObject lock = doc["layoutLockMs"].to<JsonObject>();
            lock["renderWaited"] = l.renderWaitUs / 1000.0f;
            lock["renderHeld"] = l.renderHoldUs / 1000.0f;
            lock["apiWaited"] = l.apiWaitUs / 1000.0f;
            lock["apiHeld"] = l.apiHoldUs / 1000.0f;
            doc["widget"] = "OrbIt"; // the only one; kept for info-orbs clients
            health::toJson(doc.as<JsonObject>());
            sendJson(200, doc);
        }

        void handleCapabilities() {
            JsonDocument doc;
            doc["firmwareBuilt"] = BUILT;
            doc["screens"] = display::NUM_SCREENS;
            JsonObject orbit = doc["widgets"].to<JsonArray>().add<JsonObject>();
            orbit["name"] = "OrbIt";
            orbit["showing"] = true;
            orbit["api"] = String(orbit::API_BASE) + "/screens";
            orbit::listControls(orbit["controls"].to<JsonArray>());
            sendJson(200, doc);
        }

        void handleGetSettings() {
            JsonDocument doc;
            settings::toJson(doc.to<JsonObject>());
            sendJson(200, doc);
        }

        void handlePostSettings() {
            JsonDocument body;
            if (!server->hasArg("plain") || deserializeJson(body, server->arg("plain")) || !body.is<JsonObject>()) {
                sendError(400, "body must be a JSON object");
                return;
            }
            String error;
            bool saved;
            if (int status = settings::update(body.as<JsonObjectConst>(), error, saved)) {
                sendError(status, error);
                return;
            }
            JsonDocument doc;
            settings::toJson(doc.to<JsonObject>());
            if (!saved) doc["warning"] = "applied, but it could not be saved - it will not survive a reboot";
            sendJson(200, doc);
        }
    }

    void registerRoutes(WebServer &s) {
        server = &s;
        s.on("/api/v1/system", HTTP_GET, handleSystem);
        s.on("/api/v1/capabilities", HTTP_GET, handleCapabilities);
        s.on("/api/v1/settings", HTTP_GET, handleGetSettings);
        s.on("/api/v1/settings", HTTP_POST, handlePostSettings);
        // Makapix pairing: state and code; forget erases it (pairs again on next use).
        // Recent events, oldest first: restarts and why, failures, warnings.
        s.on("/api/v1/log", HTTP_GET, [] {
            JsonDocument doc;
            health::logJson(doc["events"].to<JsonArray>());
            sendJson(200, doc);
        });
        s.on("/api/v1/makapix", HTTP_GET, [] {
            JsonDocument doc;
            makapix::statusJson(doc.to<JsonObject>());
            sendJson(200, doc);
        });
        s.on("/api/v1/makapix/forget", HTTP_POST, [] {
            makapix::forget();
            JsonDocument doc;
            doc["forgotten"] = true;
            sendJson(200, doc);
        });
        s.onNotFound([] { sendError(404, "not found: " + server->uri()); });
    }

}
