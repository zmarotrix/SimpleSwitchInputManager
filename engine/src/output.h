#pragma once

#include "pad_input.h"

#include <functional>
#include <memory>

enum class OutputMode : int {
    DirectInput = 0,  // generic HID game pad (joy.cpl, DirectInput emulators)
    XInput = 1,       // Xbox controller: widest game support
    DualSense = 2,    // PS5 controller: native gyro + rumble for Steam/SDL games and emulators
};

const char* OutputModeName(OutputMode m);

// A virtual controller that games see. Each physical controller owns one.
class OutputBackend {
public:
    virtual ~OutputBackend() = default;
    virtual bool Create(int slot) = 0;
    virtual void Destroy() = 0;
    virtual bool Submit(const PadInput& in) = 0;
    // True if Windows itself stops this controller's rumble when the game exits
    // (XInput does); otherwise the engine's safety cut-off guards against stuck rumble.
    virtual bool RumbleStopsWithApp() const { return false; }

    // Called (from a driver thread) whenever a game changes rumble.
    void OnRumble(std::function<void(const RumbleCommand&)> handler) { m_rumble = std::move(handler); }

protected:
    void Rumble(const RumbleCommand& c)
    {
        if (m_rumble) m_rumble(c);
    }

private:
    std::function<void(const RumbleCommand&)> m_rumble;
};

// dinputPid: the DirectInput product ID for this controller family (see common.h).
std::unique_ptr<OutputBackend> CreateOutput(OutputMode mode, USHORT dinputPid);
bool WinUHidAvailable();

// Shared helpers for backends.
inline uint16_t AxisTo16(float v)  // [-1, 1] -> 0..65535, 0x8000 centered
{
    float x = 32767.5f + v * 32767.5f;
    return (uint16_t)(x < 0 ? 0 : (x > 65535 ? 65535 : x + 0.5f));
}
inline uint8_t AxisTo8(float v)  // [-1, 1] -> 0..255, 0x80 centered
{
    float x = 127.5f + v * 127.5f;
    return (uint8_t)(x < 0 ? 0 : (x > 255 ? 255 : x + 0.5f));
}
