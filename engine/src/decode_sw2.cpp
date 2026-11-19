#include "decode_sw2.h"

#include <algorithm>
#include <cstring>

// Percent from the smoothed voltage. The controller reports about 3.72 V when full
// (measured: charging stops there); this is a typical lithium-ion discharge curve
// shifted down to that scale.
static int BatteryPercent(float mv)
{
    static const struct { float mv; int pct; } curve[] = {
        {3700, 100}, {3620, 90}, {3540, 80}, {3470, 70}, {3420, 60}, {3380, 50},
        {3350, 40}, {3320, 30}, {3290, 20}, {3240, 10}, {3150, 5}, {3050, 0},
    };
    if (mv <= 0) return -1;
    if (mv >= curve[0].mv) return 100;
    for (size_t i = 1; i < std::size(curve); i++) {
        if (mv >= curve[i].mv) {
            float t = (mv - curve[i].mv) / (curve[i - 1].mv - curve[i].mv);
            return curve[i].pct + (int)(t * (curve[i - 1].pct - curve[i].pct) + 0.5f);
        }
    }
    return 0;
}

PadInput Sw2Decoder::Decode(const uint8_t* r)
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
    in.hasMotion = true;
    in.motionTimestampUs = m_imuClock;
    // Controller axes -> DS4/DSU axes (Switch2Connect's validated mapping).
    in.accel[0] = -a[0] * kAccelScale;
    in.accel[1] = -a[2] * kAccelScale;
    in.accel[2] = a[1] * kAccelScale;
    in.gyro[0] = g[0] * kGyroScale;
    in.gyro[1] = -g[2] * kGyroScale;
    in.gyro[2] = g[1] * kGyroScale;

    uint16_t mv;
    memcpy(&mv, r + kOffBatteryMv, 2);
    if (mv > 2500 && mv < 5000) {
        // About a 2 s time constant at 250 Hz, 8 s at 65 Hz.
        m_mvAvg = m_mvAvg == 0 ? mv : m_mvAvg + (mv - m_mvAvg) * 0.002f;
    }
    in.batteryPercent = BatteryPercent(m_mvAvg);
    // Power status byte: 0x00 on battery, 0x20 plugged in and full, 0x2C/0x34/0x3C charging.
    uint8_t power = r[kOffCharging];
    in.charging = (power & 0x20) && power != 0x20;
    in.wired = m_wired;
    return in;
}
