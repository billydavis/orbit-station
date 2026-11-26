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

## Phase 6 results

- **Watchdog** (`core/Health`): the display sender, renderer, feeds and
  Makapix tasks beat as they work (deadlines 10 s, 10 s, 120 s, 180 s; the
  network tasks block on long downloads). A silent task makes the board
  record why and restart. The Arduino loop is on the hardware watchdog,
  paused during updates over WiFi. The reason survives the restart (RTC
  memory) and shows in `/api/v1/system` (`lastRestart`,
  `lastRestartDetail`). Checked with serial `z`, which freezes the renderer:
  restart after 10 s, reason kept.
- **Memory watch:** a largest free internal block under 20 KB (too small for
  HTTPS) for 3 minutes restarts the board; a summary is logged every 15
  minutes; `lowestLargestFreeBlock` is reported.
- **Event log:** the last 48 events with times at `GET /api/v1/log`.
- **Quiet screen refresh:** every 30 s one screen in turn gets its setup
  re-sent (no reset; the 120 ms sleep-out wait cut to 10 ms) and a whole
  frame, requested from the renderer even for still screens. Checked: no
  visible blink; a screen whose power was unplugged and replugged came back
  by itself.
- **Soak:** 24 hours, polled from the PC every 5 minutes (results below).

### Soak results (2026-11-20 13:36 to 2026-11-21 13:37)

Third run, after fixes for what the first two found: a blocking WiFi scan that
tripped the watchdog, and a silent reconnect after a router update.

- **Reachable:** 288 of 288 checks. No WiFi drops logged; signal -29 to -35 dBm.
- **Restarts:** one crash, at 21:16 on 11-20 (`LoadProhibited`, core 0). The
  backtrace points into lwIP's DNS timer (`dns_send` to `udp_sendto_if_src`),
  which resembles arduino-esp32 issue 9388. It was decoded against a newer ELF
  than the one that crashed, so the location is **not yet confirmed**. The board
  recovered by itself and then ran for over 16 hours until the soak ended.
- **Internal memory:** free heap 106-159 KB at the checks, lowest ever 97 KB.
- **Largest free block:** 82-115 KB at the checks. The all-time low kept
  falling after the crash: 86, 82, 78, then 72 KB at 09:27, and held there for
  the last 4 hours. Still far above the 20 KB restart threshold, but one day
  can't show whether it has levelled off.
- **PSRAM:** 4.5-7.2 MB free.

Still open:

- ~~Confirm the crash location by rebuilding the exact firmware that crashed.~~
  Not possible: that ELF is gone and the build may have had uncommitted
  changes. Every build now keeps its ELF, map, image, uncommitted changes and
  tool versions in `builds/` (gitignored, newest 20), and the board reports
  `firmwareCommit` and `firmwareElfSha` (the hash a crash report's
  `ELF file SHA256` line shows), so the next crash can be decoded exactly.
  Libraries are pinned to exact versions.
- Test the Makapix outage fix with WiFi up and the internet down. The `q` WiFi
  test doesn't reach that code, because Makapix waits while WiFi is down.
- A real router reboot on the 30-second retry build.
- A longer run to see whether the largest-block low flattens out.

### After the soak

- **Transitions:** `orbit/Transition` plays a 0.8 s change between two pictures
  on a screen (13 effects adapted from Pixel Jukebox; `random`, `none` or one by
  name). A control opts in with `contentId()` and a `transition` param; only
  `makapix` does so far. The choice is a setting and is saved.
- **Art released early:** a 1 ms timing wrap made about 1 in 5 artworks drop to
  "Loading art" before their time. Fixed; in a 5-minute check every change
  waited its full interval.

### Claude usage control

Designs: `docs/mockups/claude-monitor.html`. Three styles were picked: pace,
orbit and twin tanks.

1. **Shared core and `pace`** (done, checked on the screens):
   `claudeUsage` with `style` and `animate` saved; `session`, `week`,
   `sessionResetsAt` and `weekResetsAt` live only. The board counts down to
   each reset with its own clock, and shows a window as 0% once its reset has
   passed. The logo animates for 3 minutes after a reading changes.
   Lesson: text drawn with OpenFontRender costs 5-15 ms a string, so a frame
   with all its text took ~110 ms and starved the other screens at 15 fps.
   Everything but the moving parts now goes in a cached PSRAM layer per
   screen (115 KB), repainted only when its text or bars change: a frame is
   ~10 ms, 12-14 fps while animating, and the other screens keep their rate.
2. **`orbit`** (done, checked on the screens): rings and text in the cached
   layer; the planets and the spinning logo drawn each frame. 10-12 ms a
   frame. Value-only updates don't save to NVS. The rings are drawn
   anti-aliased pixel by pixel (fillArc's edges were jagged); that costs a
   few ms, but only when the layer is repainted.
3. **`tanks`** (done, checked on the screens): the rippling
   surfaces run through the text, so the layer also keeps the text's alpha
   mask (57 KB PSRAM a screen) and each frame redraws only the rows round
   each surface, putting the text back from the mask. ~16 ms a frame.
   Bus check: three screens animating (tanks, orbit, pace) all ran at 14 fps;
   all five animating ran at 11 fps each, with the sender busy only 28% of
   the time. The limit is drawing on core 0 (about 72 ms for all five
   frames), not the SPI bus.
   Decided on the bench: no amber/red as usage climbs; session stays
   orange and week sand at any level (grey with no reading), since the
   level shows it. Only pace's verdict line still uses amber and red.
4. **Command Center** (built in OrbIt-Command-Center, tried end to end with
   the board; not yet committed there): a Claude Usage control with a style
   choice. It always follows `claude.session`, `claude.week`,
   `claude.sessionResetsAt` and `claude.weekResetsAt`: the binding is built
   into the app's Rust side (nothing to pick, nothing saved), so any screen
   showing it gets the readings, however it was set.
   `examples/claude-usage-statusline.ps1` is a Claude Code status line that
   publishes them (~0.3 s a run). Two fixes the reset times needed in the
   app: whole feed readings now go to the board as JSON integers (the
   firmware rejects `1795535998.0`), and whole readings compare exactly
   (the gauge's 1e-4 float margin hid ~2 days of change in a Unix time).
   After a board restart the app re-sent the readings ~4 s after boot.

### Engineering review follow-up (docs/engineering-review.md)

1. **Build records** (done): see the crash item under "Still open" above.
   A GitHub Actions workflow builds `env:s3` on every push to main.
2. **Makapix memory** (done, checked on the screens): five screens each
   keeping up to 1 MB of art, plus a download (then up to 4 MB) and a decode,
   could need more than the 8 MB of PSRAM, and past PSRAM allocations fall
   back to the internal RAM HTTPS needs. Downloads are now capped at twice the
   decoded budget (~2.1 MB; nothing that decodes within 1 MB is bigger), and a
   post is fetched only if 1 MB of PSRAM stays free after it; otherwise it's
   skipped, logged once, and the next one tried after 60 s while the screen
   keeps its art. `/api/v1/system` reports `minFreePsram` and
   `largestFreePsramBlock`. Checked with a 7 MB test reserve (skipped and
   logged). Ten minutes with Makapix on all five screens, art changing every
   20 s: lowest free PSRAM 2.85 MB (art and downloads took ~3.2 MB at peak),
   largest free internal block never below 62 KB, nothing skipped.
3. **Layout lock** (measured, no change needed): the longest wait for and
   hold of the lock, renderer and API apart, are in the `f` report and in
   `/api/v1/system` (`layoutLockMs`, since boot). With the usual layout
   (Makapix x2, screensaver, clock, claudeUsage tanks): 50 value-only
   updates and 30 that changed the readings, then two real layout saves.
   The API never waited more than 19 ms and held it at most 14 ms for a
   value-only update, 32 ms for one that saved to NVS (the renderer waited
   33 ms for that one). The renderer held it up to 170 ms, when claudeUsage
   repaints its cached layer after a reading changes; that stalls the other
   screens for those frames anyway, since they're drawn by the same task, so
   moving the draw out of the lock wouldn't help them. Drawing outside the
   lock would also need care, since four controls keep state that `applied()`
   (API) changes and `draw()` (renderer) reads. If the 170 ms ever matters,
   the fix is in claudeUsage's repaint, not the lock.
4. **claudeUsage layer repaint** (1 and 2 done, checked on the screens):
   a `tanks` repaint cost 110-170 ms, and the other screens froze for 2-3
   frames while it ran. It happens on every reading change and also
   **every minute**, since the countdown note is part of the layer's key.
   Timed before (9 repaints): clearing 3-6 ms; text 38-68 ms, 5-15 ms a
   string, even the fixed `SESSION` and `WEEK` every time (8 KB glyph
   cache, and `text::draw` lays each string out twice); pixel pass 59-72 ms
   (`tankPixel()` with 2-3 float `blend()`s on all 57,600 pixels).
   1. Pixel pass (done): each row is built in an internal-RAM line as plain
      runs (background, session liquid, gap, week liquid, background), then
      blended only round the logo's disc and in the text band (rows
      90-190, the only rows cleared and read back); `blend()` rounds with
      integers. The ripple shares the faster helpers.
   2. Glyph cache raised to 64 KB (done). FreeType allocates from PSRAM
      here (`FileSupport.h`), so it costs no internal RAM.
   After: a `tanks` repaint is 19-28 ms (text 6-13 ms, pixel pass ~11 ms);
   the first one after boot ~50-60 ms while the glyph cache fills. `pace`
   repaints in ~23 ms (all text). Largest free internal block unchanged
   (~84 KB).
   3. Repainting only the countdown note: not worth it at ~20 ms (one
      frame at most at 15 fps). Skipped.
   4. `orbit` rings (done): `smoothArc()` took 70-80 ms a repaint, visiting
      the whole square round each of its four arcs (~54,000 pixels) with
      `readPixel`/`drawPixel`, plus `atan2f` per pixel and `hypotf` past
      the ends. Each row now visits only the band the ring crosses (~4,000
      pixels an arc) and writes the buffer directly; a cross-product test
      replaces `atan2f` (checked on the PC against it on 5 million points:
      only exact end-ray points differ, where both distances are equal),
      and plain `sqrtf` the `hypotf`s. An `orbit` repaint is now 31-33 ms
      (text ~6 ms, rings and clearing ~25 ms); 42-45 ms while the glyph
      cache fills after boot.
   5. `pace` (measured, no change needed): with the 64 KB glyph cache warm,
      a repaint is 13-15 ms (about 10 ms of text, 8 strings, plus ~4 ms
      clearing). Measuring a cached string costs 0.1-0.3 ms; drawing it
      0.5-1.5 ms, since the cache keeps outlines and each draw rasterizes.
      The first repaint after a restart or style change is 50-112 ms while
      glyphs load from the font (up to ~10 ms a string); that hits every
      control once. Redrawing only the changed parts (~1-2 ms on the minute
      tick) or warming the cache were both judged not worth it.

### Analog clock faces (planned, not started)

Designs: `docs/mockups/analog-clock.html` (eight finished faces and a
builder). Today the clock is a 1 px circle, 12 ticks and three `drawLine`
hands, with five colour params.

**Naming:** "clock face" everywhere (UI, API, files, code), never bare
"face", so a later feature can't clash with it.

**The split.** Command Center designs clock faces; the board stores them
and draws whatever moves. Design work moves off the board, so a new dial
or hand style needs no firmware change, and the firmware stays small.

- **Board (this repo):** keeps time and turns the hands, draws the date
  window, weekday, second hand (tick or sweep) and second time zone, and
  runs with Command Center closed. It never acts on a clock face's name.
- **Command Center:** draws everything that stays still (background,
  photo, gradient, numerals in any font, ticks) into one 240x240 picture,
  and describes each hand as outline points. Clock faces are saved there
  by name, outside the board's NVS.

**Clock face file**, one per screen in LittleFS (`/orbit-clockface{n}`),
written only when it changes, like drawings:
- the dial picture as PNG (decoder already in the firmware; thin lines
  stay sharp, where JPEG would blur them);
- JSON for the moving parts: per hand (hour, minute, second, zone) a list
  of outline points in a unit system (0 = centre, 1 = face radius, up to
  ~16 points), fill colour, optional outline/lume colour; centre cap;
  second hand style; date/weekday position, font size and colours.
  About 100-150 bytes a hand, which is why it lives in the file, not NVS.

**API (proposal):**
- `PUT /orbit/api/v1/screens/{n}/clock-face`: picture plus JSON. Stored,
  then the screen's control params get `clockFaceId` and `clockFaceHash`
  (settings, saved).
- The screen's state reports the hash of the file the board actually holds,
  so Command Center can see a wiped or reflashed board and send the clock
  face again.
- Per-screen quick options stay plain params: `seconds` (on/off),
  `motion` (tick/sweep), `timeZone` / `zone2`.
- Built-in clock faces by name (`clockFace: "classic"` and two or three
  presets from the mockup), so scripts and other orbit-api clients get a
  good clock without uploading, and the board has a fallback if a file is
  missing. The old colour params keep working for `classic`.
- A capability flag, so Command Center shows the clock face picker only
  on firmware that has it (info-orbs boards keep the colour form).
- Document all of it in Command Center's `docs/orbit-api.md` before the
  app side is built.

**Board steps:**
1. Cached PSRAM layer for the still parts (as claudeUsage does), filled
   from the built-in clock face; frame = copy the layer + draw hands.
   Target 2-4 ms a frame. Replace `drawLine` hands with anti-aliased ones
   (`drawWedgeLine`, `fillSmoothCircle`). Check on the screens.
   Done (1edd089): background, ring (`smooth::arc`, now shared in
   `ui/Smooth`) and ticks in the layer; hands 2 / 1.5 / 1 px wedges. A
   frame is 8-10 ms, not 2-4 (was 5-6): copying the 115 KB layer is ~5 ms
   (PSRAM write speed with the sender busy), hands 2-3 ms. 2-4 ms needs
   restoring only the strips under the old hands, which needs the display
   to tell a control a picture is unchanged since it drew it (transitions
   and the status screen also write pictures). Only worth it for the sweep
   (step 4); at 1 fps it's under 1% of core 0. Done later; see step 4.
2. Polygon hands: rotate the outline points and fill with smooth edges
   (LovyanGFX has no anti-aliased polygon fill, so our own scanline fill
   with edge coverage). Built-in presets move to the same hand format.
   Done, checked on the screens: a hand is up to 16 outline points plus up
   to two discs (round ends, lollipop), in face-radius units pointing at
   12. `smooth::polygon` samples 16 sub-rows a pixel row with exact cover
   across each (area within 0.007% on the PC); `smooth::disc` for discs.
   `clockFace` param (setting): `classic` (the board's geometry, colour
   params), `platform` and `minimal` from the mockup, with their own
   colours. Frame: classic 8.6-10.4 ms, platform 14-16 ms (thick batons,
   8.5 px lollipop), both copy ~5 ms of that. A screen POST replaces all
   params, so a client switching clock face must send classic's colours
   too, or they reset to defaults.
3. Clock face file and the upload endpoint: store, decode the PNG into the
   layer, report the hash, fall back to the built-in clock face on a
   missing or bad file. Check value-only updates don't save to NVS.
   Done, checked on the screens; format and API in `docs/clock-faces.md`.
   JSON body with the dial as a base64 240x240 PNG (the WebServer turns a
   raw body into a String, which stops at the first zero byte), stored
   byte for byte as `/orbit-clockface{n}.json`; hash = first 16 hex digits
   of its SHA-256, so Command Center can work it out itself. Checking and
   writing happen without the layout lock (watchdog paused), then the file
   is renamed into place with it. `clockFaceId`/`clockFaceHash` params
   (settings) pick it; GET reports `clockFaceHeld`; capabilities list
   `clockFaces`. The clock has no live values, so nothing to strip.
   Tested: hash matches the PC's, nine kinds of bad file refused with the
   stored file untouched, reloads after a restart. Test faces: the
   mockup's Dress (37.5 KB body, ~18 ms a frame: four big dauphine
   triangles) and Photo over the dusk scene (13 KB). Hand shadows can't be
   see-through, so the Photo face uses thin dark outlines instead.
   Bug found on the bench: re-parsing into the live hands kept the old
   point counts and wrote past 16 points, corrupting PSRAM, so switching
   clock face crashed 1-3 s after an upload (boot was fine). Fixed by
   resetting each part and parsing into a fresh object. Once, the first
   upload after flashing the fix hit a LittleFS assert closing the new
   file; not seen again in 13 uploads over five restarts. Likely damage
   left by the corruption (LittleFS keeps its state in PSRAM); unproven.
4. Date window, weekday, second time zone, sweep (~10 fps, costs core-0
   drawing time, not bus time).
   Done, checked on the screens. Params `seconds`, `motion` (tick/sweep),
   `timeZone`, `zone2` (settings); clock face files can add `date`,
   `weekday`, `zoneLabel` and `hands.zone` (docs/clock-faces.md); minimal
   shows the weekday. Date, weekday and zone name are painted into the
   cached layer (its key holds the day and the name), so they cost nothing
   per frame; an uploaded dial is read from its file again when they change
   (once a day). Text over a picture goes through `text::drawOver` (drawn
   white on black, green channel as alpha), since OpenFontRender blends
   edges into one bg colour.
   Other zones: `core/PosixTz.h` works a POSIX rule out without the global
   TZ (other tasks read it). Checked on the PC (g++ against Python
   zoneinfo, tzdata 2026f, every 15 min through 2026-27): 451 of 461 zones
   match exactly. The other 10 are `zones.csv` being older than tzdata:
   America/Vancouver, Edmonton, Winnipeg, Yellowknife, Inuvik, Rainy_River
   (Canada's 2026 changes), Havana, Europe/Chisinau, Africa/Casablanca and
   El_Aaiun (Ramadan switches POSIX can't express). The board's own zone
   uses the same table.
   Table refreshed (tzdata 2026f): `scripts/zones_refresh.py` takes each
   zone's rule from its TZif footer, keeps every existing name (524 now,
   aliases like `Asia/Calcutta` and new zones like `Europe/Kyiv` added)
   and checks every zone every 15 minutes for two years. Canada, Chisinau
   and Morocco (permanent +00 since 2026-09-20) now match; only Havana
   differs: Cuba skipped the 2026 fall-back, which a POSIX rule can't say,
   so it shows an hour behind until 2027-03-14, then is right.
   Sweep on classic: 10 fps at 11-13 ms a frame, ~12% of core 0 (other
   screens kept their rates); an uploaded face like Dress would be ~18-20%.
   Most of that is copying the 115 KB layer (~5 ms); restoring only the
   strips under the old hands (step 1's note) would cut it.
   Faster sweep (done, checked on the screens):
   - The render loop tells a control which of its two pictures it draws
     into and whether it's clean (`DrawContext::picture`/`pictureClean`):
     unchanged since the control last drew it. Cleared when the control or
     its params change, when the status screen draws, and when a
     transition draws over it (`transition::apply` now says so); set under
     the layout lock so an API write can't race it.
   - The clock records, per picture, the span of each row its hands
     covered, and on a clean picture with the same layer copies back only
     those (0.3-0.5 ms instead of ~5). A temporary check redrew every
     partial frame the slow way: 600 classic and 500 Dress frames, no pixel
     different.
   - Timing inside draw() then showed the polygon fill was the real cost
     (~45 us a pixel row: a division per edge per sub-row, and the whole
     bounding box cleared and scanned). Now the signed-area accumulation
     rasterizer font renderers use (font-rs): exact cover, ~14.5 us a row.
     Against 64x64 supersampling on the PC: every pixel within ~1.5 steps
     of the 6-bit channel. Where an outline crosses itself, both loops
     fill (non-zero) instead of cancelling.
   - Sweep at 10 fps: classic with a zone hand 3.3-4.6 ms a frame, Dress
     ~7 ms (~7% of core 0, was ~20%).
   Seconds ring (done, checked on the screens): the mockup's 3 px arc round
   the rim, filling from 12 o'clock with the seconds. `secondsRing` param
   for built-in faces (radius 116, the second hand's colour; the hour
   hand's on minimal), `secondsRing: {radius, width, color}` in clock face
   files. Follows `motion`, hidden with `seconds: false`. Drawn with
   smooth::arc each frame; the partial copy now keeps up to three
   stretches a row (ring left, hands, ring right) and restores the ring's
   whole band (~5,400 px a frame). Checked on the PC: every pixel the arc
   draws, over radii, widths and sweeps, falls inside the recorded spans.
   Fix (0d04b46): a thin hand lying nearly flat (near 3 or 9) left a dotted
   trail that pulsed every other second. Its two long edges cross a row
   several pixels apart, and the spans recorded only those crossings, not
   the inside between them; each row now takes leftmost to rightmost.
   Note: the board holds one clock face file per screen, so a test upload
   replaces whatever Command Center put there.
5. Later, only if wanted: Makapix art as the dial, which needs the dial as
   a see-through overlay drawn over changing art (the one costly case).

**Command Center (OrbIt-Command-Center, not this repo):**
- A "Clock faces" rail section between Profiles and Feeds: round
  thumbnails of the saved clock faces and "New clock face" (starts from a
  preset gallery, the mockup's eight).
- The builder takes over the main column while editing (the screen grid
  shrinks to a strip showing which screens use it): large live preview on
  the left, the mockup's option sections on the right. Duplicate and
  tweak, save by name; later import/export as a `.orbitclockface` file
  and a photo cropper (pan and zoom in the circle).
- Stored by the Rust side in `persistence/clock_faces.rs` (like
  `profiles.rs`): `clock-faces.json` plus a folder for photos. What's
  saved is the recipe (the builder's choices), not the rendered picture.
- One drawing module, `clockFaceRenderer.ts`: `renderDial(recipe)` → PNG
  and `handsFor(recipe)` → hand JSON. The builder preview, thumbnails and
  `ScreenPreview` tiles put them together the way the board does, so the
  preview is the board's picture, not a likeness.
- `AnalogClockForm`: a clock face picker with thumbnails, the quick
  options, and an "Edit clock face" link. The colour form stays for
  firmware without the capability.
- Sending follows the existing Apply model: picking or editing a clock
  face marks the screens using it as changed ("Apply Layout (n)"); on
  Apply, `useApplyLayout` uploads it (new Rust command in
  `device/client.rs`) to each screen whose `clockFaceHash` differs, then
  does the bulk POST. A screen reporting a different hash (e.g. after a
  reflash) shows "clock face missing" and gets it on the next Apply. A
  failed upload shows a toast.
- Profiles keep `clockFaceId` like any other param; applying one uploads
  the clock faces it needs, and editing a clock face updates every
  profile using it.
- Optional "Try on screen n" while editing: sends a draft on a button
  press, not on every change, since each one writes the board's flash.

## Open decisions

- None right now.
