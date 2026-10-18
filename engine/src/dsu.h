#pragma once

#include "common.h"

// Motion + button snapshot for one DSU (Cemuhook) slot.
struct MotionState {
    bool connected = false;
    bool bluetooth = false;
    uint32_t buttons = 0;        // report 0x05 button bits
    float lx = 0, ly = 0, rx = 0, ry = 0;  // [-1, 1], +Y up
    uint64_t timestampUs = 0;
    float accel[3] = {};         // g, DS4 axes
    float gyro[3] = {};          // deg/s: pitch, yaw, roll
    uint8_t battery = 0x05;      // DSU battery code
};

// Cemuhook/DSU motion server on 127.0.0.1:26760 (Ryujinx, Cemu, Dolphin, the
// Yuzu family). Sends motion straight from the controller's 250 Hz stream.
namespace Dsu {

bool Start();
void Stop();
void Update(int slot, const MotionState& s);  // call from the controller thread
void Disconnect(int slot);

}  // namespace Dsu
