# Orbit Station plan

New firmware built around OrbIt alone. It drives five round GC9A01 screens from an
ESP32-S3 (N16R8) and is controlled only through the orbit-api; there are no buttons.
It is spun off from [info-orbs](https://github.com/brettdottech/info-orbs), so it is
AGPL-3.0.

## What carries over from info-orbs

- **orbit-api v1**, unchanged, so existing clients keep working.
- **The control designs:** time, analogClock, gauge, sysMonitor, weather, ticker,
  custom, countdown, screensaver, blank.
- **The persistence rule:** settings are saved, live data never goes to flash.
  Layout goes in NVS (about 8 KB spare), large payloads (drawings, images) go to
  LittleFS files, and a file is written only when it changes.

## What doesn't carry over

- The widget system, buttons, and `config.h` feature switches. 16 MB of flash
  holds every control.
- TFT_eSPI and `ScreenManager`. LovyanGFX replaces them; it's proven on the bench.
- Makapix memory limits (128 px cap, first frame only).

## Hardware during development

Breadboard rig: Lonely Binary ESP32-S3 N16R8 on the screw terminal base, five
screens on about 20 cm cables, screens powered from their own AMS1117 3.3 V
module fed from 5 V. Serial goes over native USB (`ARDUINO_USB_CDC_ON_BOOT=1`).

| Signal | GPIO |
|---|---|
| SCLK | 12 |
| MOSI | 11 |
| DC | 13 |
| RST (shared by all screens) | 14 |
| CS, screens 1 to 5 | 10, 9, 8, 17, 16 |
| Sense (not wired yet) | 42, 41, 40, 39, 38 |

SPI runs at 40 MHz; 80 MHz fails on the breadboard.

## Display design

Each screen gets its own 240×240 picture (sprite) in PSRAM. A control draws its
whole picture there. A sender on the other core finds which 16×16 tiles changed
and sends only those. This is the bench's mode 5 approach, which reached about
37 frames/s per screen on all five.

Lessons from the bench that must be built in:

1. With manual CS, call `lcd.waitDMA()` before switching CS. `waitDisplay()` does
   nothing for LCD panels.
2. Never call `lcd.init()` a second time; it restarts the SPI bus. To re-init,
   pulse RST, wait 150 ms, resend the panel's init commands (through a
   `Panel_GC9A01` subclass), then set inversion and `setRotation(0)`.
3. After any reset or re-init, send one whole frame before changed-tiles-only
   updates.
4. Don't set both `cfg.invert` and `invertDisplay(true)`; they cancel out.
5. Don't DMA whole frames straight from PSRAM while the other core draws into
   PSRAM. Copy them through an internal-RAM DMA buffer in 20-line bands.
6. A long-running task must yield (`vTaskDelay(1)`) at least every ~100 ms.
   On core 0 the idle task starves and the watchdog reboots the board; on
   core 1 a busy sender at a higher priority than `loop()` silenced serial
   completely (found in phase 1).
7. LovyanGFX remembers the last drawing window it sent and skips re-sending
   it. Each screen has its own window, so forget it on every CS switch
   (`PanelGC9A01::forgetWindow()`), or partial updates land in the wrong place.

## Phase 1 results

Tile diff with a fingerprint per 16×16 tile (no second copy of each screen):
about 26 frames/s per screen on all five with the test animation redrawing
whole pictures, about 26% of each frame sent. Comparing two full copies of
each picture managed only 19. Paused (nothing changes, nothing sent) it
reaches 32, the limit of redrawing five whole pictures on core 0. The bench's
37 came from redrawing and sending only the changed boxes without comparing.
If a control ever needs more, let it pass the areas it changed to `endFrame()`.

Re-init one screen at a time through its own CS, without the shared RST, so the
others don't blank.

## Phases

Each phase ends with a check on the rig before the next one starts.

| # | Phase | Done when |
|---|---|---|
| 0 | **Skeleton.** Repo, PlatformIO set up for the N16R8, pin map, boot log | It builds, boots and logs over USB |
| 1 | **Display core.** Five-screen driver with the bench lessons, PSRAM pictures, tile-diff sender on the other core, per-screen re-init | A test animation matches the bench speed; re-init and replugging are clean |
| 2 | **WiFi, setup and recovery.** Setup hotspot when no WiFi is saved, `orbit.local`, updates over WiFi | Set up from a phone; update over WiFi without USB |
| 3 | **API and saving.** orbit-api v1 endpoints, layout in NVS, `blank` and `time` controls, settings over the API (timezone, API keys) | Screens set over the API survive a restart; value-only updates never write flash |
| 4 | **Port the controls** in batches: clocks and gauge; custom, sysMonitor, countdown; screensaver, weather, ticker | Each batch checked on the screens |
| 5 | **Images and animation.** Makapix-style control: fetch or upload, decode PNG, GIF and WebP into PSRAM, play animations | Full-size animations play smoothly |
| 6 | **Hardening.** Watchdog, memory monitoring, periodic screen re-init, error reporting | A day-long run stays clean |

## Phase 2 results

- **Setup:** with no saved WiFi the board opens the hotspot `OrbitStation-xx`
  (last MAC byte); WiFiManager's page saves the network. If the saved network
  can't be reached for 3 minutes the hotspot reopens for 5, then it tries again.
- **Name:** `orbit-station-xx.local`, advertised as `_http._tcp` (Command Center
  also needs the `orbit` TXT record, added with the API in phase 3).
- **Updates over WiFi:** `pio run -e s3-ota -t upload --upload-port <name or IP>`.
  The board connects back to the PC on TCP 47269, so the PC firewall must allow it.
- **Factory reset:** three power-ups in a row, each under 10 s. Only power-on
  resets count, so crashes, brownouts and flasher resets can't trigger it.
- **Mesh networks:** one eero node refused the ESP32-S3 every time (reason 2)
  while another accepted it. After 4 refusals the board scans for every access
  point with the saved name and tries them strongest first; the one that works
  is saved with the network, so restarts go straight to it.
- **Power:** WiFi's current bursts need a solid 3.3 V. Never tie the board's
  3V3 to another regulator's output; screens get their own supply. One board's
  regulator was damaged on the bench (runs fine with WiFi off, browns out with
  it on). The PCB needs bulk capacitance (~470–1000 µF) on its 5 V input.

## Phase 3 results

- **orbit-api v1** at `/orbit/api/v1`: `GET`/`POST /screens`, `GET`/`POST
  /screens/{n}`, `GET /capabilities`, with info-orbs' shapes, status codes and
  error messages. Controls orbit-api defines but this build doesn't have yet
  answer 501, not 400. A bulk write with a bad entry changes nothing.
- **Board-wide:** `GET /api/v1/system` and `GET /api/v1/capabilities` (what
  Command Center reads; OrbIt is the one "widget"), and `GET`/`POST
  /api/v1/settings` for `timezone`, `units` and the weather and ticker API
  keys (reported masked). The `_http._tcp` mDNS service carries
  `orbit=/orbit/api/v1/screens` for discovery.
- **Controls are classes** (`orbit/Control.h`): `parse` checks and normalises
  params, `draw` paints the whole picture, `stripLive` removes live readings
  before saving, `refreshMs` says how often it changes by itself. A render
  task on core 0 draws each screen when its config changes or its refresh
  time comes round; the display core sends only what changed.
- **Saving:** the layout is one JSON blob in NVS (`orbit`/`layout`), written
  only when its saved form changes; checked on the board (identical writes
  don't touch flash, a change writes once, a restart restores everything).
- **Time:** SNTP once online, timezone by IANA name through `assets/zones.csv`
  built into the firmware (or a POSIX rule given directly); no API key.
- **Text:** Roboto through OpenFontRender, drawn into each screen's picture,
  with info-orbs' sizes and scaling, so ported controls look the same.
- **Screen 1** shows the network status while the board isn't online and
  for 15 s after it comes online; otherwise its own control.

## Phase 4 results

- **Batch 1:** `analogClock`, `gauge` (value never saved). Shared param
  helpers in `orbit/Control.cpp` (colours as RGB565 or info-orbs' names).
- **Batch 2:** `sysMonitor` (all readings live, only `center` saved),
  `countdown` (commands checked against the screen's state; never saved: the
  screen is saved, and restored on `stop`, as what it showed before) and
  `custom` (WebDataWidget drawings; saved to `/orbit-draw{n}.json` on LittleFS
  only when the drawing changes; overlay frames never touch flash). Each
  custom screen keeps its accumulated picture in PSRAM so overlays build on
  it; the display core's own pictures are always a frame behind.
- The control interface grew optional hooks for this: `parseWrite` (sees
  the screen's current state), `applied`, `report` (live state in GET),
  `saved()` and `savedInFile()`.
- **Batch 3:** `screensaver` (asteroids, matrix, warp, orrery, radar, cycle;
  the old `asteroids` control is an alias), `weather` (Visual Crossing, a
  location per screen, dark mode only) and `ticker` (twelvedata, 5-minute
  floor), plus `POST /screens/{n}/refresh` and `POST /weather/preview`. API
  keys and the default weather location come from `/api/v1/settings`; no key
  is built into the firmware. Fetching runs in its own task (`data/Feeds`)
  and only for what a screen shows; readings are never saved.
- **Speed:** with all five effects on screen, about 19.5 frames/s each
  (target 20); every effect draws in 5-8 ms. The render loop keeps each
  screen on a steady cadence and sleeps only until the next one is due (a
  fixed 20 ms sleep had capped everything near 15 fps). The radar's 3°
  afterglow slices are triangles: as pie-slice arcs they took 55-72 ms.

## Phase 5 results

- **`makapix` control:** pixel art from Makapix Club through its HTTPS
  player API (fabkury/makapix `docs/http-api/player-rpc.md`). Params:
  `channel`, `user`, `hashtag`, `sort` (random by default), `changeSeconds`.
  Screens on one feed each get a different artwork; views are reported (at
  most one per 5 s).
- **Pairing without keys:** the board provisions itself, shows the
  registration code on the screen, polls until it's registered, then keeps
  its player key and token in NVS (masked in `GET /api/v1/makapix`;
  `POST /api/v1/makapix/forget` starts over). A rejected token is renewed
  with the player key. Provisioning only starts once a screen shows Makapix.
- **Decoding** (`makapix/Artwork`): PNG (PNGdec), GIF with disposal
  (AnimatedGIF), still and animated WebP (libwebp vendored from the
  info-orbs Makapix branch, decode and demux only) and BMP, into RGB565
  frames in PSRAM at the art's own size; transparency on black. 1 MB budget
  per artwork; posts over it are skipped before download, using the size and
  frame count Makapix reports. Checked with stills from 16x16 to 64x64 and
  animations up to 91 frames (64x64) and 32 frames (128x128).
- **Playback:** nearest-neighbour, `fit` per screen: `crisp` (default, the
  largest whole multiple, centred; 256x256 shrinks to 240) or `fill` (covers
  the whole screen, long edges trimmed, pixels may be slightly uneven). Each
  screen redraws at its art's frame rate.
- **Internal RAM:** HTTPS needs about 17 KB of internal RAM in one piece per
  connection. Small allocations had fragmented it until TLS failed with 80 KB
  nominally free, so allocations above 256 bytes now go to PSRAM
  (`heap_caps_malloc_extmem_enable`), and HTTPS connections take turns
  (`net::HttpsTurn`). Free internal RAM went from a 19.7 KB low point to
  108 KB; `/api/v1/system` reports `largestFreeBlock`.

## Open decisions

- None right now.
