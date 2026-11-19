#include "SysMonitorControl.h"
#include "ui/Text.h"

namespace orbit {

    namespace {
        const char *const READINGS[] = {"cpu", "cpuTemp", "gpu", "gpuTemp", "ram", "ramTotal", "ssdTemp"};
        const char *const CENTERS[] = {"", "cpu", "cpuTemp", "gpu", "gpuTemp", "ram", "ssdTemp", "none", nullptr};

        constexpr int CENTRE = 120;
        constexpr int BAR_W = 60, BAR_H = 6;
        constexpr int VALUE_SIZE = 19, LABEL_SIZE = 10, CENTER_SIZE = 20;
        constexpr uint16_t BG = TFT_BLACK;
        constexpr uint16_t CPU_COLOR = TFT_CYAN, GPU_COLOR = TFT_MAGENTA, RAM_COLOR = 0xFDA0, TEMP_COLOR = TFT_RED;
        const char *DEG = "\xC2\xB0"; // °
        const char *DOT = "\xC2\xB7"; // ·

        String whole(float v) { return String((int) lroundf(v)); }

        void quadrant(LGFX_Sprite &spr, int x, int y, uint16_t color, float percent, const String &value, const String &label) {
            text::draw(spr, value, x, y - 15, VALUE_SIZE, Align::MiddleCenter, color, BG);
            text::draw(spr, label, x, y + 2, LABEL_SIZE, Align::MiddleCenter, TFT_WHITE, BG);
            const int bx = x - BAR_W / 2, by = y + 12;
            spr.fillRect(bx, by, BAR_W, BAR_H, TFT_DARKGREY);
            const int fill = lroundf(BAR_W * constrain(percent, 0.0f, 100.0f) / 100.0f);
            if (fill > 0) spr.fillRect(bx, by, fill, BAR_H, color);
        }
    }

    int SysMonitorControl::parse(JsonObjectConst in, JsonObject out, String &error) const {
        int status;
        for (const char *r : READINGS) {
            if ((status = optionalNumber(in, out, r, 0, error))) return status;
        }
        if ((status = optionalChoice(in, out, "center", CENTERS, error))) {
            error = "unknown sysMonitor center '" + in["center"].as<String>() + "'";
            return status;
        }
        return 0;
    }

    void SysMonitorControl::stripLive(JsonObject params) const {
        for (const char *r : READINGS) params.remove(r);
    }

    void SysMonitorControl::draw(LGFX_Sprite &spr, JsonObjectConst p, const DrawContext &) {
        const float cpu = p["cpu"], gpu = p["gpu"], ram = p["ram"], ssdTemp = p["ssdTemp"];
        const float cpuTemp = p["cpuTemp"], gpuTemp = p["gpuTemp"], ramTotal = p["ramTotal"];
        spr.fillScreen(BG);

        // CPU and GPU on top, RAM and drive temperature below.
        quadrant(spr, 71, 71, CPU_COLOR, cpu, whole(cpu) + "%", String("CPU ") + DOT + " " + whole(cpuTemp) + DEG);
        quadrant(spr, 170, 71, GPU_COLOR, gpu, whole(gpu) + "%", String("GPU ") + DOT + " " + whole(gpuTemp) + DEG);
        quadrant(spr, 71, 170, RAM_COLOR, ram, whole(ram) + "%",
                 ramTotal > 0 ? String("RAM ") + DOT + " " + String(ramTotal, 1) + "G" : String("RAM"));
        quadrant(spr, 170, 170, TEMP_COLOR, ssdTemp, whole(ssdTemp) + DEG, String("SSD ") + DOT + " TEMP");

        // The centre: a pinned reading, nothing, or the busiest of cpu/gpu/ram.
        String center = p["center"].as<String>();
        if (center == "none") return;
        if (center.isEmpty()) center = cpu >= gpu && cpu >= ram ? "cpu" : (gpu >= ram ? "gpu" : "ram");
        String value;
        uint16_t color;
        if (center == "cpu") value = "CPU " + whole(cpu) + "%", color = CPU_COLOR;
        else if (center == "cpuTemp") value = "CPU " + whole(cpuTemp) + DEG + "C", color = CPU_COLOR;
        else if (center == "gpu") value = "GPU " + whole(gpu) + "%", color = GPU_COLOR;
        else if (center == "gpuTemp") value = "GPU " + whole(gpuTemp) + DEG + "C", color = GPU_COLOR;
        else if (center == "ram") value = "RAM " + whole(ram) + "%", color = RAM_COLOR;
        else value = "SSD " + whole(ssdTemp) + DEG + "C", color = TEMP_COLOR;
        text::draw(spr, value, CENTRE, CENTRE, CENTER_SIZE, Align::MiddleCenter, color, BG);
    }

}
