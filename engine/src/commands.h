#pragma once

#include "common.h"
#include "protocol.h"
#include "session.h"

#include <vector>

// The controller's command channel: same packets over USB (bulk endpoint) and
// Bluetooth (GATT write + notify); only the transport byte differs.
class CommandTransport {
public:
    virtual ~CommandTransport() = default;

    // Sends one command and waits for its reply; on success `reply` gets the
    // payload after the 8-byte reply header.
    virtual bool Send(uint8_t cmd, uint8_t sub, const uint8_t* data, uint8_t len,
                      std::vector<uint8_t>* reply = nullptr) = 0;

    bool Send(uint8_t cmd, uint8_t sub, std::initializer_list<uint8_t> data, std::vector<uint8_t>* reply = nullptr)
    {
        std::vector<uint8_t> d(data);
        return Send(cmd, sub, d.data(), (uint8_t)d.size(), reply);
    }

    bool ReadMemory(uint32_t addr, uint8_t len, uint8_t* out)
    {
        uint8_t req[8] = {len, 0x7E, 0, 0, (uint8_t)addr, (uint8_t)(addr >> 8), (uint8_t)(addr >> 16),
                          (uint8_t)(addr >> 24)};
        std::vector<uint8_t> r;
        if (!Send(0x02, 0x04, req, sizeof(req), &r) || r.size() < 8u + len) return false;
        uint32_t echoed;
        memcpy(&echoed, &r[4], 4);
        if (r[0] != len || echoed != addr) return false;
        memcpy(out, &r[8], len);
        return true;
    }

    // Player LED pattern for slot 0..3.
    bool SetPlayerLed(int slot)
    {
        static const uint8_t pattern[] = {0x01, 0x03, 0x07, 0x0F};
        return Send(0x09, 0x07, {pattern[slot % 4], 0, 0, 0});
    }
};

inline StickCal ReadStickCal(CommandTransport& ch, uint32_t userAddr, uint32_t factoryAddr, const char* name)
{
    StickCal cal;
    uint8_t d[11];
    bool haveUser = ch.ReadMemory(userAddr, sizeof(d), d) && !(d[0] == 0xFF && d[1] == 0xFF && d[2] == 0xFF);
    if ((haveUser && cal.Parse(d)) || (ch.ReadMemory(factoryAddr, sizeof(d), d) && cal.Parse(d))) {
        Log("  %s stick calibration: center %d,%d  range -%d/+%d, -%d/+%d%s", name, cal.cx, cal.cy, cal.minX,
            cal.maxX, cal.minY, cal.maxY, haveUser ? " (user)" : "");
    } else {
        Log("  %s stick calibration unreadable, using defaults", name);
    }
    return cal;
}

// Switch 2 identity and player lights over the command channel.
class Sw2DeviceOps : public DeviceOps {
public:
    explicit Sw2DeviceOps(CommandTransport& ch) : m_ch(ch) {}

    bool ReadIdentity(std::string& id, std::string& bodyColor) override
    {
        // Serial number at 0x13000+2. Read only 32 bytes: a USB reply is one
        // 64-byte packet including 16 header bytes.
        uint8_t info[0x20] = {};
        if (!m_ch.ReadMemory(0x13000, sizeof(info), info)) return false;
        for (int i = 2; i < 16 && info[i] >= 0x20 && info[i] < 0x7F; i++) id += (char)info[i];
        char color[8];
        snprintf(color, sizeof(color), "#%02X%02X%02X", info[25], info[26], info[27]);
        bodyColor = color;
        return !id.empty();
    }

    void SetPlayerLeds(uint8_t pattern) override { m_ch.Send(0x09, 0x07, {pattern, 0, 0, 0}); }

private:
    CommandTransport& m_ch;
};
