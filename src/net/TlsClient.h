#pragma once

// An HTTPS client that doesn't close file descriptor 0.
//
// arduino-esp32 2.0.17's stop_ssl_socket() ends by zeroing the whole TLS
// context, which leaves its socket number at 0 instead of -1. The next
// WiFiClientSecure::stop() (HTTPClient calls it in end() and again when it's
// destroyed) then sees socket 0 as open and calls close(0). That closes
// whatever holds descriptor 0: first stdin, after that any LittleFS file
// that happens to get 0, mid-write (a picture upload, a clock face, a
// drawing).
//
// TlsClient puts the socket back to -1 after each stop. HTTPClient must be
// given one (begin(client, url)): when it makes its own, it makes the
// faulty kind.

#include <HTTPClient.h>
#include <WiFiClientSecure.h>

namespace net {

    class TlsClient : public WiFiClientSecure {
    public:
        TlsClient() { setInsecure(); } // as HTTPClient sets up its own clients
        ~TlsClient() { stop(); }       // before ~WiFiClientSecure calls its own stop()
        void stop() override {
            WiFiClientSecure::stop();
            sslclient->socket = -1;
        }
    };

    // Starts a request on http, through tls if the URL is HTTPS.
    inline bool begin(HTTPClient &http, TlsClient &tls, const String &url) {
        return url.startsWith("https://") ? http.begin(tls, url) : http.begin(url);
    }

}
