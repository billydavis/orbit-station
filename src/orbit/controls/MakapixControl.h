#pragma once

#include "orbit/Control.h"

namespace orbit {

    // Pixel art from Makapix Club (makapix.club), animated, changing every so
    // often. Screens on the same feed each show a different artwork. Art is
    // scaled by the largest whole number that fits (32x32 at 7x, 64x64 at 3x)
    // and centred on black; 256x256 art is shrunk to fit.
    //
    // params (all settings):
    //   channel        all (default), promoted, user (the board owner's art),
    //                  by_user, hashtag, reactions
    //   user           handle, for by_user and reactions
    //   hashtag        without the #, for hashtag
    //   sort           random (default), server_order, created_at
    //   changeSeconds  how long each artwork stays (default 120, 10-86400)
    //   fit            crisp (default): largest whole multiple, centred on black
    //                  fill: covers the whole screen; long edges are trimmed and
    //                  pixels may be slightly uneven
    //
    // The first makapix screen pairs the board with makapix.club: it shows a
    // code to register there (see makapix/Makapix.h).
    class MakapixControl : public Control {
    public:
        const char *name() const override { return "makapix"; }
        int parse(JsonObjectConst params, JsonObject out, String &error) const override;
        void draw(LGFX_Sprite &spr, JsonObjectConst params, const DrawContext &ctx) override;
        uint32_t refreshMs(int screen, JsonObjectConst params) const override;

    private:
        uint32_t frameMs[5] = {500, 500, 500, 500, 500}; // per screen, from the art showing
    };

}
