#include "Network.h"
#include "core/BuildStamp.h"
#include "core/Health.h"
#include "core/TimeKeeper.h"
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <algorithm>
#include <atomic>
#include <esp_wifi.h>
#include <vector>

namespace net {

    namespace {
        // How long to keep trying the saved network before opening the hotspot,
        // and how long the hotspot stays open before trying the network again.
        // A router restart takes a couple of minutes, so don't give up sooner.
        constexpr uint32_t CONNECT_TIMEOUT_MS = 3 * 60 * 1000;
        constexpr uint32_t SETUP_WINDOW_S = 5 * 60;
        const char *SETUP_ADDRESS = "192.168.4.1";

        WiFiManager wm;
        WebServer web(80);
        bool webRunning = false;
        bool otaStarted = false;

        Status current = {};
        portMUX_TYPE statusLock = portMUX_INITIALIZER_UNLOCKED;
        uint32_t stateSince = 0;

        void setState(State s) {
            portENTER_CRITICAL(&statusLock);
            current.state = s;
            portEXIT_CRITICAL(&statusLock);
            stateSince = millis();
        }

        void setText(char *field, size_t size, const String &value) {
            portENTER_CRITICAL(&statusLock);
            strlcpy(field, value.c_str(), size);
            portEXIT_CRITICAL(&statusLock);
        }

        void handleRoot() {
            Status s = status();
            String html;
            html.reserve(600);
            html += F("<!doctype html><html><head><meta charset=utf-8>"
                      "<meta name=viewport content='width=device-width,initial-scale=1'>"
                      "<title>Orbit Station</title></head><body style='font-family:sans-serif'>"
                      "<h1>Orbit Station</h1><ul>");
            html += String("<li>Firmware " ORBIT_STATION_VERSION " (built ") + BUILD_STAMP + ", " + BUILD_COMMIT + ")</li>";
            html += "<li>Address " + String(s.ip) + " (" + s.hostname + ".local)</li>";
            html += "<li>WiFi " + String(s.ssid) + ", signal " + String(WiFi.RSSI()) + " dBm</li>";
            html += "<li>Up " + String(millis() / 1000) + " s</li>";
            html += "<li>Free memory " + String(ESP.getFreeHeap() / 1024) + " KB, PSRAM " + String(ESP.getFreePsram() / 1024) + " KB</li>";
            html += F("</ul><p>API: <a href='/orbit/api/v1/screens'>/orbit/api/v1/screens</a>, "
                      "<a href='/api/v1/system'>/api/v1/system</a>, <a href='/api/v1/settings'>/api/v1/settings</a></p>"
                      "</body></html>");
            web.send(200, "text/html", html);
        }

        void startServices() {
            if (!otaStarted) {
                ArduinoOTA.setHostname(current.hostname);
                ArduinoOTA.onStart([] {
                    health::pause(true); // the update blocks the loop until it's done
                    health::event("Update: receiving new firmware");
                    setState(State::Updating);
                });
                ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
                    portENTER_CRITICAL(&statusLock);
                    current.updatePercent = total ? done * 100 / total : 0;
                    portEXIT_CRITICAL(&statusLock);
                });
                ArduinoOTA.onEnd([] { Serial.println("Update: done, restarting"); });
                ArduinoOTA.onError([](ota_error_t e) {
                    health::pause(false);
                    health::event("Update failed (error %u)", e);
                    setState(State::Connected);
                });
                ArduinoOTA.begin(); // also starts mDNS with our hostname
                MDNS.addService("http", "tcp", 80);
                MDNS.addServiceTxt("http", "tcp", "path", "/");
                // How OrbIt clients (Command Center) recognise an OrbIt board.
                MDNS.addServiceTxt("http", "tcp", "orbit", "/orbit/api/v1/screens");
                otaStarted = true;
                Serial.printf("Network: reachable at http://%s.local/\n", current.hostname);
            }
            if (!webRunning) {
                web.begin();
                webRunning = true;
            }
        }

        void onConnected() {
            setText(current.ssid, sizeof(current.ssid), WiFi.SSID());
            setText(current.ip, sizeof(current.ip), WiFi.localIP().toString());
            setState(State::Connected);
            health::event("Network: connected to %s as %s", current.ssid, current.ip);
            timekeeper::startSync();
            startServices();
        }

        // Transmit power is left at the chip's default unless set with 't' on
        // serial (a diagnostic). 0 = default. In units
        // of 0.25 dBm; 't <dBm>' on serial changes it.
        int txQuarterDbm = 0;
        void lowPower() {
            if (txQuarterDbm) WiFi.setTxPower((wifi_power_t) txQuarterDbm);
        }

        bool haveSaved() { return wm.getWiFiSSID(true).length() > 0; }

        // ---- Trying each access point of the saved network ----
        // In a mesh (eero and the like) every node broadcasts the same network
        // name, and a node can refuse a device that another node accepts: one
        // eero turned the ESP32-S3 away every time while the next one let it in.
        // So after a few refusals, try the next access point, strongest first.
        // The one that works is saved with the network (a settings write, only
        // when it changes), so later starts go straight to it.
        constexpr uint8_t REFUSALS_BEFORE_SWITCHING = 4;
        std::atomic<uint8_t> refusals{0};

        struct AccessPoint {
            uint8_t bssid[6];
            int32_t channel;
            int32_t rssi;
        };
        std::vector<AccessPoint> accessPoints;
        size_t nextAccessPoint = 0;

        bool isRefusal(uint8_t reason) {
            return reason == 2 || reason == 15 || reason == 201 || reason == 202 || reason == 203 || reason == 204;
        }

        // The scan for other access points runs in the background: a full scan
        // takes several seconds, longer than the loop's 5 s watchdog allows.
        bool scanning = false;
        uint32_t scanStarted = 0;
        constexpr uint32_t SCAN_TIMEOUT_MS = 20000;

        void startAccessPointScan() {
            accessPoints.clear();
            nextAccessPoint = 0;
            // A scan fails while a connection attempt is running.
            WiFi.setAutoReconnect(false);
            WiFi.disconnect(false);
            delay(100);
            scanning = WiFi.scanNetworks(true) == WIFI_SCAN_RUNNING;
            scanStarted = millis();
            if (!scanning) {
                // Couldn't start one; keep trying the saved network instead.
                Serial.println("Network: couldn't start a scan, retrying the saved network");
                WiFi.setAutoReconnect(true);
                WiFi.begin();
            }
        }

        void connectNextAccessPoint() {
            WiFi.setAutoReconnect(true);
            if (accessPoints.empty() || nextAccessPoint >= accessPoints.size()) {
                WiFi.begin(); // nothing (more) in range; keep trying the saved network
                return;
            }
            const String ssid = wm.getWiFiSSID(true);
            const String pass = wm.getWiFiPass(true);
            const AccessPoint &ap = accessPoints[nextAccessPoint++];
            Serial.printf("Network: trying access point %02X:%02X:%02X:%02X:%02X:%02X (channel %d, %d dBm)\n", ap.bssid[0],
                          ap.bssid[1], ap.bssid[2], ap.bssid[3], ap.bssid[4], ap.bssid[5], (int) ap.channel, (int) ap.rssi);
            WiFi.begin(ssid.c_str(), pass.c_str(), ap.channel, ap.bssid);
        }

        // Called each time round the loop while a scan runs; never waits.
        void checkAccessPointScan() {
            const int n = WiFi.scanComplete();
            if (n == WIFI_SCAN_RUNNING && millis() - scanStarted < SCAN_TIMEOUT_MS) return;
            scanning = false;
            const String ssid = wm.getWiFiSSID(true);
            for (int i = 0; i < n; i++) {
                if (WiFi.SSID(i) != ssid) continue;
                AccessPoint ap;
                memcpy(ap.bssid, WiFi.BSSID(i), 6);
                ap.channel = WiFi.channel(i);
                ap.rssi = WiFi.RSSI(i);
                accessPoints.push_back(ap);
            }
            WiFi.scanDelete();
            WiFi.setAutoReconnect(true);
            std::sort(accessPoints.begin(), accessPoints.end(),
                      [](const AccessPoint &a, const AccessPoint &b) { return a.rssi > b.rssi; });
            Serial.printf("Network: %u access point(s) found for %s\n", (unsigned) accessPoints.size(), ssid.c_str());
            refusals = 0;
            connectNextAccessPoint();
        }

        // Auto-reconnect sometimes goes quiet (no attempts, no refusals), e.g. after
        // the router reboots. If nothing has happened for this long, try again ourselves.
        constexpr uint32_t RETRY_MS = 30000;
        uint32_t lastAttempt = 0;

        void tryNextAccessPoint() {
            refusals = 0;
            lastAttempt = millis();
            if (nextAccessPoint >= accessPoints.size()) {
                startAccessPointScan(); // connects when the scan finishes
            } else {
                connectNextAccessPoint();
            }
        }

        void connectSaved() {
            WiFi.mode(WIFI_STA);
            lowPower();
            refusals = 0;
            if (scanning) {
                WiFi.scanDelete();
                scanning = false;
            }
            accessPoints.clear(); // look again: access points may have changed
            nextAccessPoint = 0;
            WiFi.begin(); // the network saved by the setup page
            lastAttempt = millis();
            setText(current.ssid, sizeof(current.ssid), wm.getWiFiSSID(true));
            setText(current.ip, sizeof(current.ip), "");
            setState(State::Connecting);
            Serial.printf("Network: connecting to %s\n", current.ssid);
        }

        void openSetup(bool haveSaved) {
            // The setup page uses port 80 too.
            if (webRunning) {
                web.stop();
                webRunning = false;
            }
            // With a saved network, close the hotspot after a while and try that
            // network again. Without one there's nothing else to do, so stay open.
            wm.setConfigPortalTimeout(haveSaved ? SETUP_WINDOW_S : 0);
            wm.startConfigPortal(current.apName);
            lowPower(); // starting the hotspot resets any power set with 't'
            setText(current.ip, sizeof(current.ip), SETUP_ADDRESS);
            setState(State::Setup);
            Serial.printf("Network: setup hotspot \"%s\" open, page at http://%s/\n", current.apName, SETUP_ADDRESS);
        }
    }

    void begin() {
        uint8_t mac[6];
        WiFi.macAddress(mac);
        char name[32];
        snprintf(name, sizeof(name), "orbit-station-%02x", mac[5]);
        setText(current.hostname, sizeof(current.hostname), name);
        snprintf(name, sizeof(name), "OrbitStation-%02X", mac[5]);
        setText(current.apName, sizeof(current.apName), name);

        WiFi.persistent(true);
        WiFi.setAutoReconnect(true);
        WiFi.setHostname(current.hostname);

        wm.setConfigPortalBlocking(false);
        wm.setTitle("Orbit Station");
        wm.setHostname(current.hostname);
        wm.setShowInfoUpdate(false); // firmware updates go over WiFi, not through this page
        std::vector<const char *> menu = {"wifi", "exit"};
        wm.setMenu(menu);
        // After details are saved, close the setup page even if the first join
        // failed, so the access point fallback below gets a go.
        wm.setBreakAfterConfig(true);

        web.on("/", HTTP_GET, handleRoot);

        // Log why the router turned us away; the setup page only says "failed".
        WiFi.onEvent(
            [](arduino_event_id_t, arduino_event_info_t info) {
                const uint8_t r = info.wifi_sta_disconnected.reason;
                const char *why = r == 2 || r == 15 || r == 202 ? "authentication failed (password, or security type)"
                                  : r == 201                    ? "network not found"
                                  : r == 203                    ? "association failed"
                                  : r == 204                    ? "handshake timed out (password, or weak signal)"
                                  : r == 210                    ? "no network with a supported security type (WPA3-only?)"
                                                                : "";
                Serial.printf("Network: disconnected, reason %u %s\n", r, why);
                if (isRefusal(r)) refusals++;
            },
            ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

        // Check the saved network's name rather than wm.getWiFiIsSaved(), which
        // said yes right after the saved WiFi was erased.
        WiFi.mode(WIFI_STA);
        lowPower();
        if (haveSaved()) {
            connectSaved();
        } else {
            openSetup(false);
        }
    }

    void loop() {
        switch (status().state) {
        case State::Connecting:
            if (scanning) {
                checkAccessPointScan();
            } else if (WiFi.status() == WL_CONNECTED) {
                onConnected();
            } else if (refusals >= REFUSALS_BEFORE_SWITCHING) {
                tryNextAccessPoint();
            } else if (millis() - lastAttempt >= RETRY_MS) {
                Serial.println("Network: still not connected, trying again");
                tryNextAccessPoint();
            } else if (millis() - stateSince >= CONNECT_TIMEOUT_MS) {
                health::event("Network: can't reach the saved network, opening setup");
                openSetup(true);
            }
            break;

        case State::Setup:
            wm.process();
            if (WiFi.status() == WL_CONNECTED) {
                // New details entered on the setup page, or the saved network came back.
                if (wm.getConfigPortalActive()) wm.stopConfigPortal();
                WiFi.mode(WIFI_STA);
                onConnected();
            } else if (!wm.getConfigPortalActive()) {
                // Details were saved but the first join failed, or the hotspot
                // closed after its time window: try the saved network.
                connectSaved();
            }
            break;

        case State::Connected:
            if (WiFi.status() != WL_CONNECTED) {
                health::event("Network: lost WiFi, reconnecting");
                setState(State::Connecting); // auto-reconnect keeps trying meanwhile
                lastAttempt = millis();
                break;
            }
            ArduinoOTA.handle();
            web.handleClient();
            break;

        case State::Updating:
            ArduinoOTA.handle();
            break;
        }
    }

    Status status() {
        portENTER_CRITICAL(&statusLock);
        Status s = current;
        portEXIT_CRITICAL(&statusLock);
        return s;
    }

    WebServer &server() { return web; }

    void scan() {
        Serial.println("Network: scanning...");
        health::pause(true); // a full scan outlasts the loop watchdog
        // A scan fails while the board keeps retrying a connection, so pause that.
        const bool retrying = status().state == State::Connecting;
        if (retrying) {
            WiFi.setAutoReconnect(false);
            WiFi.disconnect(false);
            delay(300);
        }
        const int n = WiFi.scanNetworks();
        const char *auth[] = {"open", "WEP", "WPA", "WPA2", "WPA/WPA2", "WPA2-Enterprise", "WPA3", "WPA2/WPA3", "WAPI"};
        for (int i = 0; i < n; i++) {
            const int a = WiFi.encryptionType(i);
            Serial.printf("  %-32s %s %4d dBm  channel %2d  %s\n", WiFi.SSID(i).c_str(), WiFi.BSSIDstr(i).c_str(), WiFi.RSSI(i),
                          WiFi.channel(i), a < (int) (sizeof(auth) / sizeof(auth[0])) ? auth[a] : "?");
        }
        Serial.printf("Network: %d found\n", n);
        WiFi.scanDelete();
        if (retrying) {
            WiFi.setAutoReconnect(true);
            WiFi.begin();
        }
        health::pause(false);
    }

    void testFallback() {
        Serial.println("Network test: dropping WiFi and acting as if refused, so the access point scan runs");
        WiFi.disconnect(false);
        refusals = REFUSALS_BEFORE_SWITCHING;
        accessPoints.clear();
        nextAccessPoint = 0;
        setState(State::Connecting);
    }

    void testQuietRadio() {
        Serial.println("Network test: dropping WiFi with auto-reconnect off; expect a retry within 30 s");
        WiFi.setAutoReconnect(false);
        WiFi.disconnect(false);
    }

    void setTxPower(float dBm) {
        txQuarterDbm = (int) (dBm * 4);
        lowPower();
        int8_t now = 0;
        esp_wifi_get_max_tx_power(&now);
        Serial.printf("Network: transmit power %.2f dBm\n", now / 4.0f);
        if (status().state == State::Connecting) {
            WiFi.disconnect(false);
            WiFi.begin();
        }
    }

    void forgetWifi() {
        Serial.println("Network: forgetting saved WiFi and restarting");
        wm.resetSettings();
        delay(200);
        ESP.restart();
    }

}
