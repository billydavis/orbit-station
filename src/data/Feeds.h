#pragma once

// Data from the internet for the weather and ticker controls, fetched by a
// background task so slow HTTPS requests never hold up drawing or the API.
//
// Controls ask for what they show while drawing; that both returns the
// latest readings and marks the location or symbol as in use. Only what's in
// use is fetched: weather every 10 minutes (one request per location, shared
// by every screen showing it), a ticker symbol at its own interval (never
// more often than every 5 minutes). Readings are kept in memory only.

#include <Arduino.h>

namespace feeds {

    struct Weather {
        bool valid = false;
        String problem; // why there's nothing to show yet
        String city;    // first part of the address Visual Crossing resolved
        float temperature = 0, high = 0, low = 0;
        String description, icon;
    };

    struct Quote {
        bool valid = false;
        String problem;
        String ticker, company, currency;
        float price = 0, percentChange = 0, high52 = 0, low52 = 0;
    };

    void begin();

    Weather weather(const String &location);
    Quote quote(const String &symbol, uint32_t pollMs);
    // Fetches this symbol again now, whatever its interval (the refresh endpoint).
    void refreshQuote(const String &symbol);

    // Made-up readings for every weather screen, for checking how they look.
    // Any can be NAN to leave it alone. Replaced by the next fetch, which is
    // put off by a full refresh interval.
    void previewWeather(float temperature, float high, float low);

}
