#include "session.h"

#include "config.h"

#include <algorithm>
#include <chrono>

// Player lights 1-8, as the console shows them.
uint8_t PlayerLedPattern(int slot)
{
    static const uint8_t patterns[] = {0x00, 0x01, 0x03, 0x07, 0x0F, 0x09, 0x05, 0x0D, 0x06};
    return (slot >= 1 && slot <= 8) ? patterns[slot] : 0x00;
}

SwitchSession::SwitchSession(DeviceOps& ops, bool wired, std::string model, RumbleSink* rumble, int rumblePeriodMs)
    : m_ops(ops), m_wired(wired), m_model(std::move(model)), m_rumbleSink(rumble), m_rumblePeriodMs(rumblePeriodMs)
{
}

SwitchSession::~SwitchSession()
{
    End();
}

bool SwitchSession::Begin(int reportsPerSecond)
{
    {
        std::lock_guard<std::mutex> lock(m_opsLock);
        m_ops.ReadIdentity(m_serial, m_bodyColor);
    }
    m_persist = !m_serial.empty();
    if (!m_persist) {
        Log("  could not read the controller's identity; settings will not be remembered");
        m_serial = m_wired ? "unknown-usb" : "unknown-bt";
    }

    if (m_persist) {
        m_settings = Config::Get().ForController(m_serial, m_model);
    } else {
        m_settings.name = m_model;
        m_settings.mode = Config::Get().Global().defaultMode;
    }
    m_fwd.Start(reportsPerSecond, m_rumbleSink, m_rumblePeriodMs);
    m_fwd.SetRumbleStrength(m_settings.rumble);
    m_fwd.SetRumbleSafety(Config::Get().Global().rumbleSafetySeconds);

    int slot = Hub::AcquireSlot(this, m_serial, m_settings.slot);
    if (!slot) Log("  all %d player slots are taken; this controller has no virtual pad", kMaxSlots);
    m_begun = true;
    ApplySlot(slot);
    Log("  %s (%s): player %d, %s", m_settings.name.c_str(), m_serial.c_str(), slot, OutputModeName(m_settings.mode));
    Hub::Changed();
    return true;
}

void SwitchSession::End()
{
    if (!m_begun) return;
    m_identifying = false;
    if (m_identify.joinable()) m_identify.join();
    Hub::Release(this);  // after this the tray app can no longer reach us
    m_fwd.Stop();
    m_begun = false;
}

void SwitchSession::ApplySlot(int slot)
{
    std::lock_guard<std::mutex> lock(m_lock);
    m_slot = slot;
    if (!m_fwd.Configure(slot, m_settings.mode) && slot) Log("  could not create the virtual controller");
    SetLeds(PlayerLedPattern(slot));
    if (slot && m_settings.slot != slot) {
        // Remember where the user (or a swap) put this controller.
        m_settings.slot = slot;
        SaveSettings();
    }
}

void SwitchSession::SetLeds(uint8_t pattern)
{
    std::lock_guard<std::mutex> lock(m_opsLock);
    m_ops.SetPlayerLeds(pattern);
}

void SwitchSession::SaveSettings()
{
    if (m_persist) Config::Get().SetController(m_serial, m_settings);
}

SessionStatus SwitchSession::Status()
{
    std::lock_guard<std::mutex> lock(m_lock);
    SessionStatus s;
    s.serial = m_serial;
    s.name = m_settings.name;
    s.model = m_model;
    s.transport = m_wired ? "usb" : "bluetooth";
    s.slot = m_slot;
    s.mode = m_settings.mode;
    s.battery = m_fwd.Battery();
    s.rumble = m_settings.rumble;
    s.bodyColor = m_bodyColor;
    return s;
}

void SwitchSession::SetOutputMode(OutputMode m)
{
    {
        std::lock_guard<std::mutex> lock(m_lock);
        if (m_settings.mode == m) return;
        m_settings.mode = m;
        SaveSettings();
        m_fwd.Configure(m_slot, m);
    }
    Log("Player %d now presents as %s", m_slot, OutputModeName(m));
    Hub::Changed();
}

void SwitchSession::SetRumbleStrength(float s)
{
    {
        std::lock_guard<std::mutex> lock(m_lock);
        m_settings.rumble = std::clamp(s, 0.f, 1.f);
        SaveSettings();
        m_fwd.SetRumbleStrength(m_settings.rumble);
    }
    Hub::Changed();
}

void SwitchSession::SetName(const std::string& name)
{
    {
        std::lock_guard<std::mutex> lock(m_lock);
        m_settings.name = name.substr(0, 40);
        SaveSettings();
    }
    Hub::Changed();
}

// Sweeps the player lights and buzzes briefly so the user can see which one it is.
void SwitchSession::Identify()
{
    if (m_identifying.exchange(true)) return;
    if (m_identify.joinable()) m_identify.join();
    m_identify = std::thread([this] {
        m_fwd.Pulse(0.6f, 300);
        static const uint8_t sweep[] = {0x01, 0x02, 0x04, 0x08, 0x04, 0x02};
        for (int round = 0; round < 3 && m_identifying; round++) {
            for (uint8_t p : sweep) {
                if (!m_identifying) break;
                SetLeds(p);
                std::this_thread::sleep_for(std::chrono::milliseconds(90));
            }
        }
        int slot;
        {
            std::lock_guard<std::mutex> lock(m_lock);
            slot = m_slot;
        }
        SetLeds(PlayerLedPattern(slot));
        m_identifying = false;
    });
}
