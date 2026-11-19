#pragma once

// Board-wide endpoints, alongside orbit-api:
//   GET  /api/v1/system        board and network info (same fields as info-orbs)
//   GET  /api/v1/capabilities  what's built in (info-orbs' shape, with OrbIt as the one widget)
//   GET  /api/v1/settings      timezone, units, API keys (keys masked)
//   POST /api/v1/settings      change any of those
// Anything else answers 404 with a JSON error.

#include <WebServer.h>

namespace api {
    void registerRoutes(WebServer &server);
}
