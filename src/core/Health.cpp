#include "Health.h"
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <stdarg.h>
#include <time.h>

namespace health {

    namespace {
        // How long each task may go without a beat. The network tasks block on
        // HTTPS requests (up to about 35 s for a large download), so theirs are long.
        const uint32_t DEADLINE_MS[] = {10000, 10000, 120000, 180000};
        const char *const TASK_NAMES[] = {"display", "render", "feeds", "makapix"};

        constexpr size_t HTTPS_NEEDS = 20 * 1024;     // one HTTPS connection, with margin
        constexpr uint32_t LOW_MEMORY_MS = 3 * 60000; // that short for this long: restart
        constexpr uint32_t MEMORY_REPORT_MS = 15 * 60000;

        constexpr int LOG_SIZE = 48, LOG_LINE = 112;
        struct Entry {
            uint32_t uptime;
            time_t wall; // 0 if the clock wasn't set
            char text[LOG_LINE];
        };
        Entry *entries; // in PSRAM
        int logNext = 0, logCount = 0;
        portMUX_TYPE logLock = portMUX_INITIALIZER_UNLOCKED;

        volatile uint32_t lastBeat[(int) Task::Count];
        volatile bool paused = false;
        uint32_t lowSince = 0, lastReport = 0;
        size_t minLargestBlock = SIZE_MAX;

        // Survives a software restart or watchdog reset (not a power cut).
        constexpr uint32_t MAGIC = 0x4F524254; // "ORBT"
        struct Record {
            uint32_t magic;
            uint32_t uptime;
            char why[96];
        };
        RTC_NOINIT_ATTR Record record;

        String lastReason, lastDetail;
        uint32_t lastUptime = 0;

        const char *resetName(esp_reset_reason_t r) {
            switch (r) {
            case ESP_RST_POWERON: return "power on";
            case ESP_RST_EXT: return "reset pin";
            case ESP_RST_SW: return "software restart";
            case ESP_RST_PANIC: return "crash";
            case ESP_RST_INT_WDT: return "interrupt watchdog";
            case ESP_RST_TASK_WDT: return "task watchdog";
            case ESP_RST_WDT: return "watchdog";
            case ESP_RST_BROWNOUT: return "brownout";
            case ESP_RST_DEEPSLEEP: return "deep sleep";
            default: return "flashing or unknown";
            }
        }
    }

    void begin() {
        entries = (Entry *) heap_caps_calloc(LOG_SIZE, sizeof(Entry), MALLOC_CAP_SPIRAM);
        const esp_reset_reason_t r = esp_reset_reason();
        lastReason = resetName(r);
        if (record.magic == MAGIC && r != ESP_RST_POWERON) {
            record.why[sizeof(record.why) - 1] = 0;
            lastDetail = record.why;
            lastUptime = record.uptime;
        }
        record.magic = 0;
        if (lastDetail.length()) event("Started after %s: %s (had been up %lus)", lastReason.c_str(), lastDetail.c_str(), (unsigned long) lastUptime);
        else event("Started after %s", lastReason.c_str());
        // The Arduino loop is on the hardware watchdog (5 s).
        enableLoopWDT();
    }

    void beat(Task t) { lastBeat[(int) t] = millis() | 1; }

    void pause(bool p) {
        paused = p;
        if (p) {
            disableLoopWDT();
        } else {
            enableLoopWDT();
            for (volatile uint32_t &b : lastBeat) {
                if (b) b = millis() | 1; // fresh start after the pause
            }
        }
    }

    void restart(const char *why) {
        record.magic = MAGIC;
        record.uptime = millis() / 1000;
        strlcpy(record.why, why, sizeof(record.why));
        event("Restarting: %s", why);
        Serial.flush();
        delay(200);
        esp_restart();
        for (;;) {}
    }

    void loop() {
        const uint32_t now = millis();
        if (!paused) {
            for (int i = 0; i < (int) Task::Count; i++) {
                const uint32_t b = lastBeat[i];
                // Signed: a task may beat between reading now and reading its
                // beat, which makes this slightly negative, not huge.
                const int32_t silent = (int32_t) (now - b);
                if (b && silent > (int32_t) DEADLINE_MS[i]) {
                    char why[96];
                    snprintf(why, sizeof(why), "%s task stopped responding for %lds", TASK_NAMES[i], (long) (silent / 1000));
                    restart(why);
                }
            }
        }

        const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        minLargestBlock = min(minLargestBlock, largest);
        if (largest < HTTPS_NEEDS) {
            if (!lowSince) {
                lowSince = now | 1;
                event("Low memory: largest free block %u bytes", (unsigned) largest);
            } else if (now - lowSince > LOW_MEMORY_MS) {
                restart("internal memory too fragmented for HTTPS");
            }
        } else if (lowSince) {
            lowSince = 0;
            event("Memory recovered: largest free block %u bytes", (unsigned) largest);
        }

        if (now - lastReport >= MEMORY_REPORT_MS) {
            lastReport = now;
            event("Memory: %u KB free (lowest %u KB), largest block %u KB, PSRAM %u KB free", (unsigned) (ESP.getFreeHeap() / 1024),
                  (unsigned) (ESP.getMinFreeHeap() / 1024), (unsigned) (largest / 1024), (unsigned) (ESP.getFreePsram() / 1024));
        }
    }

    void event(const char *fmt, ...) {
        char text[LOG_LINE];
        va_list args;
        va_start(args, fmt);
        vsnprintf(text, sizeof(text), fmt, args);
        va_end(args);
        Serial.println(text);
        if (!entries) return;
        const time_t t = time(nullptr);
        portENTER_CRITICAL(&logLock);
        Entry &e = entries[logNext];
        e.uptime = millis() / 1000;
        e.wall = t > 1700000000 ? t : 0;
        memcpy(e.text, text, sizeof(text));
        logNext = (logNext + 1) % LOG_SIZE;
        if (logCount < LOG_SIZE) logCount++;
        portEXIT_CRITICAL(&logLock);
    }

    void toJson(JsonObject out) {
        out["lastRestart"] = lastReason;
        if (lastDetail.length()) out["lastRestartDetail"] = lastDetail;
        out["lowestLargestFreeBlock"] = minLargestBlock == SIZE_MAX ? 0 : minLargestBlock;
    }

    void logJson(JsonArray out) {
        if (!entries) return;
        // Copy out under the lock, format outside it.
        static Entry *copy = (Entry *) heap_caps_malloc(LOG_SIZE * sizeof(Entry), MALLOC_CAP_SPIRAM);
        if (!copy) return;
        int count, start;
        portENTER_CRITICAL(&logLock);
        count = logCount;
        start = (logNext - logCount + LOG_SIZE) % LOG_SIZE;
        for (int i = 0; i < count; i++) copy[i] = entries[(start + i) % LOG_SIZE];
        portEXIT_CRITICAL(&logLock);
        for (int i = 0; i < count; i++) {
            JsonObject o = out.add<JsonObject>();
            o["uptime"] = copy[i].uptime;
            if (copy[i].wall) {
                char iso[24];
                struct tm tm;
                localtime_r(&copy[i].wall, &tm);
                strftime(iso, sizeof(iso), "%Y-%m-%d %H:%M:%S", &tm);
                o["time"] = iso;
            }
            o["event"] = copy[i].text;
        }
    }

}
