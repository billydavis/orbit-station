#pragma once

// Factory reset without buttons: power the board on and off three times in a
// row, each time for less than 10 seconds. The third start erases every
// saved setting, WiFi included, and the setup hotspot opens.

namespace recovery {

    // Call first thing in setup(), before WiFi or settings are read.
    void begin();
    // Call from loop(); after 10 s up, this start no longer counts as quick.
    void loop();
    // True if this start erased the settings.
    bool didFactoryReset();
    // How many quick power-ups in a row so far, this one included (0 once
    // the board has been up 10 s).
    int quickStarts();

}
