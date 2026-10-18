#pragma once

#include "config.h"
#include "forwarder.h"
#include "hub.h"

#include <atomic>
#include <mutex>
#include <thread>

// What a session needs from a specific controller family.
class DeviceOps {
public:
    virtual ~DeviceOps() = default;
    // Stable identity (serial number or Bluetooth address) and body colour "#RRGGBB".
    virtual bool ReadIdentity(std::string& id, std::string& bodyColor) = 0;
    // Player lights, bit 0 = first light.
    virtual void SetPlayerLeds(uint8_t pattern) = 0;
};

// Everything one connected controller needs after its transport has woken it
// up: identity, settings, player slot + lights, the virtual controller and live
// changes from the tray app. Shared by every controller family and transport.
class SwitchSession : public ControllerSession {
public:
    SwitchSession(DeviceOps& ops, bool wired, std::string model, USHORT dinputPid, RumbleSink* rumble,
                  int rumblePeriodMs);
    ~SwitchSession() override;

    // Reads identity, claims a slot and creates the virtual controller.
    bool Begin(int reportsPerSecond);
    void End();
    void Process(const PadInput& in) { m_fwd.Process(in); }
    const std::string& Id() const { return m_serial; }

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

    DeviceOps& m_ops;
    std::mutex m_opsLock;  // device commands can come from the tray app's thread
    bool m_wired;
    std::string m_model;
    RumbleSink* m_rumbleSink;
    int m_rumblePeriodMs;
    USHORT m_dinputPid;
    InputForwarder m_fwd;

    std::mutex m_lock;
    std::string m_serial;
    std::string m_bodyColor;
    ControllerSettings m_settings;
    int m_slot = 0;
    bool m_begun = false;
    bool m_persist = true;  // false when the identity could not be read

    std::thread m_identify;
    std::atomic<bool> m_identifying{false};
};

uint8_t PlayerLedPattern(int slot);
