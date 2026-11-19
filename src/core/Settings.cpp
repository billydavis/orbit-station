#include "Settings.h"
#include "TimeKeeper.h"
#include <Preferences.h>

namespace settings {

    namespace {
        constexpr const char *NS = "station";
        Values current;
        SemaphoreHandle_t writeLock; // serialises updates (the API can be called while another runs)

        // Shows enough of a key to tell which one is set, without giving it away.
        String masked(const String &key) {
            if (key.isEmpty()) return "";
            return key.length() <= 4 ? String("****") : "****" + key.substring(key.length() - 4);
        }

        bool saveString(Preferences &prefs, const char *key, const String &value) {
            // putString returns the bytes written; 0 for an empty string is fine.
            return prefs.putString(key, value) == value.length();
        }
    }

    void begin() {
        writeLock = xSemaphoreCreateMutex();
        Preferences prefs;
        prefs.begin(NS, true);
        Values v;
        v.timezone = prefs.getString("tz", "UTC");
        v.metric = prefs.getBool("metric", false);
        v.weatherApiKey = prefs.getString("weatherKey", "");
        v.tickerApiKey = prefs.getString("tickerKey", "");
        prefs.end();
        current = v;
        timekeeper::setTimezone(v.timezone);
    }

    Values get() {
        xSemaphoreTake(writeLock, portMAX_DELAY);
        Values v = current;
        xSemaphoreGive(writeLock);
        return v;
    }

    int update(JsonObjectConst req, String &error, bool &saved) {
        saved = true;
        xSemaphoreTake(writeLock, portMAX_DELAY);
        Values next = current;

        // Check everything before changing anything.
        JsonVariantConst tz = req["timezone"];
        if (!tz.isNull()) {
            if (!tz.is<const char *>() || !timekeeper::validTimezone(tz.as<String>())) {
                xSemaphoreGive(writeLock);
                error = "unknown timezone; use a name like \"America/Chicago\" or a POSIX rule like \"CST6CDT,M3.2.0,M11.1.0\"";
                return 400;
            }
            next.timezone = tz.as<String>();
        }
        JsonVariantConst units = req["units"];
        if (!units.isNull()) {
            const String u = units.as<String>();
            if (!units.is<const char *>() || (u != "metric" && u != "imperial")) {
                xSemaphoreGive(writeLock);
                error = "units must be \"metric\" or \"imperial\"";
                return 400;
            }
            next.metric = u == "metric";
        }
        const char *keyFields[] = {"weatherApiKey", "tickerApiKey"};
        String *keyValues[] = {&next.weatherApiKey, &next.tickerApiKey};
        for (int i = 0; i < 2; i++) {
            JsonVariantConst k = req[keyFields[i]];
            if (k.isNull()) continue;
            if (!k.is<const char *>() || k.as<String>().length() > 128) {
                xSemaphoreGive(writeLock);
                error = String(keyFields[i]) + " must be a string (\"\" to clear it)";
                return 400;
            }
            *keyValues[i] = k.as<String>();
        }

        // Write only what changed.
        Preferences prefs;
        prefs.begin(NS, false);
        if (next.timezone != current.timezone) {
            saved &= saveString(prefs, "tz", next.timezone);
            timekeeper::setTimezone(next.timezone);
        }
        if (next.metric != current.metric) saved &= prefs.putBool("metric", next.metric) > 0;
        if (next.weatherApiKey != current.weatherApiKey) saved &= saveString(prefs, "weatherKey", next.weatherApiKey);
        if (next.tickerApiKey != current.tickerApiKey) saved &= saveString(prefs, "tickerKey", next.tickerApiKey);
        prefs.end();

        current = next;
        xSemaphoreGive(writeLock);
        return 0;
    }

    void toJson(JsonObject out) {
        Values v = get();
        out["timezone"] = v.timezone;
        out["units"] = v.metric ? "metric" : "imperial";
        out["weatherApiKey"] = masked(v.weatherApiKey);
        out["tickerApiKey"] = masked(v.tickerApiKey);
        out["timeSet"] = timekeeper::isSet();
    }

}
