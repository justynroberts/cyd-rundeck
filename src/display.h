#pragma once
#include <Arduino.h>

namespace display {
    void begin();
    void tick();
    void setBacklight(uint8_t pct);
    bool touched();
    // Boot-time gesture: if user is holding the screen, draws a progress bar.
    // Returns true if the user held for holdMs (caller should wipe NVS + reboot).
    bool factoryResetPrompt(uint32_t holdMs = 2000);
}
