#include "Display.h"
#include "Lgfx.h"
#include "core/Health.h"
#include <atomic>
#include <esp_heap_caps.h>

namespace display {

    namespace {
        constexpr int TILE_COLS = WIDTH / TILE;
        constexpr int TILE_ROWS = HEIGHT / TILE;
        constexpr uint16_t ALL_TILES = (1u << TILE_COLS) - 1;
        static_assert(TILE_COLS <= 16, "changed-tile mask is 16 bits");

        struct Screen {
            LGFX_Sprite pictures[2];
            int drawing = -1;        // picture taken by beginFrame()
            int latest = -1;         // picture handed over last
            // Fingerprint of each tile the screen shows. Comparing these instead
            // of a full copy of the screen halves the PSRAM reads per frame.
            uint32_t tileHash[HEIGHT / TILE][WIDTH / TILE];
            QueueHandle_t free;      // picture indices ready to draw into
            bool needWhole = true;   // screen contents unknown: send everything
            bool inverted = true;    // most of these panels need it on
            Stats stats = {};
        };

        struct Job {
            uint8_t screen;
            uint8_t picture;
        };

        Lgfx lcd;
        Screen screens[NUM_SCREENS];
        QueueHandle_t ready; // pictures waiting to be sent
        portMUX_TYPE statsLock = portMUX_INITIALIZER_UNLOCKED;

        // Requests from other tasks, carried out by the sender (it owns the bus).
        std::atomic<uint32_t> reinitMask{0}; // bit per screen
        std::atomic<bool> reinitAllPending{false};
        std::atomic<uint32_t> invertMask{0}; // bit per screen whose invert changed
        std::atomic<uint32_t> frameWanted{0}; // screens whose whole picture must be sent again

        // Two small buffers in internal RAM. Frames are copied out of PSRAM into
        // these and sent by DMA from here: sending straight from PSRAM while the
        // other core draws into PSRAM garbled the screens on the bench. While
        // one is being sent, the next is filled.
        uint16_t *bands[2];

        // ---- Chip select ----
        // LovyanGFX may still be sending by DMA when a call returns, so always wait
        // before switching CS (waitDisplay() does nothing for these panels).
        void deselectAll() {
            lcd.waitDMA();
            for (int cs : pins::CS) digitalWrite(cs, HIGH);
        }

        void selectScreen(int s) {
            deselectAll();
            digitalWrite(pins::CS[s], LOW);
            lcd.panel().forgetWindow();
        }

        void selectAll() {
            lcd.waitDMA();
            for (int cs : pins::CS) digitalWrite(cs, LOW);
            lcd.panel().forgetWindow();
        }

        // Settings the panel loses on reset. setRotation(0) also re-sends the
        // pixel format and orientation.
        void applySettings(int s) {
            selectScreen(s);
            lcd.setRotation(0);
            lcd.invertDisplay(screens[s].inverted);
            deselectAll();
        }

        void reinitAll() {
            selectAll();
            digitalWrite(pins::RST, LOW);
            delay(10);
            digitalWrite(pins::RST, HIGH);
            delay(150);
            lcd.panel().resendInitCommands(false);
            deselectAll();
            for (int s = 0; s < NUM_SCREENS; s++) {
                applySettings(s);
                screens[s].needWhole = true;
            }
            frameWanted |= (1u << NUM_SCREENS) - 1;
        }

        void reinitScreen(int s) {
            selectScreen(s);
            lcd.panel().resendInitCommands(true);
            deselectAll();
            applySettings(s);
            screens[s].needWhole = true;
            frameWanted |= 1u << s;
        }

        // Every REFRESH_MS, one screen (in turn) gets its setup re-sent quietly
        // and a whole new frame, so one that was unplugged and plugged back in
        // recovers by itself within NUM_SCREENS * REFRESH_MS.
        constexpr uint32_t REFRESH_MS = 30000;
        uint32_t lastRefresh = 0;
        int nextRefresh = 0;

        void refreshQuietly(int s) {
            selectScreen(s);
            lcd.panel().refreshQuietly();
            deselectAll();
            applySettings(s);
            screens[s].needWhole = true;
            frameWanted |= 1u << s;
        }

        void handleRequests() {
            if (millis() - lastRefresh >= REFRESH_MS) {
                lastRefresh = millis();
                refreshQuietly(nextRefresh);
                nextRefresh = (nextRefresh + 1) % NUM_SCREENS;
            }
            if (reinitAllPending.exchange(false)) {
                reinitMask = 0;
                reinitAll();
                health::event("Display: re-initialised all screens");
            }
            uint32_t mask = reinitMask.exchange(0);
            for (int s = 0; s < NUM_SCREENS; s++) {
                if (mask & (1u << s)) {
                    reinitScreen(s);
                    health::event("Display: re-initialised screen %d", s + 1);
                }
            }
            mask = invertMask.exchange(0);
            for (int s = 0; s < NUM_SCREENS; s++) {
                if (mask & (1u << s)) {
                    selectScreen(s);
                    lcd.invertDisplay(screens[s].inverted);
                    deselectAll();
                }
            }
        }

        // Fingerprints (FNV-1a over 32-bit words) of the 16x16 tiles in one row of
        // tiles, read line by line so PSRAM is read in order. Returns which tiles
        // differ from the stored fingerprints, and stores the new ones.
        uint16_t changedTiles(const uint16_t *now, uint32_t *stored, int y0) {
            uint32_t h[TILE_COLS];
            for (uint32_t &v : h) v = 2166136261u;
            for (int line = 0; line < TILE; line++) {
                const uint32_t *row = (const uint32_t *) (now + (y0 + line) * WIDTH);
                for (int tx = 0; tx < TILE_COLS; tx++) {
                    const uint32_t *p = row + tx * (TILE / 2);
                    uint32_t v = h[tx];
                    for (int k = 0; k < TILE / 2; k++) v = (v ^ p[k]) * 16777619u;
                    h[tx] = v;
                }
            }
            uint16_t mask = 0;
            for (int tx = 0; tx < TILE_COLS; tx++) {
                if (h[tx] != stored[tx]) {
                    mask |= 1u << tx;
                    stored[tx] = h[tx];
                }
            }
            return mask;
        }

        void sendFrame(int s, LGFX_Sprite &picture) {
            Screen &sc = screens[s];
            const uint16_t *now = (const uint16_t *) picture.getBuffer();
            const bool whole = sc.needWhole;
            int tiles = 0;
            int band = 0;
            bool started = false;

            for (int ty = 0; ty < TILE_ROWS; ty++) {
                const int y0 = ty * TILE;
                // Fingerprints are worked out even when sending everything, so
                // the next frame compares against what's really on the screen.
                uint16_t changed = changedTiles(now, sc.tileHash[ty], y0);
                if (whole) changed = ALL_TILES;
                int tx = 0;
                while (tx < TILE_COLS) {
                    if (!(changed & (1u << tx))) {
                        tx++;
                        continue;
                    }
                    // Send each run of changed tiles side by side as one block.
                    const int first = tx;
                    while (tx < TILE_COLS && (changed & (1u << tx))) tx++;
                    const int x0 = first * TILE;
                    const int w = (tx - first) * TILE;

                    // This band's last transfer finished before the other band's
                    // transfer started (waitDMA below), so it's safe to refill.
                    uint16_t *buf = bands[band];
                    band ^= 1;
                    for (int line = 0; line < TILE; line++) {
                        memcpy(buf + line * w, now + (y0 + line) * WIDTH + x0, w * 2);
                    }
                    if (!started) {
                        selectScreen(s);
                        lcd.startWrite();
                        started = true;
                    }
                    lcd.waitDMA();
                    lcd.pushImageDMA(x0, y0, w, TILE, (const lgfx::swap565_t *) buf);
                    tiles += tx - first;
                }
            }
            if (started) {
                lcd.waitDMA();
                lcd.endWrite();
                deselectAll();
            }
            sc.needWhole = false;

            portENTER_CRITICAL(&statsLock);
            sc.stats.framesSent++;
            sc.stats.tilesSent += tiles;
            if (whole) sc.stats.wholeFrames++;
            portEXIT_CRITICAL(&statsLock);
        }

        void senderTask(void *) {
            uint32_t lastYield = millis();
            for (;;) {
                health::beat(health::Task::Display);
                // While frames keep coming this task never waits on the queue, so
                // let lower-priority tasks on this core (and its idle task) run.
                if (millis() - lastYield >= 100) {
                    vTaskDelay(1);
                    lastYield = millis();
                }
                handleRequests();
                Job job;
                if (xQueueReceive(ready, &job, pdMS_TO_TICKS(20)) != pdTRUE) continue;
                const uint32_t start = micros();
                Screen &sc = screens[job.screen];
                sendFrame(job.screen, sc.pictures[job.picture]);
                xQueueSend(sc.free, &job.picture, 0);
                const uint32_t took = micros() - start;
                portENTER_CRITICAL(&statsLock);
                sc.stats.busyUs += took;
                portEXIT_CRITICAL(&statsLock);
            }
        }
    }

    bool begin() {
        for (int cs : pins::CS) {
            pinMode(cs, OUTPUT);
            digitalWrite(cs, HIGH);
        }
        lcd.configure(SPI_HZ);
        selectAll();
        lcd.init(); // pulses RST and sets up the bus; only ever call this once
        deselectAll();
        for (int s = 0; s < NUM_SCREENS; s++) applySettings(s);

        bool ok = true;
        for (int i = 0; i < 2; i++) {
            bands[i] = (uint16_t *) heap_caps_malloc(WIDTH * TILE * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
            ok &= bands[i] != nullptr;
        }
        ready = xQueueCreate(NUM_SCREENS * 2, sizeof(Job));
        for (Screen &sc : screens) {
            sc.free = xQueueCreate(2, sizeof(uint8_t));
            for (uint8_t i = 0; i < 2; i++) {
                LGFX_Sprite &p = sc.pictures[i];
                p.setColorDepth(16);
                p.setPsram(true);
                ok &= p.createSprite(WIDTH, HEIGHT) != nullptr;
                xQueueSend(sc.free, &i, 0);
            }
        }
        if (!ok) {
            Serial.println("Display: out of memory setting up the pictures");
            return false;
        }

        // Same priority as Arduino's loop(), which also runs on core 1, so the two
        // share the core; drawing goes on core 0.
        xTaskCreatePinnedToCore(senderTask, "display", 4096, nullptr, 1, nullptr, 1);
        return true;
    }

    LGFX_Sprite &beginFrame(int screen) {
        Screen &sc = screens[screen];
        uint8_t i;
        xQueueReceive(sc.free, &i, portMAX_DELAY);
        sc.drawing = i;
        return sc.pictures[i];
    }

    void endFrame(int screen) {
        Screen &sc = screens[screen];
        if (sc.drawing < 0) return;
        Job job = {(uint8_t) screen, (uint8_t) sc.drawing};
        sc.latest = sc.drawing;
        sc.drawing = -1;
        xQueueSend(ready, &job, portMAX_DELAY);
    }

    const uint16_t *lastFrame(int screen) {
        const Screen &sc = screens[screen];
        if (sc.latest < 0 || sc.latest == sc.drawing) return nullptr;
        return (const uint16_t *) sc.pictures[sc.latest].getBuffer();
    }

    void requestReinit(int screen) {
        if (screen < 0) {
            reinitAllPending = true;
        } else if (screen < NUM_SCREENS) {
            reinitMask |= 1u << screen;
        }
    }

    uint32_t takeFrameRequests() { return frameWanted.exchange(0); }

    void setInverted(int screen, bool on) {
        if (screen < 0 || screen >= NUM_SCREENS) return;
        screens[screen].inverted = on;
        invertMask |= 1u << screen;
    }

    bool inverted(int screen) {
        return screen >= 0 && screen < NUM_SCREENS && screens[screen].inverted;
    }

    Stats takeStats(int screen) {
        portENTER_CRITICAL(&statsLock);
        Stats s = screens[screen].stats;
        screens[screen].stats = {};
        portEXIT_CRITICAL(&statsLock);
        return s;
    }

}
