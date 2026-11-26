"""Regenerates assets/zones.csv from the newest IANA tz database.

Each zone's POSIX rule is the footer of its TZif file, as posix_tz_db makes
it. A footer only holds after the zone's last listed change, so a zone whose
rules have a one-off exception (Havana skipping a fall-back, say) can be
wrong until that's passed; the check below lists any that are.

    python -m pip install -U tzdata
    python scripts/zones_refresh.py            # writes assets/zones.csv, then checks it

Keeps every name already in the table, and adds tzdata's other Area/City
names (aliases too, so an old name a client sends still works).
"""
import csv, datetime as dt, importlib.resources as res, re, time, zoneinfo
from pathlib import Path

import tzdata

CSV = Path(__file__).resolve().parent.parent / "assets" / "zones.csv"
LEGACY = ("posix/", "right/", "SystemV/", "US/", "Canada/", "Mexico/", "Brazil/", "Chile/")


def footer(name):
    data = res.files("tzdata.zoneinfo").joinpath(*name.split("/")).read_bytes()
    assert data[:4] == b"TZif" and data[4] >= ord("2"), name
    return data.rstrip(b"\n").rsplit(b"\n", 1)[1].decode()


# A small POSIX rule evaluator for the check (the board's is core/PosixTz.h).
def offset_name(s, i):
    if s[i] == "<":
        i = s.index(">", i) + 1
    else:
        while i < len(s) and s[i].isalpha():
            i += 1
    m = re.match(r"[+-]?\d+(:\d+)?(:\d+)?", s[i:])
    if not m:
        return None, i
    sign = -1 if m.group().startswith("-") else 1
    parts = [int(p) for p in m.group().lstrip("+-").split(":")] + [0, 0]
    return sign * (parts[0] * 3600 + parts[1] * 60 + parts[2]), i + m.end()


def when(spec, year):
    date, _, at = spec.partition("/")
    secs = 7200
    if at:
        sign = -1 if at.startswith("-") else 1
        p = [int(x) for x in at.lstrip("+-").split(":")] + [0, 0]
        secs = sign * (p[0] * 3600 + p[1] * 60 + p[2])
    if date.startswith("M"):
        m, w, d = (int(x) for x in date[1:].split("."))
        first = dt.date(year, m, 1)
        day = first + dt.timedelta(days=(d - (first.weekday() + 1) % 7) % 7 + 7 * (w - 1))
        while day.month != m:
            day -= dt.timedelta(days=7)
    elif date.startswith("J"):
        n = int(date[1:])
        day = dt.date(year, 1, 1) + dt.timedelta(days=n - 1 + (1 if n >= 60 and year % 4 == 0 and (year % 100 or year % 400 == 0) else 0))
    else:
        day = dt.date(year, 1, 1) + dt.timedelta(days=int(date))
    return dt.datetime(day.year, day.month, day.day) + dt.timedelta(seconds=secs)


EPOCH = dt.datetime(1970, 1, 1)
_years = {}


def year_rule(rule, year):
    """(std, dst, summer start, summer end) for one year, times as Unix seconds."""
    key = (rule, year)
    if key not in _years:
        std, i = offset_name(rule, 0)
        if i >= len(rule):
            _years[key] = (std, None, 0, 0)
        else:
            dst, _ = offset_name(rule, i)
            dst = std - 3600 if dst is None else dst
            start, end = rule[rule.index(",") + 1:].split(",")
            s = (when(start, year) - EPOCH).total_seconds() + std  # local standard time -> UTC
            e = (when(end, year) - EPOCH).total_seconds() + dst    # local summer time -> UTC
            _years[key] = (std, dst, s, e)
    return _years[key]


def east_offset(rule, t):
    std, dst, s, e = year_rule(rule, time.gmtime(t).tm_year)
    if dst is None:
        return -std
    summer = s <= t < e if s < e else not (e <= t < s)
    return -(dst if summer else std)


def main():
    old = {r[0]: r[1] for r in csv.reader(open(CSV, newline=""))} if CSV.exists() else {}
    avail = zoneinfo.available_timezones()
    names = {n for n in avail if "/" in n and not n.startswith(LEGACY)} | {n for n in old if n in avail}
    new = {n: footer(n) for n in names}
    for n in old:
        new.setdefault(n, old[n])  # a name tzdata has dropped keeps its old rule
    with open(CSV, "w", newline="") as f:
        for n in sorted(new):
            f.write(f'"{n}","{new[n]}"\n')
    changed = sorted(n for n in old if new[n] != old[n])
    print(f"tzdata {tzdata.IANA_VERSION}: {len(new)} zones, {len(set(new) - set(old))} added, {len(changed)} changed")
    for n in changed:
        print(f"  {n}: {old[n]} -> {new[n]}")

    # Check every zone against zoneinfo, every 15 minutes for two years from now.
    now = int(time.time()) // 900 * 900
    wrong = []
    for n in sorted(new):
        if n not in avail:
            continue
        z = zoneinfo.ZoneInfo(n)
        bad = [t for t in range(now, now + 2 * 366 * 86400, 900)
               if east_offset(new[n], t) != dt.datetime.fromtimestamp(t, z).utcoffset().total_seconds()]
        if bad:
            wrong.append((n, len(bad) / 4, dt.datetime.fromtimestamp(bad[0], dt.timezone.utc), dt.datetime.fromtimestamp(bad[-1], dt.timezone.utc)))
    print(f"check, next two years: {len(wrong)} zones differ from tzdata")
    for n, hours, a, b in wrong:
        print(f"  {n}: {hours:.0f} h wrong, {a:%Y-%m-%d} to {b:%Y-%m-%d}")


main()
