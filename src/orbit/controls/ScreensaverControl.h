#pragma once

#include "display/Display.h"
#include "orbit/Control.h"

namespace orbit {

    // Decorative animations, about 20 frames a second. Same five effects as
    // info-orbs, each redrawn whole every frame:
    //   asteroids  drifting stars, a ringed "Orb-It" planet and wireframe rocks
    //              bouncing off the bezel (color tints the planet; orange)
    //   matrix     falling katakana rain (color tints the rain; green)
    //   warp       streaking stars and rings down a turning wormhole
    //              (color tints the rings; sky blue)
    //   orrery     a sun and five orbiting planets (color tints the sun; gold)
    //   radar      a sweeping scope with fading contacts (whole scope; green)
    //
    // params: effect (default "asteroids", or "cycle" to rotate through all),
    // color (optional; reported only when set), cycleSeconds (cycle only,
    // default 300, clamped to 30-86400). All settings. Any write starts the
    // animation afresh.
    class ScreensaverControl : public Control {
    public:
        ScreensaverControl();
        ~ScreensaverControl();
        const char *name() const override { return "screensaver"; }
        int parse(JsonObjectConst params, JsonObject out, String &error) const override;
        AfterWrite applied(int screen, JsonObjectConst request, JsonObjectConst stored) override;
        void draw(LGFX_Sprite &spr, JsonObjectConst params, const DrawContext &ctx) override;
        uint32_t refreshMs(int, JsonObjectConst) const override { return 50; }

        static const char *const EFFECTS[]; // names, nullptr-terminated, cycle order

        struct Effect {
            virtual ~Effect() = default;
            virtual void seed(uint32_t now) = 0;
            virtual void step(float dt, uint32_t now) = 0;
            virtual void paint(LGFX_Sprite &spr, uint16_t color) = 0;
            virtual uint16_t defaultColor() const = 0;
        };

    private:
        struct Screen {
            Effect *effects[5];
            int showing = -1;
            bool fresh = true;
            uint32_t lastFrame = 0;
            uint32_t effectStarted = 0;
        };
        Screen screens[display::NUM_SCREENS];
    };

}
