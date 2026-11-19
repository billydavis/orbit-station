#pragma once

// WiFi, the setup hotspot, mDNS, updates over WiFi and the web server.
//
// With no saved WiFi the board opens a setup hotspot and waits. With saved
// WiFi it connects; if it can't for a few minutes (or loses the network for
// that long) it opens the hotspot for a while, then tries the saved network
// again, and so on, so a moved router or a changed password can be fixed
// from a phone without buttons.

#include <WebServer.h>

namespace net {

    enum class State : uint8_t {
        Connecting, // trying the saved network
        Setup,      // setup hotspot open
        Connected,
        Updating, // receiving new firmware over WiFi
    };

    struct Status {
        State state;
        uint8_t updatePercent;
        char ssid[33];     // network being joined or joined
        char ip[16];       // address on that network
        char hostname[32]; // reachable at <hostname>.local
        char apName[32];   // setup hotspot name
    };

    void begin();
    // Call often from loop().
    void loop();
    // Safe to call from any task.
    Status status();

    // Lists the networks the board can see, with signal and security type.
    void scan();

    // Diagnostic: sets the WiFi transmit power (0 = chip default).
    void setTxPower(float dBm);

    // Erases the saved WiFi and restarts into setup.
    void forgetWifi();

    // Phase 3 adds the API routes here. Only running while connected.
    WebServer &server();

}
