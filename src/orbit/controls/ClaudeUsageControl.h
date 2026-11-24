#pragma once

#include "display/Display.h"
#include "orbit/Control.h"

namespace orbit {

    // Claude plan usage: how much of the 5-hour session limit and the weekly
    // limit has been used, and when each resets. A client (OrbIt Command
    // Center, fed by Claude Code's status line) sends the numbers; the board
    // counts down to the resets by itself.
    //
    // params (all optional):
    //   style ("pace")           pace: two bars, with a tick for how far
    //                            through each window it is, and a verdict.
    //                            orbit: two rings with planets, big logo,
    //                            both numbers side by side.
    //                            tanks: two tanks of liquid, session left
    //                            and week right
    //   animate (true)           move the logo (orbit's planets, the tanks'
    //                            surfaces) while usage is changing
    //   session, week            percent used, 0-100; live, never saved.
    //                            Left out = no reading yet.
    //   sessionResetsAt,         when each window resets, in whole Unix
    //   weekResetsAt             seconds; live, never saved
    //
    // Once a window's reset time has passed, it shows 0% until the next
    // reading, since that's what a fresh window holds. After a restart the
    // screen keeps its style and shows "Waiting for data".
    class ClaudeUsageControl : public Control {
    public:
        const char *name() const override { return "claudeUsage"; }
        int parse(JsonObjectConst params, JsonObject out, String &error) const override;
        void stripLive(JsonObject params) const override;
        void draw(LGFX_Sprite &spr, JsonObjectConst params, const DrawContext &ctx) override;
        uint32_t refreshMs(int screen, JsonObjectConst params) const override;
        AfterWrite applied(int screen, JsonObjectConst request, JsonObjectConst stored) override;
        void report(int screen, JsonObject params) override;

    private:
        // Whether the readings are moving, which is when the logo animates.
        struct Activity {
            bool seen = false;    // a reading arrived since boot
            bool changed = false; // and a later one differed from it
            float session = 0, week = 0;
            uint32_t changedAt = 0; // millis()
        };
        Activity activity[display::NUM_SCREENS];

        // Everything but the logo, kept in PSRAM (115 KB a screen, made the
        // first time a screen shows this control) and repainted only when
        // its key changes: about once a minute, as the countdowns tick.
        LGFX_Sprite layer[display::NUM_SCREENS];
        String layerKey[display::NUM_SCREENS];
        // tanks only: the text's alpha (57 KB of PSRAM a screen), so the
        // rippling surfaces can be redrawn under it without redrawing text.
        uint8_t *mask[display::NUM_SCREENS] = {};

        bool active(int screen, uint32_t now) const;
        bool ensureLayer(int screen);
        uint8_t *ensureMask(int screen);
    };

}
