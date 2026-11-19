#include "TimeControl.h"
#include "core/Settings.h"
#include "core/TimeKeeper.h"
#include "ui/Text.h"

namespace orbit {

    int TimeControl::parse(JsonObjectConst in, JsonObject out, String &error) const {
        int status;
        if ((status = optionalBool(in, out, "showDate", false, error))) return status;
        if ((status = optionalBool(in, out, "showDay", false, error))) return status;
        return optionalBool(in, out, "format24Hour", false, error);
    }

    void TimeControl::draw(LGFX_Sprite &spr, JsonObjectConst params, const DrawContext &) {
        constexpr int CENTRE = 120;
        constexpr uint16_t FG = TFT_WHITE;
        constexpr uint16_t BG = TFT_BLACK;
        spr.fillScreen(BG);

        struct tm t;
        if (!timekeeper::now(t)) {
            // Not synced from the internet yet (no WiFi, or just started).
            text::draw(spr, "--:--", CENTRE, 120, 66, Align::MiddleCenter, 0x7BEF, BG);
            return;
        }

        char buf[32];
        if (params["showDate"]) {
            strftime(buf, sizeof(buf), settings::get().metric ? "%d %B" : "%B %d", &t);
            String date = buf;
            // Day without a leading zero: "November 9", "9 November".
            date.replace(" 0", " ");
            if (date.startsWith("0")) date.remove(0, 1);
            text::draw(spr, date, CENTRE, 50, 18, Align::MiddleCenter, FG, BG);
        }
        if (params["showDay"]) {
            strftime(buf, sizeof(buf), "%A", &t);
            text::draw(spr, buf, CENTRE, 190, 22, Align::MiddleCenter, FG, BG);
        }
        int hour = t.tm_hour;
        if (!params["format24Hour"]) hour = hour % 12 == 0 ? 12 : hour % 12;
        snprintf(buf, sizeof(buf), "%d:%02d", hour, t.tm_min);
        text::draw(spr, buf, CENTRE, 120, 66, Align::MiddleCenter, FG, BG);
    }

}
