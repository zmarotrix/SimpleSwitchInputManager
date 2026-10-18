#pragma once

#include "commands.h"
#include "config.h"
#include "forwarder.h"
#include "hub.h"

#include <atomic>
#include <mutex>
#include <thread>

// Everything one connected Switch 2 controller needs after its transport has
// woken it up: identity, settings, player slot + lights, the virtual controller
// and live changes from the tray app. Shared by USB and Bluetooth.
class SwitchSession : public ControllerSession {
public:
    SwitchSession(bool bluetooth, CommandTransport& cmd, RumbleSink* rumble);
    ~SwitchSession() override;

    // Reads identity and calibration, claims a slot and creates the virtual controller.
    bool Begin(int reportsPerSecond);
    void End();
    void Process(const uint8_t* body) { m_fwd.Process(body); }

    // ControllerSession
    SessionStatus Status() override;
    void SetOutputMode(OutputMode m) override;
    void SetRumbleStrength(float s) override;
    void SetName(const std::string& name) override;
    void Identify() override;
    void ApplySlot(int slot) override;

private:
    void SetLeds(uint8_t pattern);
    void SaveSettings();

    bool m_bluetooth;
    CommandTransport& m_cmd;
    std::mutex m_cmdLock;  // commands can come from the tray app's thread
    RumbleSink* m_rumbleSink;
    InputForwarder m_fwd;

    std::mutex m_lock;
    std::string m_serial;
    std::string m_bodyColor;
    ControllerSettings m_settings;
    int m_slot = 0;
    bool m_begun = false;
    bool m_persist = true;  // false when the serial could not be read

    std::thread m_identify;
    std::atomic<bool> m_identifying{false};
};

uint8_t PlayerLedPattern(int slot);
