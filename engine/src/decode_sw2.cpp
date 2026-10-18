#include "decode_sw2.h"

#include <algorithm>
#include <cstring>

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
    in.batteryPercent = std::clamp(((int)mv - 3300) * 100 / (4150 - 3300), 0, 100);
    in.wired = m_wired;
    return in;
}
