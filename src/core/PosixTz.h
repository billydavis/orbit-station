#pragma once

// POSIX timezone rules as zones.csv has them, worked out without the C
// library's global TZ: "STDoffset[DST[offset],start[/time],end[/time]]" with
// Mm.w.d dates, e.g. "CET-1CEST,M3.5.0,M10.5.0/3" or "<+0545>-5:45".
// Offsets are west of Greenwich (UTC = local + offset). Header-only and free
// of Arduino, so it can be tested on a PC.

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>

namespace posixtz {

    struct Change {
        int month = 1, week = 1, weekday = 0; // Mm.w.d: week 5 is the last
        int32_t at = 2 * 3600;                // seconds after local midnight (may be negative or past 24h)
    };

    struct Rule {
        int32_t stdOffset = 0, dstOffset = 0;
        bool dst = false;
        Change start, end;
    };

    namespace detail {
        inline void skipName(const char *&p) {
            if (*p == '<') {
                while (*p && *p != '>') p++;
                if (*p) p++;
            } else {
                while (isalpha((unsigned char) *p)) p++;
            }
        }

        // [+-]hh[:mm[:ss]] in seconds.
        inline bool parseTime(const char *&p, int32_t &out) {
            int sign = 1;
            if (*p == '+' || *p == '-') sign = *p++ == '-' ? -1 : 1;
            if (!isdigit((unsigned char) *p)) return false;
            int32_t v = strtol(p, (char **) &p, 10) * 3600;
            if (*p == ':') {
                p++;
                v += strtol(p, (char **) &p, 10) * 60;
                if (*p == ':') {
                    p++;
                    v += strtol(p, (char **) &p, 10);
                }
            }
            out = sign * v;
            return true;
        }

        inline bool parseChange(const char *&p, Change &c) {
            if (*p++ != 'M') return false;
            c.month = strtol(p, (char **) &p, 10);
            if (*p++ != '.') return false;
            c.week = strtol(p, (char **) &p, 10);
            if (*p++ != '.') return false;
            c.weekday = strtol(p, (char **) &p, 10);
            c.at = 2 * 3600;
            if (*p == '/') {
                p++;
                if (!parseTime(p, c.at)) return false;
            }
            return c.month >= 1 && c.month <= 12 && c.week >= 1 && c.week <= 5 && c.weekday >= 0 && c.weekday <= 6;
        }

        // Days from 1970-01-01 to year-month-day (Howard Hinnant's algorithm).
        inline int64_t daysFromCivil(int y, int m, int d) {
            y -= m <= 2;
            const int64_t era = (y >= 0 ? y : y - 399) / 400;
            const int yoe = (int) (y - era * 400);
            const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
            const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
            return era * 146097 + doe - 719468;
        }

        // The year of a day count from 1970-01-01 (the inverse, year only).
        inline int yearOf(int64_t days) {
            days += 719468;
            const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
            const int doe = (int) (days - era * 146097);
            const int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
            const int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
            const int mp = (5 * doy + 2) / 153;
            return (int) (yoe + era * 400) + (mp >= 10);
        }

        inline bool leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

        // When a change happens in year y, in UTC seconds; offset is the one
        // in force just before it.
        inline int64_t changeAt(const Change &c, int y, int32_t offset) {
            static const int DAYS[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
            const int64_t first = daysFromCivil(y, c.month, 1);
            const int firstWeekday = (int) (((first + 4) % 7 + 7) % 7); // 1970-01-01 was a Thursday
            int day = 1 + (c.weekday - firstWeekday + 7) % 7 + (c.week - 1) * 7;
            const int length = DAYS[c.month - 1] + (c.month == 2 && leap(y));
            while (day > length) day -= 7; // week 5: the last one
            return (first + day - 1) * 86400 + c.at + offset;
        }
    }

    inline bool parse(const char *text, Rule &r) {
        using namespace detail;
        r = Rule();
        const char *p = text;
        skipName(p);
        if (!parseTime(p, r.stdOffset)) return false;
        if (!*p) return true; // no daylight saving
        skipName(p);
        r.dst = true;
        r.dstOffset = r.stdOffset - 3600;
        if (*p && *p != ',' && !parseTime(p, r.dstOffset)) return false;
        if (*p++ != ',' || !parseChange(p, r.start) || *p++ != ',' || !parseChange(p, r.end)) return false;
        return *p == 0;
    }

    // The offset in force at UTC time t.
    inline int32_t offsetAt(const Rule &r, int64_t t) {
        using namespace detail;
        if (!r.dst) return r.stdOffset;
        const int64_t days = t >= 0 ? t / 86400 : (t - 86399) / 86400;
        const int y = yearOf(days);
        const int64_t start = changeAt(r.start, y, r.stdOffset), end = changeAt(r.end, y, r.dstOffset);
        const bool inDst = start < end ? (t >= start && t < end) : (t >= start || t < end); // south: across new year
        return inDst ? r.dstOffset : r.stdOffset;
    }

}
