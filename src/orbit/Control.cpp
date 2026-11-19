#include "Control.h"

namespace orbit {

    int optionalBool(JsonObjectConst in, JsonObject out, const char *key, bool fallback, String &error) {
        JsonVariantConst v = in[key];
        if (v.isNull()) {
            out[key] = fallback;
            return 0;
        }
        if (!v.is<bool>()) {
            error = String("params.") + key + " must be true or false";
            return 400;
        }
        out[key] = v.as<bool>();
        return 0;
    }

    int optionalNumber(JsonObjectConst in, JsonObject out, const char *key, float fallback, String &error) {
        JsonVariantConst v = in[key];
        if (v.isNull()) {
            out[key] = fallback;
            return 0;
        }
        if (!v.is<float>() || !isfinite(v.as<float>())) {
            error = String("params.") + key + " must be a number";
            return 400;
        }
        out[key] = v.as<float>();
        return 0;
    }

    int optionalString(JsonObjectConst in, JsonObject out, const char *key, const char *fallback, size_t maxLength, String &error) {
        JsonVariantConst v = in[key];
        if (v.isNull()) {
            out[key] = fallback;
            return 0;
        }
        if (!v.is<const char *>()) {
            error = String("params.") + key + " must be a string";
            return 400;
        }
        if (strlen(v.as<const char *>()) > maxLength) {
            error = String("params.") + key + " is too long (max " + maxLength + " characters)";
            return 400;
        }
        out[key] = v.as<const char *>();
        return 0;
    }

    int optionalChoice(JsonObjectConst in, JsonObject out, const char *key, const char *const *choices, String &error) {
        JsonVariantConst v = in[key];
        if (v.isNull()) {
            out[key] = choices[0];
            return 0;
        }
        if (v.is<const char *>()) {
            for (const char *const *c = choices; *c; c++) {
                if (strcmp(v.as<const char *>(), *c) == 0) {
                    out[key] = *c;
                    return 0;
                }
            }
        }
        error = String("unknown ") + key + " '" + v.as<String>() + "'";
        return 400;
    }

    int optionalColor(JsonObjectConst in, JsonObject out, const char *key, uint16_t fallback, String &error) {
        JsonVariantConst v = in[key];
        if (v.isNull() || (v.is<const char *>() && v.as<String>().isEmpty())) {
            out[key] = fallback;
            return 0;
        }
        if (v.is<uint16_t>()) {
            out[key] = v.as<uint16_t>();
            return 0;
        }
        if (v.is<const char *>()) {
            uint16_t named;
            if (colorFromName(v.as<String>(), named)) {
                out[key] = named;
                return 0;
            }
            error = "unknown color name '" + v.as<String>() + "' for params." + key;
            return 400;
        }
        error = String("params.") + key + " must be an RGB565 integer (0-65535) or a color name";
        return 400;
    }

    int colorIfSet(JsonObjectConst in, JsonObject out, const char *key, String &error) {
        JsonVariantConst v = in[key];
        if (v.isNull() || (v.is<const char *>() && v.as<String>().isEmpty())) return 0;
        return optionalColor(in, out, key, 0, error);
    }

    bool colorFromName(String name, uint16_t &out) {
        struct Named {
            const char *name;
            uint16_t color;
        };
        static const Named NAMES[] = {
            {"black", 0x0000}, {"navy", 0x000F}, {"darkgreen", 0x03E0}, {"darkcyan", 0x03EF}, {"maroon", 0x7800},
            {"purple", 0x780F}, {"olive", 0x7BE0}, {"lightgrey", 0xD69A}, {"grey", 0xD69A}, {"darkgrey", 0x7BEF},
            {"blue", 0x001F}, {"green", 0x07E0}, {"cyan", 0x07FF}, {"red", 0xF800}, {"magenta", 0xF81F},
            {"yellow", 0xFFE0}, {"white", 0xFFFF}, {"orange", 0xFDA0}, {"greenyellow", 0xB7E0}, {"pink", 0xFE19},
            {"brown", 0x9A60}, {"gold", 0xFEA0}, {"silver", 0xC618}, {"skyblue", 0x867D}, {"violet", 0x915C},
        };
        name.toLowerCase();
        name.replace(" ", "");
        for (const Named &n : NAMES) {
            if (name == n.name) {
                out = n.color;
                return true;
            }
        }
        return false;
    }

}
