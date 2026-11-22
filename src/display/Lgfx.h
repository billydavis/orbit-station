#pragma once

// LovyanGFX setup for five GC9A01 screens sharing one SPI bus. LovyanGFX sees
// a single panel; the display core switches between screens with their CS
// lines, so a few things LovyanGFX assumes about "the" panel need handling.

#include "pins.h"
#include <LovyanGFX.hpp>

namespace display {

    class PanelGC9A01 : public lgfx::Panel_GC9A01 {
    public:
        // Sends the GC9A01 setup commands to whichever screens are selected,
        // without restarting the SPI bus. Calling init() a second time does
        // restart it, and that scrambles every screen. With softReset, the
        // selected screens are reset first by command, so the shared RST wire
        // (and the other screens) are left alone.
        void resendInitCommands(bool softReset) {
            if (softReset) {
                startWrite(true);
                writeCommand(0x01, 1); // SWRESET
                endWrite();
                delay(150); // the reset takes up to 120 ms
            }
            startWrite(true);
            for (uint8_t i = 0; auto cmds = getInitCommands(i); i++) command_list(cmds);
            endWrite();
        }

        // Re-sends the setup commands to the selected screen with no reset and
        // the 120 ms wait after "sleep out" cut to 10 ms. A screen that's been
        // running all along keeps its picture and doesn't blink (the values are
        // the ones it already has); one that was unplugged and plugged back in
        // wakes up configured. Quick enough not to stall the other screens.
        void refreshQuietly() {
            startWrite(true);
            for (uint8_t i = 0; auto cmds = getInitCommands(i); i++) {
                for (;;) {
                    const uint8_t cmd = *cmds++, num = *cmds++;
                    if (cmd == 0xFF && num == 0xFF) break;
                    writeCommand(cmd, 1);
                    _bus->flush();
                    for (uint8_t n = num & ~CMD_INIT_DELAY; n; n--) {
                        writeData(*cmds++, 1);
                        _bus->flush();
                    }
                    if (num & CMD_INIT_DELAY) {
                        cmds++; // the original delay
                        delay(10);
                    }
                }
            }
            endWrite();
        }

        // LovyanGFX skips re-sending the drawing window when it matches the last
        // one it sent. Each screen has its own window, so forget it whenever we
        // switch screens.
        void forgetWindow() {
            _xsxe = ~0u;
            _ysye = ~0u;
        }
    };

    class Lgfx : public lgfx::LGFX_Device {
        PanelGC9A01 _panel;
        lgfx::Bus_SPI _bus;

    public:
        PanelGC9A01 &panel() { return _panel; }

        void configure(uint32_t freqHz) {
            auto b = _bus.config();
            b.spi_host = SPI2_HOST;
            b.spi_mode = 0;
            b.freq_write = freqHz;
            b.freq_read = 8000000;
            b.spi_3wire = true;
            b.use_lock = true;
            b.dma_channel = SPI_DMA_CH_AUTO;
            b.pin_sclk = pins::SCLK;
            b.pin_mosi = pins::MOSI;
            b.pin_miso = -1;
            b.pin_dc = pins::DC;
            _bus.config(b);
            _panel.setBus(&_bus);

            auto p = _panel.config();
            p.pin_cs = -1; // the display core drives the five CS lines
            p.pin_rst = pins::RST;
            p.pin_busy = -1;
            p.panel_width = 240;
            p.panel_height = 240;
            p.memory_width = 240;
            p.memory_height = 240;
            p.offset_x = 0;
            p.offset_y = 0;
            p.offset_rotation = 0;
            p.readable = false;
            // Leave this false and use invertDisplay(): with both on they cancel out.
            p.invert = false;
            p.rgb_order = false;
            p.dlen_16bit = false;
            p.bus_shared = false;
            _panel.config(p);
            setPanel(&_panel);
        }
    };

}
