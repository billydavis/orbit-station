#include "DeviceApi.h"
#include "core/Settings.h"
#include "display/Display.h"
#include "net/Network.h"
#include "orbit/Orbit.h"
#include <ArduinoJson.h>
#include <WiFi.h>

namespace api {

    namespace {
        WebServer *server;
        const char *BUILT = __DATE__ " " __TIME__;

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
            doc["freePsram"] = ESP.getFreePsram();
            doc["firmware"] = "Orbit Station " ORBIT_STATION_VERSION;
            doc["firmwareBuilt"] = BUILT;
            doc["widget"] = "OrbIt"; // the only one; kept for info-orbs clients
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
        s.onNotFound([] { sendError(404, "not found: " + server->uri()); });
    }

}
