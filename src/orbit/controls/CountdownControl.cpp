#include "CountdownControl.h"
#include "ui/Text.h"

namespace orbit {

    namespace {
        constexpr int CENTRE = 120;
        constexpr int OUTER = 108;
        constexpr int INNER = OUTER - 16;
        constexpr uint16_t TRACK = TFT_DARKGREY;
        constexpr uint16_t SILVER = 0xC618;
        constexpr int TIME_Y = CENTRE - 8, TIME_SIZE = 34;
        constexpr int LABEL_Y = CENTRE + 24, LABEL_SIZE = 13;
        constexpr uint32_t FLASH_MS = 500;

        // Angles are degrees clockwise from 12 o'clock; LovyanGFX counts from 3.
        void ring(LGFX_Sprite &spr, float from, float to, uint16_t color) {
            if (to <= from) return;
            float s = from - 90, e = to - 90;
            if (s < 0) {
                spr.fillArc(CENTRE, CENTRE, OUTER, INNER, s + 360, fminf(e + 360, 360), color);
                if (e > 0) spr.fillArc(CENTRE, CENTRE, OUTER, INNER, 0, e, color);
            } else {
                spr.fillArc(CENTRE, CENTRE, OUTER, INNER, s, e, color);
            }
        }

        void cap(LGFX_Sprite &spr, float angle, uint16_t color) {
            const float rad = angle * DEG_TO_RAD, mid = (OUTER + INNER) / 2.0f;
            spr.fillCircle(CENTRE + lroundf(mid * sinf(rad)), CENTRE - lroundf(mid * cosf(rad)), (OUTER - INNER) / 2, color);
        }

        String mmss(uint32_t ms) {
            // Rounded up, so it doesn't show 00:00 before it's actually done.
            const uint32_t s = (ms + 999) / 1000;
            char buf[12];
            snprintf(buf, sizeof(buf), "%02lu:%02lu", (unsigned long) (s / 60), (unsigned long) (s % 60));
            return buf;
        }

        const char *stateName(int s) {
            static const char *const NAMES[] = {"idle", "running", "paused", "completed"};
            return NAMES[s];
        }
    }

    uint32_t CountdownControl::remainingMs(const Run &r, uint32_t now) const {
        switch (r.state) {
        case State::Running: {
            const uint32_t elapsed = now - r.startedAt;
            return elapsed >= r.totalMs ? 0 : r.totalMs - elapsed;
        }
        case State::Paused:
            return r.pausedLeft;
        case State::Completed:
            return 0;
        default:
            return r.totalMs;
        }
    }

    void CountdownControl::tick(Run &r, uint32_t now) {
        if (r.state == State::Running && remainingMs(r, now) == 0) {
            r.state = State::Completed;
            r.completedAt = now;
        }
    }

    // Restoring a saved layout never meets a countdown (it isn't saved), and
    // a bulk write has no screen to check actions against; both only accept
    // set, as a fresh countdown.
    int CountdownControl::parse(JsonObjectConst in, JsonObject out, String &error) const {
        WriteContext none = {-1, false, JsonObjectConst()};
        return const_cast<CountdownControl *>(this)->parseWrite(none, in, out, error);
    }

    int CountdownControl::parseWrite(const WriteContext &ctx, JsonObjectConst in, JsonObject out, String &error) {
        if (!in["action"].is<const char *>()) {
            error = "control 'countdown' requires params.action ('set'|'pause'|'resume'|'stop'|'restart')";
            return 400;
        }
        const String action = in["action"].as<String>();
        const bool isCountdown = ctx.sameControl && ctx.screen >= 0;
        const State state = isCountdown ? runs[ctx.screen].state : State::Idle;

        if (action == "set") {
            if (!in["durationSeconds"].is<int>() || in["durationSeconds"].as<int>() <= 0) {
                error = "countdown action 'set' requires a positive integer params.durationSeconds";
                return 400;
            }
            out["durationSeconds"] = in["durationSeconds"].as<int>();
            int status;
            if ((status = optionalString(in, out, "label", "", 32, error))) return status;
            return optionalColor(in, out, "color", TFT_CYAN, error);
        }
        if (action == "pause" && !(isCountdown && state == State::Running)) {
            error = "countdown action 'pause' requires the countdown to currently be running";
            return 400;
        }
        if (action == "resume" && !(isCountdown && state == State::Paused)) {
            error = "countdown action 'resume' requires the countdown to currently be paused";
            return 400;
        }
        if (action == "restart" && !(isCountdown && (int) ctx.current["durationSeconds"] > 0)) {
            error = "countdown action 'restart' requires the screen to already be a countdown with a duration set (use action 'set' first)";
            return 400;
        }
        if (action == "stop" && !isCountdown) {
            error = "countdown action 'stop' requires the screen to currently be a countdown";
            return 400;
        }
        if (action != "pause" && action != "resume" && action != "restart" && action != "stop") {
            error = "unknown countdown action '" + action + "'";
            return 400;
        }
        // Not a new configuration: keep the current one.
        out.set(ctx.current);
        return 0;
    }

    AfterWrite CountdownControl::applied(int screen, JsonObjectConst request, JsonObjectConst stored) {
        Run &r = runs[screen];
        const String action = request["action"].as<String>();
        const uint32_t now = millis();
        tick(r, now);
        if (action == "set" || action == "restart") {
            r.totalMs = (uint32_t) stored["durationSeconds"] * 1000UL;
            r.startedAt = now;
            r.state = State::Running;
        } else if (action == "pause") {
            r.pausedLeft = remainingMs(r, now);
            r.state = State::Paused;
        } else if (action == "resume") {
            r.startedAt = now - (r.totalMs - r.pausedLeft); // carry on where it stopped
            r.state = State::Running;
        } else if (action == "stop") {
            r = Run();
            return AfterWrite::Restore;
        }
        return AfterWrite::Keep;
    }

    void CountdownControl::report(int screen, JsonObject params) {
        Run &r = runs[screen];
        const uint32_t now = millis();
        tick(r, now);
        params["state"] = stateName((int) r.state);
        params["remainingSeconds"] = remainingMs(r, now) / 1000;
    }

    uint32_t CountdownControl::refreshMs(int screen, JsonObjectConst) const {
        const State s = runs[screen].state;
        return s == State::Running || s == State::Completed ? 100 : 0;
    }

    void CountdownControl::draw(LGFX_Sprite &spr, JsonObjectConst p, const DrawContext &ctx) {
        Run &r = runs[ctx.screen];
        tick(r, ctx.nowMs);
        const uint16_t color = p["color"];
        String label = p["label"].as<String>();

        if (r.state == State::Completed) {
            const bool on = ((ctx.nowMs - r.completedAt) / FLASH_MS) % 2 == 0;
            const uint16_t bg = on ? color : TFT_BLACK, fg = on ? TFT_BLACK : color;
            spr.fillScreen(bg);
            text::draw(spr, "TIME'S UP", CENTRE, TIME_Y, 20, Align::MiddleCenter, fg, bg);
            if (label.length()) text::draw(spr, label, CENTRE, LABEL_Y, LABEL_SIZE, Align::MiddleCenter, fg, bg);
            return;
        }

        spr.fillScreen(TFT_BLACK);
        if (r.state == State::Idle) {
            ring(spr, 0, 360, TRACK);
            text::draw(spr, "READY", CENTRE, TIME_Y, TIME_SIZE - 8, Align::MiddleCenter, SILVER, TFT_BLACK);
            if (label.length()) text::draw(spr, label, CENTRE, LABEL_Y, LABEL_SIZE, Align::MiddleCenter, SILVER, TFT_BLACK);
            return;
        }

        const bool paused = r.state == State::Paused;
        const uint32_t left = remainingMs(r, ctx.nowMs);
        const float fraction = r.totalMs ? constrain((float) left / r.totalMs, 0.0f, 1.0f) : 0;
        const bool warning = !paused && left <= min(r.totalMs / 10, (uint32_t) 30000);
        const uint16_t fill = paused ? TFT_DARKGREY : (warning ? TFT_RED : color);

        // The filled part runs clockwise from 12 o'clock and shrinks as time
        // passes; the rest is track.
        const float sweep = 360.0f * fraction;
        ring(spr, sweep, 360, TRACK);
        ring(spr, 0, sweep, fill);
        if (sweep > 0 && sweep < 360) {
            cap(spr, 0, fill);
            cap(spr, sweep, fill);
        }

        text::draw(spr, mmss(left), CENTRE, TIME_Y, TIME_SIZE, Align::MiddleCenter, TFT_WHITE, TFT_BLACK);
        if (paused) label = label.length() ? label + " - PAUSED" : String("PAUSED");
        if (label.length()) text::draw(spr, label, CENTRE, LABEL_Y, LABEL_SIZE, Align::MiddleCenter, SILVER, TFT_BLACK);
    }

}
