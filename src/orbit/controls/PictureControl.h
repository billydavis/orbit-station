#pragma once

#include "orbit/Control.h"

namespace orbit {

    // Your own pictures, uploaded to the board's picture store (see
    // orbit/Pictures.h): 240x240 JPEG, PNG, GIF or WebP, made and cropped by
    // the client (Command Center).
    //
    // params (all settings):
    //   pictures    the pictures to show, by hash, in order (up to 24)
    //   seconds     how long each stays (default 30, 5-86400)
    //   order       list (default) or shuffle
    //   transition  how one picture changes to the next: random (default),
    //               none, or one of transition::CHOICES
    //
    // So far it shows the first picture the board holds (the slideshow and
    // animation come later). A picture the board doesn't have is skipped,
    // and listed in GET as picturesMissing; it shows as soon as it's
    // uploaded.
    //
    // Each screen keeps its decoded picture (115 KB of PSRAM), so a redraw is
    // a copy, not a decode.
    class PictureControl : public Control {
    public:
        const char *name() const override { return "picture"; }
        int parse(JsonObjectConst params, JsonObject out, String &error) const override;
        void draw(LGFX_Sprite &spr, JsonObjectConst params, const DrawContext &ctx) override;
        void report(int screen, JsonObject params) override;
        uint32_t contentId(int screen) const override;

        static constexpr int MAX_PICTURES = 24;

    private:
        struct Cache {
            LGFX_Sprite *sprite = nullptr; // the decoded picture, 240x240, PSRAM
            String hash;                   // what it holds (or failed to)
            String problem;                // why it couldn't be shown
        };
        Cache cache[5];

        bool load(int screen, const String &hash);
    };

}
