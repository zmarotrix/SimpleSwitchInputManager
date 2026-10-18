#pragma once

#include "common.h"

// Switch 2 Pro Controller USB protocol.
// References: ndeadly/switch2_controller_research (hid_reports.md, commands.md)
// and the Switch2Connect sources in ../Example.

// Report 0x05 ("common" input report) byte offsets, counted after the report-id byte.
constexpr int kOffButtons = 0x04;      // u32 LE
constexpr int kOffLeftStick = 0x0A;    // 3 bytes, packed 12-bit X/Y
constexpr int kOffRightStick = 0x0D;   // 3 bytes, packed 12-bit X/Y
constexpr int kOffBatteryMv = 0x1F;    // u16 LE
constexpr int kOffCharging = 0x21;     // u8
constexpr int kOffMotion = 0x2A;       // u32 timestamp(us), i16 temp, i16 accel xyz, i16 gyro xyz

// Button bits in the report 0x05 u32.
enum : uint32_t {
    kBtnY = 1u << 0,
    kBtnX = 1u << 1,
    kBtnB = 1u << 2,
    kBtnA = 1u << 3,
    kBtnR = 1u << 6,
    kBtnZR = 1u << 7,
    kBtnMinus = 1u << 8,
    kBtnPlus = 1u << 9,
    kBtnRStick = 1u << 10,
    kBtnLStick = 1u << 11,
    kBtnHome = 1u << 12,
    kBtnCapture = 1u << 13,
    kBtnC = 1u << 14,
    kBtnDown = 1u << 16,
    kBtnUp = 1u << 17,
    kBtnRight = 1u << 18,
    kBtnLeft = 1u << 19,
    kBtnL = 1u << 22,
    kBtnZL = 1u << 23,
    kBtnGR = 1u << 24,
    kBtnGL = 1u << 25,
};

// Feature mask: buttons/sticks + motion + battery. 0x80 (magnetometer) is left off
// because it lowers the controller's report rate.
constexpr uint8_t kFeatureMask = 0x27;

// Stick calibration addresses in controller flash (9 bytes: center, min-range, max-range).
constexpr uint32_t kCalLeftUser = 0x1FC042;
constexpr uint32_t kCalRightUser = 0x1FC062;
constexpr uint32_t kCalLeftFactory = 0x0130A8;
constexpr uint32_t kCalRightFactory = 0x0130E8;

inline void DecodeStick(const uint8_t* p, int& x, int& y)
{
    uint32_t v = p[0] | (p[1] << 8) | (p[2] << 16);
    x = v & 0xFFF;
    y = (v >> 12) & 0xFFF;
}

struct StickCal {
    int cx = 2048, cy = 2048;
    int minX = 1500, minY = 1500;  // distance from center to the low end
    int maxX = 1500, maxY = 1500;  // distance from center to the high end

    // Parses the 9-byte flash block. Returns false (and keeps defaults) on garbage.
    bool Parse(const uint8_t* d)
    {
        int c_x, c_y, n_x, n_y, m_x, m_y;
        DecodeStick(d + 0, c_x, c_y);
        DecodeStick(d + 3, n_x, n_y);
        DecodeStick(d + 6, m_x, m_y);
        bool centerOk = c_x >= 1024 && c_x <= 3072 && c_y >= 1024 && c_y <= 3072;
        bool rangeOk = n_x > 0 && n_y > 0 && m_x > 0 && m_y > 0 &&
                       m_x <= 4095 - c_x && m_y <= 4095 - c_y && n_x <= c_x && n_y <= c_y;
        if (!centerOk || !rangeOk) {
            return false;
        }
        cx = c_x; cy = c_y; minX = n_x; minY = n_y; maxX = m_x; maxY = m_y;
        return true;
    }

    // Returns each axis in [-1, 1]; +Y is up (Nintendo convention).
    void Apply(int rawX, int rawY, float& outX, float& outY) const
    {
        outX = Axis(rawX, cx, minX, maxX);
        outY = Axis(rawY, cy, minY, maxY);
    }

private:
    static float Axis(int raw, int center, int lo, int hi)
    {
        int d = raw - center;
        float v = d >= 0 ? (float)d / hi : (float)d / lo;
        return v > 1.f ? 1.f : (v < -1.f ? -1.f : v);
    }
};
