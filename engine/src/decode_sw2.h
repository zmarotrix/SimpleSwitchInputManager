#pragma once

#include "pad_input.h"
#include "protocol.h"

// Decodes a Switch 2 controller's "report 0x05" (identical over USB and
// Bluetooth) into PadInput.
class Sw2Decoder {
public:
    Sw2Decoder(const StickCal& left, const StickCal& right, bool wired)
        : m_left(left), m_right(right), m_wired(wired)
    {
        QueryPerformanceFrequency(&m_qpf);
    }

    // body: the 63-byte report without its report-id byte.
    PadInput Decode(const uint8_t* r);

private:
    StickCal m_left, m_right;
    bool m_wired;
    uint32_t m_lastImuTs = 0;
    uint64_t m_imuClock = 0;
    float m_mvAvg = 0;  // smoothed battery voltage: the raw reading jumps with load (rumble, radio)
    LARGE_INTEGER m_qpf{}, m_lastQpc{};
};
