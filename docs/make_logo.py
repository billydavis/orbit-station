"""Generates docs/logo.svg: a hub with five orbs on a tilted orbit, one orb
per screen. Same family as the OrbIt Command Center icon (dark tile, cyan
orbit, magenta accent). Run from the repo root:  python docs/make_logo.py
"""
import math
from pathlib import Path

S = 512
C = S / 2
TILE, RADIUS = "#111216", 112
ORBIT = "#22D3EE"
HUB = "#F4F4F5"
# One colour per screen, starting with Command Center's magenta.
ORBS = ["#E879F9", "#FBBF24", "#34D399", "#60A5FA", "#F87171"]

RX, RY, TILT = 196, 118, -18   # orbit ellipse and its tilt (degrees)
ORB_R, HUB_R = 30, 68
START = -40                    # where the first orb sits on the orbit (degrees)


def on_orbit(deg):
    t, a = math.radians(deg), math.radians(TILT)
    x, y = RX * math.cos(t), RY * math.sin(t)
    return C + x * math.cos(a) - y * math.sin(a), C + x * math.sin(a) + y * math.cos(a)


out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{S}" height="{S}" viewBox="0 0 {S} {S}">',
       "<title>Orbit Station</title>",
       f'<rect width="{S}" height="{S}" rx="{RADIUS}" fill="{TILE}"/>',
       f'<ellipse cx="{C}" cy="{C}" rx="{RX}" ry="{RY}" transform="rotate({TILT} {C} {C})" '
       f'fill="none" stroke="{ORBIT}" stroke-width="16"/>',
       f'<circle cx="{C}" cy="{C}" r="{HUB_R}" fill="{HUB}"/>']
for i, colour in enumerate(ORBS):
    x, y = on_orbit(START + 72 * i)
    # A ring of tile colour around each orb parts it from the orbit line.
    out.append(f'<circle cx="{x:.1f}" cy="{y:.1f}" r="{ORB_R}" fill="{colour}" stroke="{TILE}" stroke-width="9"/>')
out.append("</svg>")
Path(__file__).with_name("logo.svg").write_text("\n".join(out) + "\n", encoding="utf-8")
print("wrote docs/logo.svg")
