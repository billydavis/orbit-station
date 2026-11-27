#pragma once

// Plays a GIF one frame at a time onto a canvas, from the compressed file
// in memory, so a long animation costs its file size, not 115 KB a frame.
// Loops forever. The canvas must keep what the last frame left (GIF frames
// draw over the one before).

#include <Arduino.h>

class AnimatedGIF;

class GifPlayer {
public:
    ~GifPlayer();
    // Takes data (PSRAM, from heap_caps_malloc) and frees it when done.
    // The GIF must be width x height.
    bool open(uint8_t *data, size_t length, int width, int height, String &error);
    // Draws the next frame onto canvas (width x height, byte-swapped RGB565
    // as sprites hold it) and returns how long it shows, in ms.
    uint32_t next(uint16_t *canvas);
    // Whether the GIF has more than one frame (known after the first loop,
    // or as soon as a second frame is seen).
    bool animated() const { return frames > 1; }
    // Whether it turned out to have just the one frame (after next()).
    bool single() const { return counted && frames == 1; }

    struct State; // for the decoder's line callback

private:
    AnimatedGIF *gif = nullptr;
    uint8_t *data = nullptr;
    State *state = nullptr;
    int frames = 0;       // counted through the first loop
    bool counted = false; // the first loop is done
    bool looped = false;  // the last frame was drawn: start again next
    void close();
};
