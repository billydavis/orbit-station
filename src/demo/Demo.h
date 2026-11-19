#pragma once

// Phase 1 test animation: coloured dots circling each screen's number, drawn
// on core 0 through the display core. Stand-in until the OrbIt controls exist.

namespace demo {
    void begin();
    // While paused, screens 2-5 aren't drawn at all; only the status screen.
    void setPaused(bool paused);
    bool paused();
}
