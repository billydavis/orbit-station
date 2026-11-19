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
6. A long-running task on core 0 must yield (`vTaskDelay(1)`) at least every
   ~100 ms, or the idle task starves and the watchdog reboots the board.

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

## Open decisions

- **Recovery without buttons (phase 2).** Suggested: reopen the setup hotspot if
  WiFi fails for a few minutes; "reset everything" on three quick power cycles.
