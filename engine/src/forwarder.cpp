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

// Learns the gyro's zero-rate offset whenever the controller sits still for a second.
void InputForwarder::GyroBias::Add(const int16_t g[3])
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
        still = still && var < 16.0 && fabs(mean[i]) < 150.0;  // ~1 dps noise, < 9 dps offset
    }
    if (still) {
        for (int i = 0; i < 3; i++) bias[i] = (float)mean[i];
    }
    m_n = 0;
    memset(m_sum, 0, sizeof(m_sum));
    memset(m_sq, 0, sizeof(m_sq));
}

bool InputForwarder::Start(int slot, bool bluetooth, const StickCal& left, const StickCal& right,
                           int reportsPerSecond, OutputMode mode, RumbleSink* rumble)
{
    m_slot = slot;
    m_bluetooth = bluetooth;
    m_left = left;
    m_right = right;
    m_bias = GyroBias(reportsPerSecond);
    m_lastImuTs = 0;
    m_imuClock = 0;
    m_lastQpc = {};
    QueryPerformanceFrequency(&m_qpf);

    if (rumble) {
        // Refresh a little faster than the link delivers so the actuators never starve.
        m_rumble.Start(rumble, !bluetooth, bluetooth ? 15 : 8);
    }
    m_out = CreateOutput(mode);
    m_out->OnRumble([this](const RumbleCommand& c) { m_rumble.Set(c); });
    if (!m_out->Create(slot)) {
        m_out.reset();
        return false;
    }
    return true;
}

void InputForwarder::Stop()
{
    if (m_slot < 0) return;
    if (m_out) m_out->Destroy();  // no more rumble callbacks after this
    m_out.reset();
    m_rumble.Stop();
    Dsu::Disconnect(m_slot);
    m_slot = -1;
}

void InputForwarder::Process(const uint8_t* r)
{
    PadInput in;
    memcpy(&in.buttons, r + kOffButtons, 4);
    int lxRaw, lyRaw, rxRaw, ryRaw;
    DecodeStick(r + kOffLeftStick, lxRaw, lyRaw);
    DecodeStick(r + kOffRightStick, rxRaw, ryRaw);
    m_left.Apply(lxRaw, lyRaw, in.lx, in.ly);
    m_right.Apply(rxRaw, ryRaw, in.rx, in.ry);
    in.lt = (in.buttons & kBtnZL) ? 1.f : 0.f;  // Pro Controller triggers are digital
    in.rt = (in.buttons & kBtnZR) ? 1.f : 0.f;

    // Motion
    const float kGyroScale = 2000.f / 32767.f;  // deg/s per LSB
    const float kAccelScale = 1.f / 4096.f;     // g per LSB
    const uint8_t* m = r + kOffMotion;
    uint32_t imuTs;
    int16_t a[3], g[3];
    memcpy(&imuTs, m, 4);
    memcpy(a, m + 6, 6);
    memcpy(g, m + 12, 6);
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    uint32_t dt = imuTs - m_lastImuTs;
    if (m_lastImuTs == 0 || dt == 0 || dt > 100000) {
        // Controller clock missing or jumped: fall back to host time.
        dt = m_lastQpc.QuadPart ? (uint32_t)((qpc.QuadPart - m_lastQpc.QuadPart) * 1000000 / m_qpf.QuadPart) : 4000;
    }
    m_lastImuTs = imuTs;
    m_lastQpc = qpc;
    m_imuClock += dt;
    m_bias.Add(g);
    in.hasMotion = true;
    in.motionTimestampUs = m_imuClock;
    // Controller axes -> DS4 axes (the convention DSU and DualSense share).
    in.accel[0] = -a[0] * kAccelScale;
    in.accel[1] = -a[2] * kAccelScale;
    in.accel[2] = a[1] * kAccelScale;
    in.gyro[0] = (g[0] - m_bias.bias[0]) * kGyroScale;
    in.gyro[1] = -(g[2] - m_bias.bias[2]) * kGyroScale;
    in.gyro[2] = (g[1] - m_bias.bias[1]) * kGyroScale;

    uint16_t mv;
    memcpy(&mv, r + kOffBatteryMv, 2);
    in.batteryPercent = std::clamp(((int)mv - 3300) * 100 / (4150 - 3300), 0, 100);
    in.wired = !m_bluetooth;

    if (m_out) m_out->Submit(in);

    MotionState ms;
    ms.connected = true;
    ms.bluetooth = m_bluetooth;
    ms.buttons = in.buttons;
    ms.lx = in.lx; ms.ly = in.ly; ms.rx = in.rx; ms.ry = in.ry;
    ms.timestampUs = in.motionTimestampUs;
    memcpy(ms.accel, in.accel, sizeof(ms.accel));
    memcpy(ms.gyro, in.gyro, sizeof(ms.gyro));
    ms.battery = DsuBattery(in.batteryPercent);
    Dsu::Update(m_slot, ms);
}
