#pragma once

#include "orbit/Control.h"
#include "ui/GifPlayer.h"
#include <atomic>
#include <memory>
#include <vector>

namespace orbit {

    // Your own pictures, uploaded to the board's picture store (see
    // orbit/Pictures.h): 240x240 JPEG, PNG, GIF or WebP, made and cropped by
    // the client (Command Center), shown as a slideshow. Animated GIFs play.
    //
    // params (all settings):
    //   pictures    the pictures to show, by hash, in order (up to 24)
    //   seconds     how long each stays (default 30, 5-86400); an animation
    //               plays for that long, looping
    //   order       list (default) or shuffle (a new order each round, never
    //               the same picture twice in a row)
    //   transition  how one picture changes to the next: random (default),
    //               none, or one of transition::CHOICES
    //
    // A picture the board doesn't have is skipped, and listed in GET as
    // picturesMissing; it joins the slideshow as soon as it's uploaded. An
    // animated WebP shows its first frame (decoding WebP is far too slow to
    // play at 240x240).
    //
    // A task of its own (the loader, on core 1) decodes the next picture
    // ahead, and an animation's next frame, so the render task only copies
    // finished pictures and never waits. A GIF stays compressed in PSRAM and
    // is decoded a frame at a time (20-30 ms a frame); if the loader falls
    // behind, the animation slows down, not the other screens.
    //
    // PSRAM per screen: 115 KB for the picture shown, 115 KB for the next in
    // a slideshow, 115 KB for an animation's next frame, plus an animated
    // GIF's file (up to 1 MB) for the one shown and the one ahead.
    class PictureControl : public Control {
    public:
        const char *name() const override { return "picture"; }
        int parse(JsonObjectConst params, JsonObject out, String &error) const override;
        void draw(LGFX_Sprite &spr, JsonObjectConst params, const DrawContext &ctx) override;
        uint32_t refreshMs(int screen, JsonObjectConst params) const override;
        void report(int screen, JsonObject params) override;
        uint32_t contentId(int screen) const override;

        static constexpr int MAX_PICTURES = 24;

        // The loader's part (see PictureControl.cpp). A buffer is the
        // loader's while its state is Loading, and the render task's
        // otherwise.
        enum Load { Idle, Loading, Ready, Failed };
        struct Buffer {
            LGFX_Sprite *sprite = nullptr;     // 240x240, PSRAM
            String hash;                       // what it holds (or is loading, or failed to)
            std::unique_ptr<GifPlayer> player; // an animated GIF's decoder, positioned after the frame shown
            uint32_t delay = 0;                // how long the frame in sprite shows
        };
        struct Screen {
            Buffer shown;
            Buffer ahead;                 // the next picture in a slideshow
            std::atomic<int> state{Idle}; // of ahead
            // An animation's next frame, for shown.player.
            LGFX_Sprite *frame = nullptr;
            uint32_t frameDelay = 0;
            std::atomic<int> frameState{Idle};
            uint32_t frameAt = 0;  // when the frame shown went up
            uint32_t frameSeq = 0; // goes up with each frame shown
            uint32_t seqAtShow = 0; // frameSeq when the picture went up (for the log)
            // The slideshow, rebuilt when the params or the store change.
            uint32_t key = 0; // of the params the slideshow comes from
            uint32_t storeGeneration = 0;
            std::vector<String> sequence; // held pictures in show order
            std::vector<String> failed;   // ones that couldn't be decoded
            String upcoming;              // the next to show, decided once per change
            uint32_t shownAt = 0;
            // What each display picture holds, from this control.
            String drawn[2];
            uint32_t drawnSeq[2] = {};
        };

    private:
        Screen screens[5];
        void rebuild(Screen &s, JsonObjectConst p, uint32_t key);
        String nextAfter(Screen &s, bool shuffle);
    };

}
