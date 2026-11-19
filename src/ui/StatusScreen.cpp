#include "StatusScreen.h"
#include "net/Network.h"
#include "net/Recovery.h"

namespace ui {

    namespace {
        constexpr uint16_t DIM = 0x8410; // grey

        void line(LGFX_Sprite &spr, const String &text, int y, const lgfx::IFont *font, uint16_t color) {
            spr.setFont(font);
            spr.setTextColor(color);
            spr.drawString(text, 120, y);
        }
    }

    void drawStatus(LGFX_Sprite &spr) {
        const net::Status s = net::status();
        spr.fillScreen(TFT_BLACK);
        spr.setTextDatum(textdatum_t::middle_center);
        line(spr, "Orbit Station", 38, &fonts::Font2, DIM);

        switch (s.state) {
        case net::State::Setup:
            spr.drawCircle(120, 120, 118, TFT_ORANGE);
            line(spr, "Setup", 72, &fonts::Font4, TFT_ORANGE);
            line(spr, "Join this WiFi:", 106, &fonts::Font2, TFT_WHITE);
            line(spr, s.apName, 128, &fonts::Font4, TFT_WHITE);
            line(spr, "then open", 156, &fonts::Font2, DIM);
            line(spr, s.ip, 176, &fonts::Font2, TFT_WHITE);
            break;

        case net::State::Connecting: {
            spr.drawCircle(120, 120, 118, TFT_YELLOW);
            line(spr, "Connecting", 90, &fonts::Font4, TFT_YELLOW);
            line(spr, s.ssid, 124, &fonts::Font2, TFT_WHITE);
            // Three dots that light up in turn, so it's clearly still working.
            const int lit = (millis() / 400) % 3;
            for (int i = 0; i < 3; i++) spr.fillCircle(104 + i * 16, 156, 4, i == lit ? TFT_YELLOW : 0x4208);
            break;
        }

        case net::State::Connected:
            spr.drawCircle(120, 120, 118, TFT_GREEN);
            line(spr, "Online", 82, &fonts::Font4, TFT_GREEN);
            line(spr, String(s.hostname) + ".local", 118, &fonts::Font2, TFT_WHITE);
            line(spr, s.ip, 140, &fonts::Font2, TFT_WHITE);
            line(spr, s.ssid, 168, &fonts::Font2, DIM);
            break;

        case net::State::Updating:
            line(spr, "Updating", 82, &fonts::Font4, TFT_CYAN);
            line(spr, String(s.updatePercent) + "%", 124, &fonts::Font4, TFT_WHITE);
            spr.fillArc(120, 120, 118, 110, -90, -90 + s.updatePercent * 360 / 100, TFT_CYAN);
            break;
        }

        // Feedback for the three-quick-power-ups reset.
        if (recovery::didFactoryReset() && millis() < 60000) {
            line(spr, "Settings erased", 204, &fonts::Font2, TFT_RED);
        } else if (recovery::quickStarts() > 0) {
            line(spr, "Quick start " + String(recovery::quickStarts()) + " of 3", 204, &fonts::Font2, TFT_YELLOW);
        }
    }

}
