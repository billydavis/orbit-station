#include "Recovery.h"
#include <Arduino.h>
#include <Preferences.h>
#include <esp_system.h>
#include <nvs_flash.h>

namespace recovery {

    namespace {
        constexpr int RESET_AFTER = 3;
        constexpr uint32_t QUICK_MS = 10000;

        int count = 0;
        bool erased = false;
        bool cleared = false;

        void saveCount(int n) {
            Preferences prefs;
            prefs.begin("recovery", false);
            prefs.putUChar("quick", n);
            prefs.end();
        }

        int loadCount() {
            Preferences prefs;
            prefs.begin("recovery", true);
            int n = prefs.getUChar("quick", 0);
            prefs.end();
            return n;
        }

        void eraseEverything() {
            // All settings and saved WiFi live in NVS.
            nvs_flash_deinit();
            nvs_flash_erase();
            nvs_flash_init();
        }
    }

    void begin() {
        // Only power-ups count. Crashes, watchdog resets, brownouts, restarts
        // after an update and resets from the flasher (which report "unknown")
        // don't, so a crash loop or a weak power supply can never wipe settings.
        const esp_reset_reason_t why = esp_reset_reason();
        const bool powerUp = why == ESP_RST_POWERON || why == ESP_RST_EXT;
        count = powerUp ? loadCount() + 1 : 0;

        if (count >= RESET_AFTER) {
            Serial.println("Recovery: third quick power-up in a row, erasing all settings");
            eraseEverything();
            erased = true;
            count = 0;
            cleared = true;
            return;
        }
        if (count > 0) {
            Serial.printf("Recovery: quick power-up %d of %d\n", count, RESET_AFTER);
            saveCount(count);
        } else {
            cleared = true; // nothing saved to clear
        }
    }

    void loop() {
        if (cleared || millis() < QUICK_MS) return;
        saveCount(0);
        count = 0;
        cleared = true;
    }

    bool didFactoryReset() { return erased; }
    int quickStarts() { return count; }

}
