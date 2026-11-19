#pragma once

#include "orbit/Control.h"

namespace orbit {

    // A CPU/GPU/RAM/drive-temperature dashboard: four quadrant blocks (value,
    // label, small fill bar) in fixed colours, plus an optional centre reading.
    // Same layout as info-orbs.
    //
    // params (all optional):
    //   cpu, gpu, ram (percent), cpuTemp, gpuTemp, ssdTemp (°C), ramTotal (GB)
    //       live readings, default 0, never saved
    //   center  "cpu", "cpuTemp", "gpu", "gpuTemp", "ram", "ssdTemp", "none",
    //           or "" (default: whichever of cpu/gpu/ram is highest); a setting
    class SysMonitorControl : public Control {
    public:
        const char *name() const override { return "sysMonitor"; }
        int parse(JsonObjectConst params, JsonObject out, String &error) const override;
        void stripLive(JsonObject params) const override;
        void draw(LGFX_Sprite &spr, JsonObjectConst params, const DrawContext &ctx) override;
    };

}
