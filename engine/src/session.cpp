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

SwitchSession::SwitchSession(bool bluetooth, CommandTransport& cmd, RumbleSink* rumble)
    : m_bluetooth(bluetooth), m_cmd(cmd), m_rumbleSink(rumble)
{
}

SwitchSession::~SwitchSession()
{
    End();
}

bool SwitchSession::Begin(int reportsPerSecond)
{
    // Identity: the serial number in the controller info block (0x13000). Read only
    // the first 32 bytes: a USB reply is one 64-byte packet including 16 header bytes.
    uint8_t info[0x20] = {};
    {
        std::lock_guard<std::mutex> lock(m_cmdLock);
        if (m_cmd.ReadMemory(0x13000, sizeof(info), info)) {
            for (int i = 2; i < 16 && info[i] >= 0x20 && info[i] < 0x7F; i++) m_serial += (char)info[i];
            char color[8];
            snprintf(color, sizeof(color), "#%02X%02X%02X", info[25], info[26], info[27]);
            m_bodyColor = color;
        }
    }
    m_persist = !m_serial.empty();
    if (!m_persist) {
        Log("  could not read the serial number; settings will not be remembered");
        m_serial = m_bluetooth ? "unknown-bt" : "unknown-usb";
    }

    StickCal left, right;
    {
        std::lock_guard<std::mutex> lock(m_cmdLock);
        left = ReadStickCal(m_cmd, kCalLeftUser, kCalLeftFactory, "Left");
        right = ReadStickCal(m_cmd, kCalRightUser, kCalRightFactory, "Right");
    }

    if (m_persist) {
        m_settings = Config::Get().ForController(m_serial, "Pro Controller");
    } else {
        m_settings.name = "Pro Controller";
        m_settings.mode = Config::Get().Global().defaultMode;
    }
    m_fwd.Start(m_bluetooth, left, right, reportsPerSecond, m_rumbleSink);
    m_fwd.SetRumbleStrength(m_settings.rumble);
    m_fwd.SetRumbleSafety(Config::Get().Global().rumbleSafetySeconds);

    int slot = Hub::AcquireSlot(this, m_serial, m_settings.slot);
    if (!slot) Log("  all %d player slots are taken; this controller has no virtual pad", kMaxSlots);
    m_begun = true;
    ApplySlot(slot);
    Log("  %s (serial %s): player %d, %s", m_settings.name.c_str(), m_serial.c_str(), slot,
        OutputModeName(m_settings.mode));
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
    std::lock_guard<std::mutex> lock(m_cmdLock);
    m_cmd.Send(0x09, 0x07, {pattern, 0, 0, 0});
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
    s.model = "Pro Controller 2";
    s.transport = m_bluetooth ? "bluetooth" : "usb";
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
