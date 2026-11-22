# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

Orbit Station is OrbIt-only firmware for an ESP32-S3 N16R8 (16 MB flash, 8 MB octal PSRAM) driving five GC9A01 240x240 round screens. It has no buttons and no widget system: every screen shows whatever the orbit-api sets. It's a spin-off of info-orbs (AGPL-3.0, so this is too) and stays compatible with orbit-api v1, the API OrbIt Command Center talks to.

`PLAN.md` holds the phases, the results of each, the bench lessons and the open items. Read it before changing the display core or the network code.

## Build / dev commands

PlatformIO, Arduino framework (`espressif32@6.9.0`, arduino-esp32 2.0.17 / IDF 4.4), LovyanGFX. There are no unit tests; check changes on the board.

```powershell
pio run -e s3                                                      # build
pio run -e s3 -t upload                                            # flash over USB
pio run -e s3-ota -t upload --upload-port orbit-station-xx.local   # flash over WiFi
pio device monitor                                                 # serial, 115200
```

- The name to use is on the first screen at boot. Over-the-air updates need the PC's firewall to allow incoming TCP on port 47269.
- `scripts/build_stamp.py` writes `src/core/BuildStamp.cpp` (gitignored) on every build, so `firmwareBuilt` is always right.
- Format C++ with the repo's `.clang-format`.
- Serial commands (`r`, `i`, `f`, `n`, `w`, `l`, `x`, `q`, `z`, `s`, `?`) are listed at the top of `src/main.cpp`. `f` prints fps and draw times; `q` and `x` test WiFi recovery; `z` tests the watchdog.

## Hardware

Pins are in `src/pins.h`: SCLK 12, MOSI 11, DC 13, RST 14 shared, CS 10, 9, 8, 17, 16. SPI runs at 40 MHz on the breadboard (80 MHz fails there).

- Power the screens from their own 3.3 V regulator fed by the board's 5 V. **Never connect the board's 3V3 to the screen rail**: five screens brown it out.
- Never call `lcd.init()` twice; it restarts the SPI bus. Re-init goes through `display::requestReinit`.
- With manual CS, wait for DMA before switching CS.
- Don't DMA straight from PSRAM while the other core draws into it. Frames go through small internal-RAM bands.

## Architecture

- `src/main.cpp`: setup, serial commands, and the Arduino loop (network, health).
- `src/display/`: two PSRAM pictures per screen. Whoever draws calls `beginFrame()`/`endFrame()`; a sender task on core 1 owns the SPI bus, compares 16x16 tile fingerprints and sends only what changed. So a control can redraw everything every frame at no cost on the wire.
- `src/orbit/`: the orbit-api, the saved layout and the render task on core 0.
  - `Control` is the base class; each control is in `controls/` and listed in `CONTROLS` in `Orbit.cpp`.
  - `Transition` plays animated changes between pictures. A control opts in with `contentId()` and a `transition` param.
- `src/makapix/`: Makapix Club player client (HTTPS), and decoders for PNG, GIF, WebP and BMP into PSRAM.
- `src/data/Feeds`: weather and ticker fetching.
- `src/net/`: WiFi, setup hotspot, access point fallback, OTA, recovery. `net::HttpsTurn` makes HTTPS connections take turns.
- `src/core/`: `Health` (task heartbeats, watchdog, restart reasons, memory watch, event log at `/api/v1/log`) and `Settings`.

## Rules that are easy to break

- **Controls save settings, never live data.** The layout is written to NVS when its saved form changes, and clients can feed values every few seconds. For every new control or param, decide up front what's a setting (saved) and what's live (a gauge's `value`, readings, prices, timestamps), and remove live fields in the control's `stripLive()`. It must restore sensibly with them missing. Check that value-only updates don't print `OrbIt: saved layout to NVS`.
- **Keep large payloads out of NVS** (about 8 KB spare). Drawings go in LittleFS files, written only when they change.
- **Internal RAM is the tight resource.** Allocations over 256 bytes go to PSRAM; TLS needs about 17 KB in one internal block per connection. Watch `largestFreeBlock` in `/api/v1/system`.
- **Don't block the Arduino loop.** It's on a 5 s hardware watchdog; long work needs a background task or `health::pause`.
- **Time comparisons must survive wraparound.** Compare `millis()` values by subtraction, and use a signed difference if a stored time can be ahead of now.
- **Keys and personal details never go in files.** API keys, WiFi details and the Makapix token live only in the board's NVS.
