#pragma once

// A decoded piece of pixel art: every frame at the art's own size, as RGB565
// pixels in PSRAM, with how long each frame shows. Transparency is laid onto
// black. Decoded once, then played back by scaling (see MakapixControl).

#include <Arduino.h>
#include <memory>
#include <vector>

namespace makapix {

    struct Artwork {
        int width = 0, height = 0;
        std::vector<uint16_t *> frames; // width * height RGB565 each (native byte order)
        std::vector<uint16_t> delaysMs;
        uint32_t totalMs = 0;
        uint32_t postId = 0;
        String owner; // artist's handle, if known

        ~Artwork();
        // The frame to show this far into the animation (it loops).
        const uint16_t *frameAt(uint32_t elapsedMs) const;
        // The delay of the shortest frame (for how often to redraw); 0 for a still.
        uint32_t minDelayMs() const;
    };

    using ArtworkPtr = std::shared_ptr<const Artwork>;

    // Decodes a PNG, GIF, WebP (still or animated) or BMP file. maxBytes caps
    // the decoded size (all frames together). Returns nullptr and sets error if
    // it can't. format is Makapix's native_format; the file's own signature
    // is used if it doesn't match.
    std::shared_ptr<Artwork> decode(const uint8_t *data, size_t length, const String &format, size_t maxBytes, String &error);

}
