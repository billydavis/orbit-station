#include "Demo.h"
#include "display/Display.h"
#include <Arduino.h>

namespace demo {

    namespace {
        constexpr int NUM_DOTS = 6;
        constexpr int DOT_R = 18;
        const uint16_t SCREEN_COLORS[display::NUM_SCREENS] = {TFT_RED, TFT_GREEN, TFT_BLUE, TFT_YELLOW, TFT_MAGENTA};

        volatile bool isPaused = false;

        void drawFrame(LGFX_Sprite &spr, int s, uint32_t tick, int fps) {
            spr.fillScreen(TFT_BLACK);
            float t = (tick + s * 20) * 0.05f;
            for (int r = 0; r < NUM_DOTS; r++) {
                float a = t + r * 1.047f;
                int x = 120 + cosf(a) * (60 + r * 8);
                int y = 120 + sinf(a) * (60 + r * 8);
                spr.fillCircle(x, y, DOT_R, spr.color565(40 * r, 255 - 40 * r, 128 + 20 * s));
            }
            spr.setTextDatum(textdatum_t::middle_center);
            spr.setFont(&fonts::Font7);
            spr.setTextColor(SCREEN_COLORS[s]);
            spr.drawNumber(s + 1, 120, 120);
            spr.setFont(&fonts::Font2);
            spr.setTextColor(TFT_WHITE);
            spr.drawString(String(fps) + " fps", 120, 215);
        }

        void task(void *) {
            uint32_t tick = 0;
            uint32_t frames = 0;
            int fps = 0;
            uint32_t secondStart = millis();
            uint32_t lastYield = millis();
            for (;;) {
                for (int s = 0; s < display::NUM_SCREENS; s++) {
                    LGFX_Sprite &spr = display::beginFrame(s);
                    drawFrame(spr, s, tick, fps);
                    display::endFrame(s);
                }
                frames++;
                if (!isPaused) tick++;
                if (millis() - secondStart >= 1000) {
                    fps = frames * 1000 / (millis() - secondStart);
                    frames = 0;
                    secondStart = millis();
                }
                // If drawing is ever the slowest step, this task never waits and
                // core 0's idle task starves, which trips the task watchdog.
                if (millis() - lastYield >= 100) {
                    vTaskDelay(1);
                    lastYield = millis();
                }
            }
        }
    }

    void begin() {
        xTaskCreatePinnedToCore(task, "demo", 8192, nullptr, 1, nullptr, 0);
    }

    void setPaused(bool p) { isPaused = p; }
    bool paused() { return isPaused; }

}
