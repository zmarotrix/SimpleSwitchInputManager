#pragma once

#include "common.h"
#include "output.h"
#include "protocol.h"
#include "rumble.h"

#include <memory>

// Turns a Switch 2 controller's "report 0x05" input (identical over USB and
// Bluetooth) into the chosen virtual controller plus the DSU motion server, and
// sends the game's rumble back to the controller.
class InputForwarder {
public:
    ~InputForwarder() { Stop(); }

    bool Start(int slot, bool bluetooth, const StickCal& left, const StickCal& right, int reportsPerSecond,
               OutputMode mode, RumbleSink* rumble);
    void Stop();

    // body: the 63-byte report without its report-id byte.
    void Process(const uint8_t* body);

private:
    class GyroBias {
    public:
        explicit GyroBias(int window = 250) : m_window(window) {}
        void Add(const int16_t g[3]);
        float bias[3] = {};

    private:
        int m_window;
        int m_n = 0;
        double m_sum[3] = {}, m_sq[3] = {};
    };

    int m_slot = -1;
    bool m_bluetooth = false;
    StickCal m_left, m_right;
    std::unique_ptr<OutputBackend> m_out;
    RumbleScheduler m_rumble;
    GyroBias m_bias;
    uint32_t m_lastImuTs = 0;
    uint64_t m_imuClock = 0;
    LARGE_INTEGER m_qpf{}, m_lastQpc{};
};

// One physical controller on any transport, owned by the main loop's slot table.
class PadSource {
public:
    virtual ~PadSource() = default;
    virtual void Start() = 0;
    virtual void Stop() = 0;
    virtual bool Finished() const = 0;
    virtual const std::wstring& Key() const = 0;  // identifies the physical device
};
