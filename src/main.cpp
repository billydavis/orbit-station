// Orbit Station: OrbIt firmware for five GC9A01 screens on an ESP32-S3.
// Phase 0: boot, park the screen lines, and log the board's state.

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

    // Keep every screen deselected and out of reset until the display core
    // takes over in phase 1.
    void parkScreenLines() {
        for (int cs : pins::CS) {
            pinMode(cs, OUTPUT);
            digitalWrite(cs, HIGH);
        }
        pinMode(pins::RST, OUTPUT);
        digitalWrite(pins::RST, HIGH);
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
        if (ESP.getPsramSize() == 0) {
            Serial.println("WARNING: no PSRAM found; the display core needs it");
        }
    }
}

void setup() {
    parkScreenLines();
    Serial.begin(115200);
    // Give the USB serial port a moment to open so the boot log isn't lost.
    for (uint32_t start = millis(); !Serial && millis() - start < 3000;) delay(10);
    logBoot();
}

void loop() {
    static uint32_t last = 0;
    if (millis() - last >= 10000) {
        last = millis();
        Serial.printf("Up %lus, heap %lu KB free, PSRAM %lu KB free\n", (unsigned long) (millis() / 1000), (unsigned long) (ESP.getFreeHeap() / 1024), (unsigned long) (ESP.getFreePsram() / 1024));
    }
    delay(10);
}
