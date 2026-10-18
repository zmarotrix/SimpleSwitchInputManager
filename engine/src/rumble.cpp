#include "rumble.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// Resting frequencies the console uses for each band (from Switch2Connect's
// VibrationData defaults); amplitude alone carries the game's motor strength.
constexpr uint32_t kLfFreq = 0x0E1;
constexpr uint32_t kHfFreq = 0x1E1;
constexpr int kAmpScale = 800;      // full motor -> amplitude, as Switch2Connect maps Xbox rumble
constexpr int kWiredFrameCap = 511;  // LF + HF limit per frame on USB power

void Sw2RumbleEncoder::Block(float low, float high, uint8_t out[16])
{
    int lf = std::clamp((int)lroundf(low * kAmpScale), 0, 1023);
    int hf = std::clamp((int)lroundf(high * kAmpScale), 0, 1023);
    if (m_wired && lf + hf > kWiredFrameCap) {
        int total = lf + hf;
        lf = lf * kWiredFrameCap / total;
        hf = hf * kWiredFrameCap / total;
    }
    uint64_t v = (kLfFreq & 0x1FF) | ((uint64_t)(lf & 0x3FF) << 10) | ((uint64_t)(kHfFreq & 0x1FF) << 20) |
                 ((uint64_t)(hf & 0x3FF) << 30);
    out[0] = 0x50 | (m_counter & 0x0F);
    for (int f = 0; f < 3; f++) {
        for (int i = 0; i < 5; i++) out[1 + f * 5 + i] = (uint8_t)(v >> (8 * i));
    }
}

void Sw2RumbleEncoder::Encode(const RumbleCommand& c, uint8_t left[16], uint8_t right[16])
{
    Block(c.lowL, c.highL, left);
    Block(c.lowR, c.highR, right);
    m_counter++;
}

void RumbleScheduler::Start(RumbleSink* sink, int periodMs)
{
    Stop();
    m_sink = sink;
    m_periodMs = periodMs;
    m_quit = false;
    m_cmd = {};
    m_thread = std::thread([this] { Run(); });
}

void RumbleScheduler::Stop()
{
    {
        std::lock_guard<std::mutex> lock(m_lock);
        m_quit = true;
    }
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

void RumbleScheduler::Set(const RumbleCommand& c)
{
    {
        std::lock_guard<std::mutex> lock(m_lock);
        m_lastGameUpdate = Clock::now();  // even an unchanged command proves the game is alive
        if (c == m_cmd) return;
        m_cmd = c;
        m_changed = true;
    }
    m_cv.notify_all();
}

void RumbleScheduler::SetStrength(float s)
{
    std::lock_guard<std::mutex> lock(m_lock);
    m_strength = std::clamp(s, 0.f, 1.f);
}

void RumbleScheduler::SetSafetyTimeout(int seconds)
{
    std::lock_guard<std::mutex> lock(m_lock);
    m_safetySeconds = std::max(0, seconds);
}

void RumbleScheduler::Pulse(float level, int ms)
{
    {
        std::lock_guard<std::mutex> lock(m_lock);
        m_pulseLevel = std::clamp(level, 0.f, 1.f);
        m_pulseUntil = Clock::now() + std::chrono::milliseconds(ms);
        m_changed = true;
    }
    m_cv.notify_all();
}

void RumbleScheduler::Run()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    bool wasActive = false;
    std::unique_lock<std::mutex> lock(m_lock);
    for (;;) {
        // Idle: sleep until something changes. Active: refresh every period,
        // since each block only covers a few milliseconds of vibration.
        if (wasActive) {
            m_cv.wait_for(lock, std::chrono::milliseconds(m_periodMs), [this] { return m_quit || m_changed; });
        } else {
            m_cv.wait(lock, [this] { return m_quit || m_changed; });
        }
        if (m_quit) break;
        m_changed = false;
        auto now = Clock::now();

        if (m_cmd.Active() && m_safetySeconds > 0 &&
            now - m_lastGameUpdate > std::chrono::seconds(m_safetySeconds)) {
            Log("Rumble: no update from the game for %d s, stopping it", m_safetySeconds);
            m_cmd = {};
        }
        RumbleCommand c = m_cmd;
        float s = m_strength;
        c.lowL *= s; c.highL *= s; c.lowR *= s; c.highR *= s;
        if (now < m_pulseUntil) {
            c.lowL = c.lowR = std::max(c.lowL, m_pulseLevel);
            c.highL = c.highR = std::max(c.highL, m_pulseLevel);
        }
        lock.unlock();

        m_sink->SendRumble(c);
        wasActive = c.Active();

        lock.lock();
    }
    lock.unlock();
    if (wasActive) m_sink->SendRumble({});
}
