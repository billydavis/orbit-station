#include "TimeKeeper.h"
#include "PosixTz.h"
#include <esp_sntp.h>

extern const char zonesCsv[] asm("_binary_assets_zones_csv_start");

namespace timekeeper {

    namespace {
        bool syncStarted = false;

        // Looks name up in zones.csv (lines of "Area/City","POSIX rule").
        String lookup(const String &name) {
            const String needle = "\"" + name + "\",\"";
            const char *hit = strstr(zonesCsv, needle.c_str());
            if (!hit) return "";
            const char *start = hit + needle.length();
            const char *end = strchr(start, '"');
            return end ? String(start).substring(0, end - start) : String();
        }

        // A POSIX rule given directly, like "CST6CDT,M3.2.0,M11.1.0" or "UTC0":
        // starts with a 3+ letter (or <+05>-style) name followed by an offset.
        bool looksPosix(const String &tz) {
            if (tz.isEmpty() || tz.indexOf('/') >= 0) return false;
            int i = 0;
            if (tz[0] == '<') {
                i = tz.indexOf('>');
                if (i < 0) return false;
                i++;
            } else {
                while (i < (int) tz.length() && isalpha(tz[i])) i++;
                if (i < 3) return false;
            }
            return i < (int) tz.length() && (isdigit(tz[i]) || tz[i] == '+' || tz[i] == '-');
        }

        String toPosix(const String &tz) {
            if (tz == "UTC" || tz == "Etc/UTC") return "UTC0";
            String rule = lookup(tz);
            if (rule.length()) return rule;
            return looksPosix(tz) ? tz : String();
        }
    }

    void startSync() {
        if (syncStarted) return;
        syncStarted = true;
        // configTzTime would reset TZ; keep the one setTimezone chose.
        const String tz = getenv("TZ") ? getenv("TZ") : "UTC0";
        configTzTime(tz.c_str(), "pool.ntp.org", "time.google.com");
        Serial.println("Time: syncing from the internet");
    }

    bool isSet() {
        return time(nullptr) > 1700000000; // after Nov 2023: set from the internet, not the 1970 default
    }

    bool now(struct tm &out) {
        if (!isSet()) return false;
        time_t t = time(nullptr);
        localtime_r(&t, &out);
        return true;
    }

    bool validTimezone(const String &tz) { return toPosix(tz).length() > 0; }

    // ---- other zones ----
    namespace {
        // The last few zones asked for, so a clock drawing 10 times a second
        // doesn't search the table each time. Only the render task asks.
        struct Cached {
            String name;
            posixtz::Rule rule;
            bool ok = false;
        };
        Cached cache[4];
        int nextSlot = 0;

        const Cached &ruleFor(const String &tz) {
            for (const Cached &c : cache)
                if (c.name.length() && c.name == tz) return c;
            Cached &c = cache[nextSlot];
            nextSlot = (nextSlot + 1) % 4;
            c.name = tz;
            const String rule = toPosix(tz);
            c.ok = rule.length() && posixtz::parse(rule.c_str(), c.rule);
            return c;
        }
    }

    bool localIn(const String &tz, time_t t, struct tm &out) {
        const Cached &c = ruleFor(tz);
        if (!c.ok) return false;
        const int32_t offset = posixtz::offsetAt(c.rule, t);
        const time_t local = t - offset;
        gmtime_r(&local, &out);
        out.tm_isdst = c.rule.dst && offset != c.rule.stdOffset;
        return true;
    }

    bool nowIn(const String &tz, struct tm &out, uint32_t *ms) {
        if (!isSet()) return false;
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        if (ms) *ms = tv.tv_usec / 1000;
        return localIn(tz, tv.tv_sec, out);
    }

    void setTimezone(const String &tz) {
        String rule = toPosix(tz);
        if (rule.isEmpty()) rule = "UTC0";
        setenv("TZ", rule.c_str(), 1);
        tzset();
        Serial.printf("Time: timezone %s (%s)\n", tz.c_str(), rule.c_str());
    }

}
