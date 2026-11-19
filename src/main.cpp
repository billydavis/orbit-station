// Orbit Station: OrbIt firmware for five GC9A01 screens on an ESP32-S3.
// Phase 2: display core with a test animation, WiFi setup and recovery.
// Screen 1 shows the network status; screens 2-5 show the test animation.
//
// Serial commands (115200, end with Enter):
//   r        re-initialise all screens (shared RST wire)
//   r <1-5>  re-initialise one screen through its CS line; the others keep going
//   i <1-5>  toggle colour inversion on one screen
//   p        pause/resume the animation (paused: screens 2-5 not drawn at all)
//   n        network status
//   w        forget the saved WiFi and restart into setup
//   l        list the WiFi networks the board can see
//   s        status
//   ?        help

#include "demo/Demo.h"
#include "display/Display.h"
#include "net/Network.h"
#include "net/Recovery.h"
#include "pins.h"
#include <Arduino.h>
#include <esp_chip_info.h>
#include <esp_mac.h>
#include <esp_system.h>

namespace {
    const char *resetReasonName(esp_reset_reason_t r) {
        switch (r) {
        case ESP_RST_POWERON:
            return "power on";
        case ESP_RST_EXT:
            return "external pin";
        case ESP_RST_SW:
            return "software restart";
        case ESP_RST_PANIC:
            return "crash";
        case ESP_RST_INT_WDT:
            return "interrupt watchdog";
        case ESP_RST_TASK_WDT:
            return "task watchdog";
        case ESP_RST_WDT:
            return "other watchdog";
        case ESP_RST_DEEPSLEEP:
            return "deep sleep wake";
        case ESP_RST_BROWNOUT:
            return "brownout";
        default:
            return "unknown";
        }
    }

    void logBoot() {
        esp_chip_info_t chip;
        esp_chip_info(&chip);
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);

        Serial.printf("\nOrbit Station %s (built %s %s)\n", ORBIT_STATION_VERSION, __DATE__, __TIME__);
        Serial.printf("Chip: %s rev %d, %d cores, %lu MHz\n", ESP.getChipModel(), chip.revision, chip.cores, (unsigned long) ESP.getCpuFreqMHz());
        Serial.printf("Flash: %lu MB\n", (unsigned long) (ESP.getFlashChipSize() / (1024 * 1024)));
        Serial.printf("PSRAM: %lu KB total, %lu KB free\n", (unsigned long) (ESP.getPsramSize() / 1024), (unsigned long) (ESP.getFreePsram() / 1024));
        Serial.printf("Heap: %lu KB free\n", (unsigned long) (ESP.getFreeHeap() / 1024));
        Serial.printf("MAC: %02X:%02X:%02X:%02X:%02X:%02X\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        Serial.printf("Reset reason: %s (%d)\n", resetReasonName(esp_reset_reason()), (int) esp_reset_reason());
    }

    void printStatus() {
        Serial.printf("SPI %lu MHz, animation %s\n", (unsigned long) (display::SPI_HZ / 1000000), demo::paused() ? "paused" : "running");
        Serial.print("Inverted:");
        for (int s = 0; s < display::NUM_SCREENS; s++) Serial.printf(" %d=%s", s + 1, display::inverted(s) ? "on" : "off");
        Serial.println();
        Serial.printf("Heap %lu KB free, PSRAM %lu KB free\n", (unsigned long) (ESP.getFreeHeap() / 1024), (unsigned long) (ESP.getFreePsram() / 1024));
    }

    void printHelp() {
        Serial.println("r        re-initialise all screens");
        Serial.println("r <1-5>  re-initialise one screen (others keep going)");
        Serial.println("i <1-5>  toggle colour inversion on one screen");
        Serial.println("p        pause/resume the animation");
        Serial.println("n        network status");
        Serial.println("w        forget the saved WiFi and restart into setup");
        Serial.println("l        list the WiFi networks the board can see");
        Serial.println("s        status");
    }

    void handleSerial() {
        if (!Serial.available()) return;
        String line = Serial.readStringUntil('\n');
        line.trim();
        if (line.isEmpty()) return;
        const char cmd = line[0];
        const String rest = line.substring(1);
        const int screen = rest.length() ? rest.toInt() - 1 : -1;
        const bool validScreen = screen >= 0 && screen < display::NUM_SCREENS;

        switch (cmd) {
        case 'r':
            if (rest.length() && !validScreen) {
                Serial.println("Use r, or r 1 to r 5");
                break;
            }
            display::requestReinit(screen);
            break;
        case 'i':
            if (!validScreen) {
                Serial.println("Use i 1 to i 5");
                break;
            }
            display::setInverted(screen, !display::inverted(screen));
            Serial.printf("Screen %d inversion %s\n", screen + 1, display::inverted(screen) ? "on" : "off");
            break;
        case 'p':
            demo::setPaused(!demo::paused());
            Serial.printf("Animation %s\n", demo::paused() ? "paused" : "running");
            break;
        case 's':
            printStatus();
            break;
        case 'n': {
            const net::Status n = net::status();
            const char *names[] = {"connecting", "setup hotspot open", "connected", "updating"};
            Serial.printf("Network: %s, WiFi \"%s\", address %s, %s.local, hotspot %s\n", names[(int) n.state], n.ssid, n.ip,
                          n.hostname, n.apName);
            break;
        }
        case 'w':
            net::forgetWifi();
            break;
        case 'l':
            net::scan();
            break;
        case 't':
            net::setTxPower(rest.toFloat());
            break;
        default:
            printHelp();
        }
    }

    // Every 5 s: frames per second each screen got, how much of each frame
    // changed and was sent, and how busy the sender was.
    void report() {
        static uint32_t last = millis();
        const uint32_t elapsed = millis() - last;
        if (elapsed < 5000) return;
        last = millis();

        uint32_t frames = 0, tiles = 0, whole = 0, busyUs = 0;
        Serial.print("fps");
        for (int s = 0; s < display::NUM_SCREENS; s++) {
            display::Stats st = display::takeStats(s);
            Serial.printf(" %.1f", st.framesSent * 1000.0f / elapsed);
            frames += st.framesSent;
            tiles += st.tilesSent;
            whole += st.wholeFrames;
            busyUs += st.busyUs;
        }
        const float sentPct = frames ? 100.0f * tiles / frames / display::TILES_PER_FRAME : 0;
        Serial.printf(" | %.0f%% of each frame sent, %lu whole | sender busy %.0f%%\n", sentPct, (unsigned long) whole,
                      busyUs / 10.0f / elapsed);
    }
}

void setup() {
    pinMode(pins::RST, OUTPUT);
    digitalWrite(pins::RST, HIGH);
    Serial.begin(115200);
    // Give the USB serial port a moment to open so the boot log isn't lost.
    for (uint32_t start = millis(); !Serial && millis() - start < 3000;) delay(10);
    logBoot();
    recovery::begin(); // before anything reads saved settings

    if (!display::begin()) {
        Serial.println("Display core failed to start");
        return;
    }
    net::begin();
    demo::begin();
    printStatus();
    printHelp();
}

void loop() {
    net::loop();
    recovery::loop();
    handleSerial();
    report();
    delay(10);
}
