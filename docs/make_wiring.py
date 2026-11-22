"""Generates docs/wiring.svg, the breadboard wiring diagram in the README.

Run from the repo root:  python docs/make_wiring.py
Edit the pin map or layout here rather than the SVG.
"""
from pathlib import Path

W, H = 980, 900
FONT = "Segoe UI, Helvetica, Arial, sans-serif"

# Colours (also used in the key)
C5V, C33, CGND, CCS = "#E67E22", "#D0312D", "#222222", "#2E9E3E"
# One colour per shared signal, so each bus can be followed on its own
CSIG = {"SCL": "#2563EB", "SDA": "#9333EA", "DC": "#0891B2", "RST": "#8B5E34"}

# Board: shared signals and per-screen chip selects (pin name, GPIO)
SHARED = [("SCLK", 12, "SCL"), ("MOSI", 11, "SDA"), ("DC", 13, "DC"), ("RST", 14, "RST")]
CS = [10, 9, 8, 17, 16]

# Screen boxes and their pin rows (order as on the GC9A01 modules)
PINS = ["VCC", "GND", "SCL", "SDA", "DC", "CS", "RST"]
SX, SW, SH = 720, 220, 116
TOP0, PITCH, ROW0, ROWGAP = 60, 128, 18, 14
tops = [TOP0 + PITCH * i for i in range(5)]


def row(i, pin):
    return tops[i] + ROW0 + ROWGAP * PINS.index(pin)


# Vertical runs: rails and buses
X_33, X_GND = 640, 662
X_BUS = {"SCL": 540, "SDA": 562, "DC": 584, "RST": 606}
X_CS = [410 + 22 * i for i in range(5)]

# Board
BX, BY, BW, BH = 40, 80, 250, 530
board_y = {"SCLK": 150, "MOSI": 176, "DC": 202, "RST": 228}
cs_y = [276 + 26 * i for i in range(5)]
B_5V, B_GND, B_3V3 = 150, 215, 262  # x of the bottom-edge pins

out = []
a = out.append
a(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" font-family="{FONT}">')
a("<title>Orbit Station breadboard wiring</title>")
a("<desc>An ESP32-S3 N16R8 board drives five GC9A01 round screens. SCLK, MOSI, DC and RST are shared by all five "
  "screens; each screen has its own CS line. The screens are powered from an AMS1117 3.3 V module fed from the "
  "board's 5 V pin; the board's own 3V3 pin is not connected.</desc>")
a(f'<defs><marker id="arrow" viewBox="0 0 10 10" refX="8" refY="5" markerWidth="7" markerHeight="7" orient="auto">'
  f'<path d="M1 1L9 5L1 9z" fill="{C5V}"/></marker></defs>')
a(f'<rect width="{W}" height="{H}" fill="#FFFFFF"/>')

# Supply
a(f'<text x="{BX}" y="38" font-size="13" fill="#333">5 V USB-C supply (2 A or more) into the board\'s USB socket: power and data</text>')
a(f'<line x1="100" y1="46" x2="100" y2="{BY - 2}" stroke="{C5V}" stroke-width="2.5" marker-end="url(#arrow)"/>')

# Board box and pin labels
a(f'<rect x="{BX}" y="{BY}" width="{BW}" height="{BH}" rx="10" fill="#E8F4F1" stroke="#1D7A64" stroke-width="1.5"/>')
a(f'<text x="{BX + 16}" y="{BY + 28}" font-size="16" font-weight="600" fill="#0E4D3F">ESP32-S3 N16R8</text>')
a(f'<text x="{BX + 16}" y="{BY + 46}" font-size="12" fill="#2C6B5C">Lonely Binary, on its screw base</text>')
right = BX + BW - 8
for name, gpio, _ in SHARED:
    a(f'<text x="{right}" y="{board_y[name] + 4}" font-size="12" fill="#163E35" text-anchor="end">GPIO{gpio} · {name}</text>')
for i, gpio in enumerate(CS):
    a(f'<text x="{right}" y="{cs_y[i] + 4}" font-size="12" fill="#163E35" text-anchor="end">GPIO{gpio} · CS {i + 1}</text>')
bottom = BY + BH
for x, label, colour in [(B_5V, "5V", "#163E35"), (B_GND, "GND", "#163E35"), (B_3V3, "3V3", "#A33")]:
    a(f'<text x="{x}" y="{bottom - 10}" font-size="12" fill="{colour}" text-anchor="middle">{label}</text>')
a(f'<text x="{B_3V3}" y="{bottom + 16}" font-size="11" fill="#A33" text-anchor="middle">not used</text>')

BOARD_RIGHT = BX + BW

# Shared buses: board pin to its vertical run, then a stub to every screen
for name, _, pin in SHARED:
    x, y, c = X_BUS[pin], board_y[name], CSIG[pin]
    top, low = min(y, row(0, pin)), row(4, pin)
    a(f'<path d="M{BOARD_RIGHT} {y}H{x}M{x} {top}V{low}" stroke="{c}" stroke-width="2" fill="none"/>')
    a(f'<circle cx="{x}" cy="{y}" r="3.5" fill="{c}"/>')
    # Name at the top of each bus, like the rails
    a(f'<text x="{x}" y="{TOP0 - 6}" font-size="10" font-weight="600" fill="{c}" text-anchor="middle">{pin}</text>')
    for i in range(5):
        a(f'<path d="M{x} {row(i, pin)}H{SX}" stroke="{c}" stroke-width="1.6" fill="none"/>')
        a(f'<circle cx="{x}" cy="{row(i, pin)}" r="3" fill="{c}"/>')

# Chip selects: one line per screen
for i in range(5):
    a(f'<path d="M{BOARD_RIGHT} {cs_y[i]}H{X_CS[i]}V{row(i, "CS")}H{SX}" stroke="{CCS}" stroke-width="2" fill="none"/>')

# Rails
rail_low_33, rail_low_gnd = 790, 815
a(f'<path d="M{X_33} {row(0, "VCC")}V{rail_low_33}" stroke="{C33}" stroke-width="2.5" fill="none"/>')
a(f'<path d="M{X_GND} {row(0, "GND")}V{rail_low_gnd}" stroke="{CGND}" stroke-width="2.5" fill="none"/>')
for i in range(5):
    a(f'<path d="M{X_33} {row(i, "VCC")}H{SX}" stroke="{C33}" stroke-width="1.6" fill="none"/>')
    a(f'<circle cx="{X_33}" cy="{row(i, "VCC")}" r="3" fill="{C33}"/>')
    a(f'<path d="M{X_GND} {row(i, "GND")}H{SX}" stroke="{CGND}" stroke-width="1.6" fill="none"/>')
    a(f'<circle cx="{X_GND}" cy="{row(i, "GND")}" r="3" fill="{CGND}"/>')
a(f'<text x="{X_33}" y="{TOP0 - 6}" font-size="11" fill="{C33}" text-anchor="middle">3.3 V</text>')
a(f'<text x="{X_GND + 8}" y="{TOP0 - 6}" font-size="11" fill="{CGND}" text-anchor="start">GND</text>')

# Screens
for i, t in enumerate(tops):
    a(f'<rect x="{SX}" y="{t}" width="{SW}" height="{SH}" rx="10" fill="#F1EFFA" stroke="#5A4FB0" stroke-width="1.5"/>')
    for pin in PINS:
        a(f'<text x="{SX + 8}" y="{row(i, pin) + 4}" font-size="11" fill="#333">{pin}</text>')
    cx = SX + 140
    a(f'<text x="{cx}" y="{t + 54}" font-size="15" font-weight="600" fill="#3C3489" text-anchor="middle">Screen {i + 1}</text>')
    a(f'<text x="{cx}" y="{t + 72}" font-size="11" fill="#534AB7" text-anchor="middle">GC9A01 1.28" 240×240</text>')

# AMS1117 module: pins VIN, OUT, GND (as printed on the module)
MX, MY, MW, MH = 300, 720, 120, 62
pin_x = {"VIN": MX + 22, "OUT": MX + 60, "GND": MX + 98}
a(f'<rect x="{MX}" y="{MY}" width="{MW}" height="{MH}" rx="6" fill="#FFF4E0" stroke="#B9770E" stroke-width="1.5"/>')
a(f'<text x="{MX + MW / 2}" y="{MY + 22}" font-size="13" font-weight="600" fill="#7A4A05" text-anchor="middle">AMS1117-3.3</text>')
for name, x in pin_x.items():
    a(f'<text x="{x}" y="{MY + MH - 10}" font-size="11" fill="#7A4A05" text-anchor="middle">{name}</text>')
mb = MY + MH
# 5 V from the board to VIN
a(f'<path d="M{B_5V} {bottom}V860H{pin_x["VIN"]}V{mb}" stroke="{C5V}" stroke-width="2.5" fill="none"/>')
a(f'<text x="{B_5V + 8}" y="852" font-size="11" fill="#B35400">5 V</text>')
# OUT to the 3.3 V rail
a(f'<path d="M{pin_x["OUT"]} {mb}V{rail_low_33}H{X_33}" stroke="{C33}" stroke-width="2.5" fill="none"/>')
a(f'<circle cx="{X_33}" cy="{rail_low_33}" r="3.5" fill="{C33}"/>')
# Module GND and board GND to the GND rail
a(f'<path d="M{pin_x["GND"]} {mb}V{rail_low_gnd}" stroke="{CGND}" stroke-width="2.5" fill="none"/>')
a(f'<path d="M{B_GND} {bottom}V{rail_low_gnd}H{X_GND}" stroke="{CGND}" stroke-width="2.5" fill="none"/>')
a(f'<circle cx="{pin_x["GND"]}" cy="{rail_low_gnd}" r="3.5" fill="{CGND}"/>')
a(f'<circle cx="{X_GND}" cy="{rail_low_gnd}" r="3.5" fill="{CGND}"/>')
# 10 µF across the rails
cy = 760
a(f'<g stroke="#555" stroke-width="2"><path d="M{X_33} {cy}H{X_33 + 8}"/><path d="M{X_33 + 8} {cy - 8}V{cy + 8}"/>'
  f'<path d="M{X_33 + 14} {cy - 8}V{cy + 8}"/><path d="M{X_33 + 14} {cy}H{X_GND}"/></g>')
a(f'<text x="{X_GND + 8}" y="{cy + 4}" font-size="11" fill="#555">10 µF</text>')

# Key
kx, ky = 720, 740
a(f'<text x="{kx}" y="{ky}" font-size="12" font-weight="600" fill="#333">Key</text>')
for j, (colour, label) in enumerate([(C5V, "5 V"), (C33, "3.3 V"), (CGND, "GND")]):
    x = kx + 86 * j
    a(f'<line x1="{x}" y1="{ky + 16}" x2="{x + 24}" y2="{ky + 16}" stroke="{colour}" stroke-width="2.5"/>')
    a(f'<text x="{x + 30}" y="{ky + 20}" font-size="12" fill="#333">{label}</text>')
for j, (pin, c) in enumerate(CSIG.items()):
    x = kx + 64 * j
    a(f'<line x1="{x}" y1="{ky + 36}" x2="{x + 24}" y2="{ky + 36}" stroke="{c}" stroke-width="2.5"/>')
    a(f'<text x="{x + 30}" y="{ky + 40}" font-size="12" fill="#333">{pin}</text>')
a(f'<line x1="{kx}" y1="{ky + 56}" x2="{kx + 24}" y2="{ky + 56}" stroke="{CCS}" stroke-width="2.5"/>')
a(f'<text x="{kx + 30}" y="{ky + 60}" font-size="12" fill="#333">CS (one per screen)</text>')
a(f'<circle cx="{kx + 12}" cy="{ky + 76}" r="3.5" fill="#333"/>')
a(f'<text x="{kx + 30}" y="{ky + 80}" font-size="12" fill="#333">joined; a crossing without a dot isn\'t</text>')
a(f'<text x="{kx}" y="{ky + 102}" font-size="12" fill="#333">100 nF across VCC–GND at each screen plug</text>')
a(f'<text x="{kx}" y="{ky + 120}" font-size="12" fill="#333">Cable colours vary: wire by the labels</text>')
a(f'<text x="{kx}" y="{ky + 138}" font-size="12" fill="#333">SPI at 40 MHz; screen cables up to 20 cm</text>')

a("</svg>")
Path(__file__).with_name("wiring.svg").write_text("\n".join(out) + "\n", encoding="utf-8")
print("wrote docs/wiring.svg")
