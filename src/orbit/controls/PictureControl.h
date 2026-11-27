#pragma once

#include "orbit/Control.h"
#include <atomic>
#include <vector>

namespace orbit {

    // Your own pictures, uploaded to the board's picture store (see
    // orbit/Pictures.h): 240x240 JPEG, PNG, GIF or WebP, made and cropped by
    // the client (Command Center), shown as a slideshow.
    //
    // params (all settings):
    //   pictures    the pictures to show, by hash, in order (up to 24)
    //   seconds     how long each stays (default 30, 5-86400)
    //   order       list (default) or shuffle (a new order each round, never
    //               the same picture twice in a row)
    //   transition  how one picture changes to the next: random (default),
    //               none, or one of transition::CHOICES
    //
    // A picture the board doesn't have is skipped, and listed in GET as
    // picturesMissing; it joins the slideshow as soon as it's uploaded. A
    // GIF or animated WebP shows its first frame (animation comes later).
    //
    // Each screen keeps the picture it shows and decodes the next one ahead,
    // on a task of its own, so a change never waits and never holds up the
    // other screens: two 115 KB PSRAM pictures per slideshow screen.
    class PictureControl : public Control {
    public:
        const char *name() const override { return "picture"; }
        int parse(JsonObjectConst params, JsonObject out, String &error) const override;
        void draw(LGFX_Sprite &spr, JsonObjectConst params, const DrawContext &ctx) override;
        uint32_t refreshMs(int screen, JsonObjectConst params) const override;
        void report(int screen, JsonObject params) override;
        uint32_t contentId(int screen) const override;

        static constexpr int MAX_PICTURES = 24;

        // The loader's part (see PictureControl.cpp).
        enum Load { Idle, Loading, Ready, Failed };
        struct Buffer {
            LGFX_Sprite *sprite = nullptr; // 240x240, PSRAM
            String hash;                   // what it holds (or is loading, or failed to)
        };
        struct Screen {
            Buffer shown;
            Buffer ahead;                  // the loader's while state is Loading
            std::atomic<int> state{Idle};  // of ahead
            String problem;                // why ahead failed
            // The slideshow, rebuilt when the params or the store change.
            String key;
            uint32_t storeGeneration = 0;
            std::vector<String> sequence; // held pictures in show order
            std::vector<String> failed;   // ones that couldn't be decoded
            String upcoming;              // the next to show, decided once per change
            uint32_t shownAt = 0;
            String drawn[2]; // what each display picture holds, from this control
        };

    private:
        Screen screens[5];
        void rebuild(Screen &s, JsonObjectConst p, const String &key);
        String nextAfter(Screen &s, bool shuffle);
    };

}
