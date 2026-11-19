#pragma once

// Phase 1 test animation: coloured dots circling each screen's number, drawn
// on core 0 through the display core. Stand-in until the OrbIt controls exist.

namespace demo {
    void begin();
    // While paused, the same frame is drawn over and over, so the display core
    // should find nothing changed and send nothing.
    void setPaused(bool paused);
    bool paused();
}
