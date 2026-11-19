#pragma once

// The board as a Makapix Club player (makapix.club), over its HTTPS player API
// (see fabkury/makapix docs/http-api/player-rpc.md).
//
// Pairing needs no keys from the user: the first time a screen shows Makapix
// art, the board provisions itself and that screen shows a 6-character code
// to register at makapix.club. Once registered, the board collects its own
// API token and keeps it (and its player key) in NVS. A rejected token is
// renewed with the player key.
//
// A background task fetches each feed's posts, downloads and decodes the next
// artwork for a screen when it's due, and reports what was shown (Makapix's
// view counts). Only screens that ask are served.

#include "Artwork.h"
#include <ArduinoJson.h>

namespace makapix {

    // What a makapix screen shows.
    struct Feed {
        String channel, user, hashtag, sort;
        uint32_t changeMs = 0;
        bool sameFeed(const Feed &o) const { return channel == o.channel && user == o.user && hashtag == o.hashtag && sort == o.sort; }
    };

    struct Showing {
        ArtworkPtr art;      // null while there's nothing to show yet
        uint32_t since = 0;  // millis() the artwork came up (animation clock)
        String message;      // what to say when there's no art
        String code;         // registration code while pairing
    };

    void begin();

    // Called by a makapix screen as it draws: returns what to show, and keeps
    // the screen's feed served.
    Showing show(int screen, const Feed &feed);

    // GET/POST /api/v1/makapix: pairing state; forget erases the pairing.
    void statusJson(JsonObject out);
    void forget();

}
