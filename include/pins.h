#pragma once

// Pin map for the breadboard rig (Lonely Binary ESP32-S3 on the screw
// terminal base). Avoid GPIO 19/20 (USB) and 47/48.

namespace pins {
    constexpr int SCLK = 12; // screen SCL
    constexpr int MOSI = 11; // screen SDA
    constexpr int DC = 13;
    constexpr int RST = 14; // shared by all screens

    constexpr int NUM_SCREENS = 5;
    constexpr int CS[NUM_SCREENS] = {10, 9, 8, 17, 16};

    // Plug-detect lines, not wired yet.
    constexpr int SENSE[NUM_SCREENS] = {42, 41, 40, 39, 38};
}
