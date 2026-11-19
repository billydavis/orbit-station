#include "WeatherControl.h"
#include "core/Settings.h"
#include "data/Feeds.h"
#include "ui/Text.h"

#define WEATHER_ICON(name)                                                                       \
    extern const uint8_t name##Start[] asm("_binary_images_weather_" #name "B_jpg_start"); \
    extern const uint8_t name##End[] asm("_binary_images_weather_" #name "B_jpg_end");
WEATHER_ICON(clouds)
WEATHER_ICON(moon)
WEATHER_ICON(moonCloud)
WEATHER_ICON(rain)
WEATHER_ICON(snow)
WEATHER_ICON(sun)
WEATHER_ICON(sunClouds)

namespace orbit {

    namespace {
        constexpr int CENTRE = 120;
        const char *const ELEMENTS[] = {"icon", "temperature", "condition", nullptr};
        const char *DEG = "\xC2\xB0";

        String degreeText(float v) { return String((int) lroundf(v)) + DEG; }

        uint16_t colorOr(JsonObjectConst p, const char *key, uint16_t fallback) {
            return p[key].isNull() ? fallback : p[key].as<uint16_t>();
        }

        void drawIcon(LGFX_Sprite &spr, const String &icon) {
            const uint8_t *start = nullptr, *end = nullptr;
            if (icon == "partly-cloudy-night") start = moonCloudStart, end = moonCloudEnd;
            else if (icon == "partly-cloudy-day") start = sunCloudsStart, end = sunCloudsEnd;
            else if (icon == "clear-day") start = sunStart, end = sunEnd;
            else if (icon == "clear-night") start = moonStart, end = moonEnd;
            else if (icon == "snow") start = snowStart, end = snowEnd;
            else if (icon == "rain") start = rainStart, end = rainEnd;
            else if (icon == "fog" || icon == "wind" || icon == "cloudy") start = cloudsStart, end = cloudsEnd;
            spr.fillScreen(TFT_BLACK);
            if (start) spr.drawJpg(start, end - start, 0, 0);
        }

        // The description in up to four lines of 18 characters, split at spaces.
        void drawCondition(LGFX_Sprite &spr, const feeds::Weather &w, uint16_t fg, uint16_t bg, uint16_t city) {
            spr.fillScreen(bg);
            text::drawFitted(spr, w.city, CENTRE, 80, 210, 50, Align::MiddleCenter, city, bg);
            String rest = w.description;
            for (int line = 0, y = 125; line < 4 && rest.length(); line++, y += 25) {
                int cut = rest.length();
                if (cut > 18) {
                    cut = rest.lastIndexOf(' ', 18);
                    if (cut <= 0) cut = 18;
                }
                text::draw(spr, rest.substring(0, cut), CENTRE, y, 15, Align::MiddleCenter, fg, bg);
                rest = rest.substring(cut);
                rest.trim();
            }
        }

        void drawTemperature(LGFX_Sprite &spr, JsonObjectConst p, const feeds::Weather &w, uint16_t fg, uint16_t bg) {
            spr.fillScreen(bg);
            const bool showCity = p["showCity"];
            if (showCity) text::drawFitted(spr, w.city, CENTRE, 36, 150, 24, Align::MiddleCenter, colorOr(p, "cityColor", fg), bg);
            const String now = degreeText(w.temperature);
            if (!p["showHighLow"]) {
                if (showCity) text::drawFitted(spr, now, CENTRE, CENTRE + 6, 200, 108, Align::MiddleCenter, fg, bg);
                else text::drawFitted(spr, now, CENTRE, CENTRE, 200, 120, Align::MiddleCenter, fg, bg);
                return;
            }
            const int size = showCity ? (now.length() > 4 ? 62 : 74) : (now.length() > 4 ? 70 : 88);
            text::draw(spr, now, CENTRE, showCity ? 96 : 90, size, Align::MiddleCenter, fg, bg);
            const uint16_t high = colorOr(p, "highColor", fg), low = colorOr(p, "lowColor", fg);
            text::draw(spr, "High", 80, 170, 22, Align::MiddleCenter, high, bg);
            text::draw(spr, degreeText(w.high), 80, 210, 22, Align::MiddleCenter, high, bg);
            text::draw(spr, "Low", 160, 170, 22, Align::MiddleCenter, low, bg);
            text::draw(spr, degreeText(w.low), 160, 210, 22, Align::MiddleCenter, low, bg);
        }
    }

    int WeatherControl::parse(JsonObjectConst in, JsonObject out, String &error) const {
        if (!in["element"].is<const char *>()) {
            error = "control 'weather' requires params.element ('icon'|'temperature'|'condition')";
            return 400;
        }
        if (optionalChoice(in, out, "element", ELEMENTS, error)) {
            error = "unknown weather element '" + in["element"].as<String>() + "'";
            return 400;
        }
        // location is reported only when set.
        if (!in["location"].isNull() && !(in["location"].is<const char *>() && strlen(in["location"]) == 0)) {
            if (int status = optionalString(in, out, "location", "", 64, error)) return status;
        }
        if (strcmp(out["element"], "temperature") == 0) {
            int status;
            if ((status = optionalBool(in, out, "showCity", !out["location"].isNull(), error))) return status;
            if ((status = optionalBool(in, out, "showHighLow", true, error))) return status;
        }
        for (const char *key : {"color", "background", "highColor", "lowColor", "cityColor"}) {
            if (int status = colorIfSet(in, out, key, error)) return status;
        }
        return 0;
    }

    void WeatherControl::draw(LGFX_Sprite &spr, JsonObjectConst p, const DrawContext &) {
        const uint16_t fg = colorOr(p, "color", TFT_WHITE), bg = colorOr(p, "background", TFT_BLACK);
        String location = p["location"] | "";
        if (location.isEmpty()) location = settings::get().weatherLocation;

        feeds::Weather w;
        if (location.isEmpty()) {
            w.problem = "Set a weather location";
        } else {
            w = feeds::weather(location);
        }
        if (!w.valid) {
            // Nothing to show yet: say why, quietly.
            spr.fillScreen(bg);
            text::draw(spr, w.problem.length() ? w.problem : String("Loading weather"), CENTRE, CENTRE, 12, Align::MiddleCenter, 0x7BEF, bg);
            return;
        }

        const char *element = p["element"];
        if (strcmp(element, "icon") == 0) drawIcon(spr, w.icon);
        else if (strcmp(element, "condition") == 0) drawCondition(spr, w, fg, bg, colorOr(p, "cityColor", fg));
        else drawTemperature(spr, p, w, fg, bg);
    }

}
