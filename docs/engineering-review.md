# Engineering review: manageability and performance

**Reviewed:** 2026-11-22  
**Scope:** Firmware architecture, developer workflow, and performance opportunities. This is a recommendations backlog, not a bug report.

## Assessment

The firmware has clear runtime areas for display, networking, feeds, controls, and health monitoring. The hardware-specific lessons and measured results in [PLAN.md](../PLAN.md) are especially useful. The recommendations below aim to make the code easier to change and validate while preserving that evidence.

## Prioritized recommendations

### 1. Add automated validation

The project currently relies on board checks and has no unit tests ([CLAUDE.md](../CLAUDE.md#L13)). Add CI builds for both PlatformIO environments and focused host-side tests for behavior that does not require hardware, such as control parsing, saved-layout handling, and timing calculations.

Keep hardware validation for display behavior, Wi-Fi recovery, and soak testing. Record the firmware revision and test conditions with results.

**Done when:** pull requests build both environments and the hardware-independent tests run automatically.

### 2. Split the OrbIt module along existing responsibilities

[Orbit.cpp](../src/orbit/Orbit.cpp) combines control registration, layout persistence, rendering/scheduling, and API routes. Separate these concerns into modules for layout storage, rendering, and API handlers while retaining the existing `Control` interface.

**Done when:** each module has a clear responsibility and can be changed without navigating unrelated persistence, rendering, or HTTP code.

### 3. Preserve exact build artifacts for crash diagnosis

The soak notes say a crash backtrace was decoded against a newer ELF, so its location remains unconfirmed ([PLAN.md](../PLAN.md#L227)). Archive the ELF/map file and build metadata—including source revision and resolved dependency versions—with each firmware build. The embedded build timestamp is useful but does not identify the exact source and toolchain ([platformio.ini](../platformio.ini#L24)).

**Done when:** a firmware image can be matched to the exact source revision, dependencies, and debug symbols used to build it.

### 4. Shorten layout-lock hold times

The renderer draws while holding the layout mutex, and API writes hold it through persistence ([Orbit.cpp](../src/orbit/Orbit.cpp#L373), [Orbit.cpp](../src/orbit/Orbit.cpp#L448)). Consider taking a consistent config snapshot while locked, then drawing or performing file writes outside the lock. Use a revision/version check before clearing dirty state so concurrent API updates are not lost.

**Done when:** slow drawing and storage work no longer hold the shared layout mutex, and concurrent updates remain reliable.

## Performance opportunities

### 1. Profile and optimize the radar effect first

The measured baseline is about 19.5 fps per screen, with most effects drawing in 5–8 ms. Radar pie-slice arcs took 55–72 ms ([PLAN.md](../PLAN.md#L162)). Profile that effect before broad renderer changes. Candidate approaches include simplifying geometry, caching static content, and reducing the area redrawn.

**Measure:** per-effect draw time and frame rate on all five screens, before and after each change.

### 2. Measure full-frame tile hashing

Tile diffing reduces SPI traffic, but the sender still hashes image data tile by tile for each frame ([Display.cpp](../src/display/Display.cpp#L150)). If profiling shows that comparison is significant, let controls report changed areas or tiles so unchanged regions can skip hashing as well as transmission. The plan already records this as a possible follow-up.

**Measure:** sender CPU time and PSRAM reads alongside bytes/tiles sent; do not trade away display correctness for an unmeasured optimization.

### 3. Budget Makapix memory across active screens

Makapix limits decoded memory per artwork and compressed download size separately ([Makapix.cpp](../src/makapix/Makapix.cpp#L16), [Makapix.cpp](../src/makapix/Makapix.cpp#L241)). Since each active screen can retain artwork, per-art limits do not by themselves define a total memory budget. Record peak PSRAM use during multi-screen playback and decoding; consider aggregate admission limits or eviction if measurements show insufficient headroom.

**Measure:** peak PSRAM and internal largest-free-block values while all screens play large animations and a new artwork is decoded.

### 4. Revisit feed response buffering only if memory data warrants it

Feed responses are read using `http.getString()` and then parsed into a JSON document ([Feeds.cpp](../src/data/Feeds.cpp#L82)), which may temporarily increase memory use. Streaming may reduce buffering, but the code notes that some responses are chunked. Preserve correct transfer decoding and test against provider responses before changing this path.

## Suggested sequence

1. Establish automated builds/tests and archive exact build artifacts.
2. Measure layout-lock contention and shorten critical sections if it affects responsiveness.
3. Profile the radar effect and Makapix peak memory.
4. Pursue tile-level invalidation or streaming feed parsing only if measurements justify the added complexity.
