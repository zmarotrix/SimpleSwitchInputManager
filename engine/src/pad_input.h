#pragma once

#include "common.h"

// One controller's state, normalized so every output mode can consume it.
// Buttons use the Switch 2 "report 0x05" bit layout (protocol.h) for every
// controller family.
struct PadInput {
    uint32_t buttons = 0;
    float lx = 0, ly = 0, rx = 0, ry = 0;  // [-1, 1], +Y up
    float lt = 0, rt = 0;                  // [0, 1]
    bool hasMotion = false;
    float accel[3] = {};                   // g, DS4 axes (X right, Y up, Z toward the player)
    float gyro[3] = {};                    // deg/s, DS4 axes (pitch, yaw, roll)
    uint64_t motionTimestampUs = 0;
    int batteryPercent = 100;
    bool charging = false;
    bool wired = false;
};

// What a game asked the controller's actuators to do, per side, [0, 1].
// "low" drives the low-frequency band, "high" the high-frequency band.
struct RumbleCommand {
    float lowL = 0, highL = 0, lowR = 0, highR = 0;
    bool Active() const { return lowL > 0 || highL > 0 || lowR > 0 || highR > 0; }
    bool operator==(const RumbleCommand&) const = default;
};
