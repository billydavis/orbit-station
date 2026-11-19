#include "Feeds.h"
#include "core/Settings.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>

namespace feeds {

    namespace {
        constexpr uint32_t WEATHER_EVERY_MS = 10 * 60 * 1000;
        constexpr uint32_t MIN_TICKER_MS = 5 * 60 * 1000; // twelvedata's free tier: 800 calls a day
        constexpr uint32_t IN_USE_MS = 60 * 1000;           // not asked for in this long: stop fetching
        constexpr uint32_t RETRY_MS = 60 * 1000;            // after a failed request
        constexpr int SLOTS = 8;

        struct WeatherEntry {
            String location;
            bool metric = false;
            Weather data;
            uint32_t wantedAt = 0, fetchedAt = 0;
            bool fetched = false;
        };
        struct QuoteEntry {
            String symbol;
            uint32_t pollMs = 0;
            Quote data;
            uint32_t wantedAt = 0, fetchedAt = 0;
            bool fetched = false, now = false;
        };

        WeatherEntry weathers[SLOTS];
        QuoteEntry quotes[SLOTS];
        SemaphoreHandle_t lock;

        struct Lock {
            Lock() { xSemaphoreTake(lock, portMAX_DELAY); }
            ~Lock() { xSemaphoreGive(lock); }
        };

        // The entry for key, or a free (or the least recently wanted) one.
        template <typename E> E &entryFor(E (&entries)[SLOTS], const String &key, String E::*field) {
            E *oldest = &entries[0];
            for (E &e : entries) {
                if (e.*field == key) return e;
                if (e.wantedAt < oldest->wantedAt) oldest = &e;
            }
            *oldest = E();
            oldest->*field = key;
            return *oldest;
        }

        bool inUse(uint32_t wantedAt) { return wantedAt && millis() - wantedAt < IN_USE_MS; }

        String encode(const String &s) {
            String out;
            for (char c : s) {
                if (isalnum((unsigned char) c) || strchr("-_.~,", c)) {
                    out += c;
                } else {
                    char hex[4];
                    snprintf(hex, sizeof(hex), "%%%02X", (unsigned char) c);
                    out += hex;
                }
            }
            return out;
        }

        // GET url into doc. Returns "" or what went wrong.
        String getJson(const String &url, JsonDocument &doc) {
            HTTPClient http;
            http.setTimeout(10000);
            if (!http.begin(url)) return "couldn't start the request";
            const int code = http.GET();
            String problem;
            if (code <= 0) {
                problem = "no connection (" + http.errorToString(code) + ")";
            } else if (code != HTTP_CODE_OK) {
                problem = "refused (HTTP " + String(code) + ")";
            } else if (deserializeJson(doc, http.getString())) {
                // getString(), not getStream(): some replies (twelvedata's)
                // come chunked, and the raw stream still has the chunk markers.
                problem = "unreadable reply";
            }
            http.end();
            return problem;
        }

        void fetchWeather(const String &location, bool metric, const String &key) {
            JsonDocument doc;
            const String url = "https://weather.visualcrossing.com/VisualCrossingWebServices/rest/services/timeline/" +
                               encode(location) + "/next3days?key=" + key + "&unitGroup=" + (metric ? "metric" : "us") +
                               "&include=days,current&iconSet=icons1&lang=en";
            const String problem = getJson(url, doc);
            Weather w;
            if (problem.isEmpty()) {
                w.valid = true;
                w.city = doc["resolvedAddress"].as<String>();
                if (w.city.indexOf(',') > 0) w.city.remove(w.city.indexOf(','));
                w.temperature = doc["currentConditions"]["temp"];
                w.icon = doc["currentConditions"]["icon"].as<String>();
                w.description = doc["days"][0]["description"].as<String>();
                w.high = doc["days"][0]["tempmax"];
                w.low = doc["days"][0]["tempmin"];
                Serial.printf("Feeds: weather for %s: %.0f\n", location.c_str(), w.temperature);
            } else {
                w.problem = problem.startsWith("refused") ? "Location not found" : "Weather unavailable";
                Serial.printf("Feeds: weather for %s failed: %s\n", location.c_str(), problem.c_str());
            }
            Lock l;
            for (WeatherEntry &e : weathers) {
                if (e.location != location || e.metric != metric) continue;
                // Keep the last good readings through a failed refresh.
                if (w.valid || !e.data.valid) e.data = w;
                e.fetched = true;
                e.fetchedAt = millis() - (w.valid ? 0 : WEATHER_EVERY_MS - RETRY_MS);
            }
        }

        String currencySign(const String &currency, const String &symbol) {
            if (currency == "EUR" || symbol.indexOf("/EUR") >= 0) return "\xE2\x82\xAC"; // €
            if (currency == "GBP" || symbol.indexOf("/GBP") >= 0) return "\xC2\xA3";     // £
            return "$";
        }

        void fetchQuote(const String &symbol, const String &key) {
            JsonDocument doc;
            const String problem = getJson("https://api.twelvedata.com/quote?apikey=" + key + "&symbol=" + encode(symbol), doc);
            Quote q;
            if (problem.isEmpty() && doc["close"].as<float>() > 0) {
                q.valid = true;
                q.price = doc["close"];
                q.percentChange = doc["percent_change"];
                q.high52 = doc["fifty_two_week"]["high"];
                q.low52 = doc["fifty_two_week"]["low"];
                q.company = doc["name"].as<String>();
                q.ticker = doc["symbol"].as<String>();
                q.currency = currencySign(doc["currency"].as<String>(), symbol);
                Serial.printf("Feeds: %s at %.2f\n", symbol.c_str(), q.price);
            } else {
                // twelvedata answers 200 with {"status":"error"} for a bad symbol or key.
                q.problem = doc["message"].is<const char *>() ? "Symbol not found" : "Price unavailable";
                Serial.printf("Feeds: quote for %s failed: %s\n", symbol.c_str(), problem.length() ? problem.c_str() : (doc["message"] | "no price"));
            }
            Lock l;
            for (QuoteEntry &e : quotes) {
                if (e.symbol != symbol) continue;
                if (q.valid || !e.data.valid) e.data = q;
                e.fetched = true;
                e.fetchedAt = millis();
                if (!q.valid) e.fetchedAt -= max(e.pollMs, MIN_TICKER_MS) - RETRY_MS; // try again sooner
            }
        }

        void task(void *) {
            for (;;) {
                vTaskDelay(pdMS_TO_TICKS(500));
                if (WiFi.status() != WL_CONNECTED) continue;
                const settings::Values s = settings::get();

                // One request per pass, so a ticker refresh never waits behind a list.
                String location, symbol;
                bool metric = false;
                {
                    Lock l;
                    for (WeatherEntry &e : weathers) {
                        if (!inUse(e.wantedAt) || e.data.problem == "Set a weather API key") continue;
                        if (!e.fetched || millis() - e.fetchedAt >= WEATHER_EVERY_MS) {
                            location = e.location;
                            metric = e.metric;
                            break;
                        }
                    }
                    if (location.isEmpty()) {
                        for (QuoteEntry &e : quotes) {
                            if (!inUse(e.wantedAt) || e.data.problem == "Set a ticker API key") continue;
                            if (e.now || !e.fetched || millis() - e.fetchedAt >= max(e.pollMs, MIN_TICKER_MS)) {
                                symbol = e.symbol;
                                e.now = false;
                                break;
                            }
                        }
                    }
                }
                if (location.length()) fetchWeather(location, metric, s.weatherApiKey);
                if (symbol.length()) fetchQuote(symbol, s.tickerApiKey);
            }
        }
    }

    void begin() {
        lock = xSemaphoreCreateMutex();
        xTaskCreatePinnedToCore(task, "feeds", 12288, nullptr, 1, nullptr, 1);
    }

    Weather weather(const String &location) {
        const settings::Values s = settings::get();
        Lock l;
        WeatherEntry &e = entryFor(weathers, location, &WeatherEntry::location);
        e.wantedAt = millis() | 1;
        if (e.metric != s.metric) {
            e.metric = s.metric; // units changed: fetch again
            e.fetched = false;
            e.data = Weather();
        }
        // No key: say so (and clear it once one is set).
        if (s.weatherApiKey.isEmpty()) e.data.problem = "Set a weather API key";
        else if (e.data.problem == "Set a weather API key") e.data.problem = "";
        return e.data;
    }

    Quote quote(const String &symbol, uint32_t pollMs) {
        const settings::Values s = settings::get();
        Lock l;
        QuoteEntry &e = entryFor(quotes, symbol, &QuoteEntry::symbol);
        e.wantedAt = millis() | 1;
        e.pollMs = e.pollMs ? min(e.pollMs, pollMs) : pollMs; // several screens: the fastest wins
        if (s.tickerApiKey.isEmpty()) e.data.problem = "Set a ticker API key";
        else if (e.data.problem == "Set a ticker API key") e.data.problem = "";
        return e.data;
    }

    void refreshQuote(const String &symbol) {
        Lock l;
        QuoteEntry &e = entryFor(quotes, symbol, &QuoteEntry::symbol);
        e.wantedAt = millis() | 1;
        e.now = true;
    }

    void previewWeather(float temperature, float high, float low) {
        Lock l;
        for (WeatherEntry &e : weathers) {
            if (!inUse(e.wantedAt)) continue;
            e.data.valid = true;
            e.data.problem = "";
            if (!isnan(temperature)) e.data.temperature = temperature;
            if (!isnan(high)) e.data.high = high;
            if (!isnan(low)) e.data.low = low;
            e.fetched = true;
            e.fetchedAt = millis(); // keep it up for a full refresh interval
        }
    }

}
