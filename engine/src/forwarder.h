#pragma once

#include "common.h"
#include "output.h"
#include "rumble.h"

#include <atomic>
#include <memory>
#include <mutex>

// Feeds a controller's decoded input to its virtual controller and the DSU
// motion server, and sends the game's rumble back. Family-neutral: Switch 1
// and Switch 2 decoders both produce PadInput.
class InputForwarder {
public:
    ~InputForwarder() { Stop(); }

    void Start(int reportsPerSecond, RumbleSink* rumble, int rumblePeriodMs, USHORT dinputPid);
    // Creates (or recreates) the virtual controller for player `slot` (0 = none).
    // Safe to call while input flows.
    bool Configure(int slot, OutputMode mode);
    void Stop();

    void Process(PadInput in);

    void SetRumbleStrength(float s) { m_rumble.SetStrength(s); }
    void SetRumbleSafety(int seconds);
    void Pulse(float level, int ms) { m_rumble.Pulse(level, ms); }
    int Battery() const { return m_battery; }

private:
    // Learns the gyro's zero-rate offset whenever the controller sits still.
    class GyroBias {
    public:
        explicit GyroBias(int window = 250) : m_window(window) {}
        void Add(const float g[3]);
        float bias[3] = {};

    private:
        int m_window;
        int m_n = 0;
        double m_sum[3] = {}, m_sq[3] = {};
    };

    std::mutex m_lock;  // guards m_out / m_slot against Configure() from other threads
    bool m_started = false;
    int m_slot = 0;
    std::unique_ptr<OutputBackend> m_out;
    RumbleScheduler m_rumble;
    GyroBias m_bias;
    std::atomic<int> m_battery{-1};
    int m_safetySeconds = 20;
    USHORT m_dinputPid = kVirtualPidSwitch2Pro;
};

// One physical controller on any transport, owned by the main loop.
class PadSource {
public:
    virtual ~PadSource() = default;
    virtual void Start() = 0;
    virtual void Stop() = 0;
    virtual bool Finished() const = 0;
    virtual const std::wstring& Key() const = 0;  // identifies the physical connection
};
