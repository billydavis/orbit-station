#pragma once

// Display core: five 240x240 GC9A01 screens.
//
// Each screen has two pictures in PSRAM. Whoever draws a screen takes a free
// picture with beginFrame(), draws the whole thing, and hands it over with
// endFrame(). A sender task on core 1 compares it with what the screen already
// shows, 16x16 tile by tile, and sends only the tiles that changed. Everything
// that touches the SPI bus runs on that task.

#include "pins.h"
#include <LovyanGFX.hpp>

namespace display {

    constexpr int WIDTH = 240;
    constexpr int HEIGHT = 240;
    constexpr int NUM_SCREENS = pins::NUM_SCREENS;
    constexpr uint32_t SPI_HZ = 40000000; // 80 MHz fails on the breadboard

    // Sets up the screens, the pictures and the sender task. Call once.
    bool begin();

    // Waits for a free picture for this screen and returns it. It holds an
    // older frame, so draw the whole picture. Only one task may draw a screen.
    LGFX_Sprite &beginFrame(int screen);
    // Hands the picture from beginFrame() to the sender.
    void endFrame(int screen);

    // Re-initialises one screen through its own CS line (others keep running),
    // or all of them through the shared RST wire when screen is -1. Use after
    // plugging a screen back in.
    void requestReinit(int screen);

    void setInverted(int screen, bool on);
    bool inverted(int screen);

    struct Stats {
        uint32_t framesSent;  // frames handed over and processed
        uint32_t tilesSent;   // 16x16 tiles that changed and were sent
        uint32_t wholeFrames; // frames sent whole (after a reset or re-init)
        uint32_t busyUs;      // time the sender spent on this screen
    };
    // Returns the counts since the last call, then clears them.
    Stats takeStats(int screen);

    constexpr int TILE = 16;
    constexpr int TILES_PER_FRAME = (WIDTH / TILE) * (HEIGHT / TILE);

}
