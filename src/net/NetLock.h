#pragma once

// HTTPS connections take turns: each needs about 17 KB of internal RAM in one
// piece for its TLS buffers, and two at once (weather and Makapix, say) can
// leave neither enough. Hold this for the whole request.

#include <Arduino.h>

namespace net {

    class HttpsTurn {
    public:
        HttpsTurn();
        ~HttpsTurn();
    };

}
