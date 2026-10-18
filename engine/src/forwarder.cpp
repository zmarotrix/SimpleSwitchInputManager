#include "forwarder.h"

#include "dsu.h"

#include <algorithm>
#include <cmath>
#include <cstring>

static uint8_t DsuBattery(int percent)
{
    if (percent >= 90) return 0x05;
    if (percent >= 60) return 0x04;
    if (percent >= 30) return 0x03;
    if (percent >= 10) return 0x02;
    return 0x01;
}

void InputForwarder::GyroBias::Add(const float g[3])
{
    for (int i = 0; i < 3; i++) {
        m_sum[i] += g[i];
        m_sq[i] += (double)g[i] * g[i];
    }
    if (++m_n < m_window) return;
    bool still = true;
    double mean[3];
    for (int i = 0; i < 3; i++) {
        mean[i] = m_sum[i] / m_n;
        double var = m_sq[i] / m_n - mean[i] * mean[i];
        still = still && var < 0.08 && fabs(mean[i]) < 9.0;  // ~0.3 dps noise, < 9 dps offset
    }
    if (still) {
        for (int i = 0; i < 3; i++) bias[i] = (float)mean[i];
    }
    m_n = 0;
    memset(m_sum, 0, sizeof(m_sum));
    memset(m_sq, 0, sizeof(m_sq));
}

void InputForwarder::Start(int reportsPerSecond, RumbleSink* rumble, int rumblePeriodMs)
{
    m_bias = GyroBias(reportsPerSecond);  // about one second of samples
    if (rumble) m_rumble.Start(rumble, rumblePeriodMs);
    m_started = true;
}

bool InputForwarder::Configure(int slot, OutputMode mode)
{
    std::lock_guard<std::mutex> lock(m_lock);
    if (m_out) m_out->Destroy();
    m_out.reset();
    m_rumble.Set({});
    if (m_slot && m_slot != slot) Dsu::Disconnect(m_slot - 1);
    m_slot = slot;
    if (!slot) return false;
    m_out = CreateOutput(mode);
    m_out->OnRumble([this](const RumbleCommand& c) { m_rumble.Set(c); });
    m_rumble.SetSafetyTimeout(m_out->RumbleStopsWithApp() ? 0 : m_safetySeconds);
    if (!m_out->Create(slot - 1)) {
        m_out.reset();
        return false;
    }
    return true;
}

void InputForwarder::SetRumbleSafety(int seconds)
{
    std::lock_guard<std::mutex> lock(m_lock);
    m_safetySeconds = seconds;
    m_rumble.SetSafetyTimeout(m_out && m_out->RumbleStopsWithApp() ? 0 : seconds);
}

void InputForwarder::Stop()
{
    if (!m_started) return;
    {
        std::lock_guard<std::mutex> lock(m_lock);
        if (m_out) m_out->Destroy();  // no more rumble callbacks after this
        m_out.reset();
        if (m_slot) Dsu::Disconnect(m_slot - 1);
        m_slot = 0;
    }
    m_rumble.Stop();
    m_started = false;
}

void InputForwarder::Process(PadInput in)
{
    if (in.hasMotion) {
        m_bias.Add(in.gyro);
        for (int i = 0; i < 3; i++) in.gyro[i] -= m_bias.bias[i];
    }
    m_battery = in.batteryPercent;

    std::lock_guard<std::mutex> lock(m_lock);
    if (m_out) m_out->Submit(in);
    if (!m_slot) return;

    MotionState ms;
    ms.connected = true;
    ms.bluetooth = !in.wired;
    ms.buttons = in.buttons;
    ms.lx = in.lx; ms.ly = in.ly; ms.rx = in.rx; ms.ry = in.ry;
    ms.timestampUs = in.motionTimestampUs;
    memcpy(ms.accel, in.accel, sizeof(ms.accel));
    memcpy(ms.gyro, in.gyro, sizeof(ms.gyro));
    ms.battery = DsuBattery(in.batteryPercent);
    Dsu::Update(m_slot - 1, ms);  // DSU has slots 0..3; Update ignores the rest
}
