#include "GifPlayer.h"
#include <AnimatedGIF.h>

struct GifPlayer::State {
    uint16_t *canvas = nullptr;
    int width = 0, height = 0;
    // The frame just drawn, whose disposal applies before the next one.
    int fx = 0, fy = 0, fw = 0, fh = 0;
    uint8_t disposal = 0;
    // What a disposal-3 frame covered, to put back afterwards.
    uint16_t *saved = nullptr;
};

namespace {
    // Called for each line of a frame. Line 0 comes first, so a frame that
    // will be undone (disposal 3) saves what it covers before drawing.
    void line(GIFDRAW *d) {
        GifPlayer::State &s = *(GifPlayer::State *) d->pUser;
        if (d->y == 0) {
            s.fx = d->iX, s.fy = d->iY, s.fw = d->iWidth, s.fh = d->iHeight;
            s.disposal = d->ucDisposalMethod;
            if (s.disposal == 3) {
                if (!s.saved) s.saved = (uint16_t *) heap_caps_malloc(s.width * s.height * 2, MALLOC_CAP_SPIRAM);
                if (s.saved) {
                    for (int y = max(s.fy, 0); y < min(s.fy + s.fh, s.height); y++) {
                        memcpy(s.saved + y * s.width + max(s.fx, 0), s.canvas + y * s.width + max(s.fx, 0),
                               max(0, min(s.fx + s.fw, s.width) - max(s.fx, 0)) * 2);
                    }
                }
            }
        }
        const int y = d->iY + d->y;
        if (y < 0 || y >= s.height) return;
        uint16_t *row = s.canvas + y * s.width;
        const uint16_t *palette = d->pPalette;
        const uint8_t *px = d->pPixels;
        const int x0 = max(0, -d->iX), x1 = min(d->iWidth, s.width - d->iX);
        if (d->ucHasTransparency) {
            const uint8_t clear = d->ucTransparent;
            for (int x = x0; x < x1; x++) {
                if (px[x] != clear) row[d->iX + x] = palette[px[x]]; // keep what's beneath
            }
        } else {
            for (int x = x0; x < x1; x++) row[d->iX + x] = palette[px[x]];
        }
    }
}

GifPlayer::~GifPlayer() { close(); }

void GifPlayer::close() {
    if (gif) {
        gif->close();
        gif->~AnimatedGIF();
        free(gif);
        gif = nullptr;
    }
    if (state) {
        free(state->saved);
        delete state;
        state = nullptr;
    }
    free(data);
    data = nullptr;
}

bool GifPlayer::open(uint8_t *bytes, size_t length, int width, int height, String &error) {
    close();
    data = bytes;
    frames = 0;
    counted = looped = false;
    // AnimatedGIF keeps ~25 KB of decoder state: PSRAM.
    gif = (AnimatedGIF *) heap_caps_malloc(sizeof(AnimatedGIF), MALLOC_CAP_SPIRAM);
    if (!gif) {
        error = "out of memory";
        return false;
    }
    new (gif) AnimatedGIF();
    gif->begin(GIF_PALETTE_RGB565_BE); // byte-swapped, as sprites hold pixels
    if (!gif->open(data, length, line)) {
        error = "not a readable GIF";
        return false;
    }
    if (gif->getCanvasWidth() != width || gif->getCanvasHeight() != height) {
        error = "not " + String(width) + "x" + String(height);
        return false;
    }
    state = new State();
    state->width = width;
    state->height = height;
    return true;
}

uint32_t GifPlayer::next(uint16_t *canvas) {
    if (!gif || !state) return 1000;
    State &s = *state;
    s.canvas = canvas;
    // The last frame's disposal: 2 = back to the background (black), 3 =
    // put back what it covered.
    const int x0 = max(s.fx, 0), x1 = min(s.fx + s.fw, s.width);
    for (int y = max(s.fy, 0); y < min(s.fy + s.fh, s.height) && x1 > x0; y++) {
        if (s.disposal == 2) memset(canvas + y * s.width + x0, 0, (x1 - x0) * 2);
        else if (s.disposal == 3 && s.saved) memcpy(canvas + y * s.width + x0, s.saved + y * s.width + x0, (x1 - x0) * 2);
    }
    s.disposal = 0;
    if (looped) {
        // Back to the start, on a clean canvas.
        gif->reset();
        memset(canvas, 0, s.width * s.height * 2);
        looped = false;
    }
    int delay = 0;
    const int more = gif->playFrame(false, &delay, &s);
    if (more < 0) {
        // A broken frame: start again from the top.
        looped = true;
        return 100;
    }
    if (!counted) frames++;
    if (more == 0) looped = counted = true;
    // Browsers show a 0 or 10 ms frame for 100 ms; so do we.
    return delay <= 10 ? 100 : delay;
}
