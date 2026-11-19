#pragma once

// Network and recovery status for one screen: what to do during setup, where
// to find the board once it's online, update progress.

#include <LovyanGFX.hpp>

namespace ui {
    void drawStatus(LGFX_Sprite &spr);
}
