#pragma once

#include "pad_input.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

// Where encoded rumble goes: the controller's USB HID output report or its
// Bluetooth vibration characteristic. Each block is 16 bytes per actuator.
class RumbleSink {
public:
    virtual ~RumbleSink() = default;
    virtual bool SendRumble(const uint8_t left[16], const uint8_t right[16]) = 0;
};

// Switch 2 HD rumble: one block = 0x50|counter followed by three 5-byte frames.
// Frame bits (LSB first): LF freq(9) tone(1) LF amp(10) HF freq(9) tone(1) HF amp(10).
class Sw2RumbleEncoder {
public:
    explicit Sw2RumbleEncoder(bool wired) : m_wired(wired) {}
    void Encode(const RumbleCommand& c, uint8_t left[16], uint8_t right[16]);

private:
    void Block(float low, float high, uint8_t out[16]);
    bool m_wired;
    uint8_t m_counter = 0;
};

// Streams rumble to a controller while a game holds it on, then sends a stop.
class RumbleScheduler {
public:
    using Clock = std::chrono::steady_clock;

    ~RumbleScheduler() { Stop(); }
    void Start(RumbleSink* sink, bool wired, int periodMs);
    void Stop();
    void Set(const RumbleCommand& c);  // from the game, via the virtual controller
    void SetStrength(float s);         // 0..1 user scale
    // Stops rumble a game has left on without touching it for this long (0 = never).
    // Protects against games that crash or exit while rumbling.
    void SetSafetyTimeout(int seconds);
    void Pulse(float level, int ms);   // short buzz for "Identify"; does not change the game's state

private:
    void Run();

    RumbleSink* m_sink = nullptr;
    int m_periodMs = 8;
    bool m_wired = false;
    std::thread m_thread;
    std::mutex m_lock;
    std::condition_variable m_cv;
    RumbleCommand m_cmd;
    Clock::time_point m_lastGameUpdate{};
    Clock::time_point m_pulseUntil{};
    float m_pulseLevel = 0;
    int m_safetySeconds = 20;
    float m_strength = 1.0f;
    bool m_changed = false;
    bool m_quit = false;
};
