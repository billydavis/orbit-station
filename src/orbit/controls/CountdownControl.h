#pragma once

#include "display/Display.h"
#include "orbit/Control.h"

namespace orbit {

    // A countdown timer: a ring that empties as time runs out, MM:SS in the
    // middle and an optional label. The ring turns red near the end (the last
    // 10%, or 30 s if that's shorter). At zero the whole screen flashes
    // "TIME'S UP" until it's dismissed. Same look as info-orbs.
    //
    // Writes are commands, params.action:
    //   set      durationSeconds (required), label, color: starts a new run.
    //            The only action that can turn a screen into a countdown.
    //   pause    while running        resume   while paused
    //   restart  same duration, from the start
    //   stop     puts back what the screen showed before the first set
    //
    // Nothing about a countdown is saved: after a restart the screen shows
    // what it showed before the countdown.
    class CountdownControl : public Control {
    public:
        const char *name() const override { return "countdown"; }
        int parse(JsonObjectConst params, JsonObject out, String &error) const override;
        int parseWrite(const WriteContext &ctx, JsonObjectConst params, JsonObject out, String &error) override;
        AfterWrite applied(int screen, JsonObjectConst request, JsonObjectConst stored) override;
        void report(int screen, JsonObject params) override;
        bool saved() const override { return false; }
        void draw(LGFX_Sprite &spr, JsonObjectConst params, const DrawContext &ctx) override;
        uint32_t refreshMs(int screen, JsonObjectConst params) const override;

    private:
        enum class State : uint8_t { Idle, Running, Paused, Completed };
        struct Run {
            State state = State::Idle;
            uint32_t totalMs = 0;
            uint32_t startedAt = 0;   // millis() the current running stretch counts from
            uint32_t pausedLeft = 0;  // remaining ms when paused
            uint32_t completedAt = 0; // millis() it reached zero
        };
        Run runs[display::NUM_SCREENS];

        uint32_t remainingMs(const Run &r, uint32_t now) const;
        void tick(Run &r, uint32_t now);
    };

}
