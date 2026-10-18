#pragma once

#include "output.h"

#include <functional>
#include <string>
#include <vector>

constexpr int kMaxSlots = 8;

struct SessionStatus {
    std::string serial;
    std::string name;
    std::string model;      // e.g. "Pro Controller 2"
    std::string transport;  // "usb" or "bluetooth"
    int slot = 0;           // player 1..8, 0 = none
    OutputMode mode = OutputMode::DirectInput;
    int battery = -1;       // percent, -1 unknown
    float rumble = 1.0f;
    std::string bodyColor;  // "#RRGGBB"
};

// A connected physical controller that the engine can be asked to change.
class ControllerSession {
public:
    virtual ~ControllerSession() = default;
    virtual SessionStatus Status() = 0;
    virtual void SetOutputMode(OutputMode m) = 0;
    virtual void SetRumbleStrength(float s) = 0;
    virtual void SetName(const std::string& name) = 0;
    virtual void Identify() = 0;
    // Called by the hub when this controller's slot changes (also on swaps).
    virtual void ApplySlot(int slot) = 0;
};

// Live controllers and their player slots. Slots stay with a controller across
// USB <-> Bluetooth handovers because they are keyed by serial number.
namespace Hub {

// Picks the controller's slot: `preferred` if free, else the lowest free one.
// If the same controller is still finishing on another transport, waits for it.
int AcquireSlot(ControllerSession* s, const std::string& serial, int preferred);
void Release(ControllerSession* s);

// Moves a controller to `slot`, swapping with whoever has it. Returns false if invalid.
bool MoveToSlot(const std::string& serial, int slot);

std::vector<SessionStatus> List();
// Runs fn on the live session for `serial` (under the hub lock). Returns false if not connected.
bool With(const std::string& serial, const std::function<void(ControllerSession&)>& fn);

// Change notifications (connect, disconnect, setting changes).
void Changed();
void OnChange(std::function<void()> fn);

}  // namespace Hub
