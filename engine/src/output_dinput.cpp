// DirectInput mode: a generic HID joystick (shows up in joy.cpl and every
// DirectInput game or emulator), with USB HID PID force feedback so games and
// SDL-based emulators can rumble it.

#include "output.h"
#include "protocol.h"

#include "WinUHid.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

namespace {

// Report IDs. Input: 0x01 controls, 0x02 PID state. Output: 0x11-0x1D. Feature: 0x21-0x23.
enum : uint8_t {
    kRidInput = 0x01,
    kRidPidState = 0x02,
    kRidSetEffect = 0x11,
    kRidSetEnvelope = 0x12,
    kRidSetPeriodic = 0x14,
    kRidSetConstant = 0x15,
    kRidSetRamp = 0x16,
    kRidEffectOp = 0x1A,
    kRidBlockFree = 0x1B,
    kRidDeviceControl = 0x1C,
    kRidDeviceGain = 0x1D,
    kRidCreateEffect = 0x21,
    kRidBlockLoad = 0x22,
    kRidPool = 0x23,
};

constexpr int kMaxEffects = 40;

// Effect types, in the order of the Effect Type arrays below (values 1..7).
enum : uint8_t { kEtConstant = 1, kEtRamp, kEtSquare, kEtSine, kEtTriangle, kEtSawUp, kEtSawDown };

// Button order follows the Switch 1 Pro Controller over Bluetooth so existing
// emulator bindings line up:
//   1 B   2 A   3 Y   4 X   5 L   6 R   7 ZL   8 ZR   9 Minus  10 Plus
//  11 LStick  12 RStick  13 Home  14 Capture  15 C  16 GL  17 GR
//
// The force-feedback part is written from the USB "Device Class Definition for
// Physical Interface Devices" (PID 1.0) usage tables.
#define PID_EBI 0x09, 0x22, 0x15, 0x01, 0x25, kMaxEffects, 0x75, 0x08, 0x95, 0x01, 0x91, 0x02
#define PID_EBI_FEATURE 0x09, 0x22, 0x15, 0x01, 0x25, kMaxEffects, 0x75, 0x08, 0x95, 0x01, 0xB1, 0x02
#define PID_TIME_MS 0x66, 0x03, 0x10, 0x55, 0x0D  // Unit (seconds), Exponent (-3)
#define PID_UNIT_NONE 0x65, 0x00, 0x55, 0x00
#define PID_EFFECT_TYPES 0x09, 0x26, 0x09, 0x27, 0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x33, 0x09, 0x34

static const uint8_t kReportDescriptor[] = {
    0x05, 0x01,                    // Usage Page (Generic Desktop)
    0x09, 0x04,                    // Usage (Joystick)
    0xA1, 0x01,                    // Collection (Application)

    // --- Controls (input report 1) ---
    0x85, kRidInput,
    0x05, 0x09,                    //   Usage Page (Button)
    0x19, 0x01, 0x29, 0x11,        //   Usage Minimum (1), Maximum (17)
    0x15, 0x00, 0x25, 0x01,        //   Logical 0..1
    0x75, 0x01, 0x95, 0x11,        //   17 x 1 bit
    0x81, 0x02,                    //   Input (Data, Var, Abs)
    0x75, 0x07, 0x95, 0x01, 0x81, 0x03,  // padding
    0x05, 0x01,                    //   Usage Page (Generic Desktop)
    0x09, 0x39,                    //   Usage (Hat Switch)
    0x15, 0x00, 0x25, 0x07,        //   Logical 0..7
    0x35, 0x00, 0x46, 0x3B, 0x01,  //   Physical 0..315
    0x65, 0x14,                    //   Unit (Degrees)
    0x75, 0x04, 0x95, 0x01,
    0x81, 0x42,                    //   Input (Data, Var, Abs, Null State)
    0x65, 0x00, 0x45, 0x00,        //   Unit none, Physical Maximum 0
    0x75, 0x04, 0x95, 0x01, 0x81, 0x03,  // padding
    0x09, 0x01,                    //   Usage (Pointer)
    0xA1, 0x00,                    //   Collection (Physical)
    0x09, 0x30, 0x09, 0x31,        //     X, Y
    0x09, 0x33, 0x09, 0x34,        //     Rx, Ry
    0x15, 0x00, 0x27, 0xFF, 0xFF, 0x00, 0x00,  // Logical 0..65535
    0x75, 0x10, 0x95, 0x04,
    0x81, 0x02,
    0xC0,

    // --- PID State (input report 2) ---
    0x05, 0x0F,                    //   Usage Page (PID)
    0x09, 0x92,                    //   PID State Report
    0xA1, 0x02,
    0x85, kRidPidState,
    0x09, 0x9F, 0x09, 0xA0, 0x09, 0xA4, 0x09, 0xA5, 0x09, 0xA6,  // Paused, Actuators Enabled, Safety, Override, Power
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x05, 0x81, 0x02,
    0x95, 0x03, 0x81, 0x03,
    0x09, 0x94,                    //   Effect Playing
    0x75, 0x01, 0x95, 0x01, 0x81, 0x02,
    0x09, 0x22,                    //   Effect Block Index
    0x15, 0x01, 0x25, kMaxEffects, 0x75, 0x07, 0x95, 0x01, 0x81, 0x02,
    0xC0,

    // --- Set Effect (output 0x11) ---
    0x09, 0x21,
    0xA1, 0x02,
    0x85, kRidSetEffect,
    PID_EBI,
    0x09, 0x25,                    //   Effect Type
    0xA1, 0x02,
    PID_EFFECT_TYPES,
    0x15, 0x01, 0x25, 0x07, 0x75, 0x08, 0x95, 0x01, 0x91, 0x00,  // Output (Data, Array)
    0xC0,
    0x09, 0x50, 0x09, 0x54, 0x09, 0x51,  // Duration, Trigger Repeat Interval, Sample Period
    0x15, 0x00, 0x27, 0xFF, 0xFF, 0x00, 0x00,  // 0..65535 ms
    PID_TIME_MS,
    0x75, 0x10, 0x95, 0x03, 0x91, 0x02,
    PID_UNIT_NONE,
    0x09, 0x52,                    //   Gain
    0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x01, 0x91, 0x02,
    0x09, 0x53,                    //   Trigger Button
    0x15, 0x01, 0x25, 0x11, 0x75, 0x08, 0x95, 0x01, 0x91, 0x02,
    // Note: SDL's DirectInput rumble (all versions) requests axes "X, X", which
    // DirectInput rejects on any force-feedback device; SDL apps should use the
    // XInput or DualSense modes for rumble.
    0x09, 0x55,                    //   Axes Enable
    0xA1, 0x02,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31,  // X, Y
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x02, 0x91, 0x02,
    0xC0,
    0x05, 0x0F,
    0x09, 0x56,                    //   Direction Enable
    0x95, 0x01, 0x91, 0x02,
    0x95, 0x05, 0x91, 0x03,        //   padding
    0x09, 0x57,                    //   Direction
    0xA1, 0x02,
    0x0B, 0x01, 0x00, 0x0A, 0x00,  //   Ordinal 1
    0x0B, 0x02, 0x00, 0x0A, 0x00,  //   Ordinal 2
    0x66, 0x14, 0x00, 0x55, 0x0E,  //   Unit (degrees), Exponent (-2)
    0x15, 0x00, 0x26, 0xFF, 0x00,
    0x35, 0x00, 0x47, 0xA0, 0x8C, 0x00, 0x00,  // Physical 0..36000
    0x75, 0x08, 0x95, 0x02, 0x91, 0x02,
    PID_UNIT_NONE,
    0x35, 0x00, 0x45, 0x00,
    0xC0,
    0xC0,

    // --- Set Envelope (output 0x12) ---
    0x09, 0x5A,
    0xA1, 0x02,
    0x85, kRidSetEnvelope,
    PID_EBI,
    0x09, 0x5B, 0x09, 0x5D,        //   Attack Level, Fade Level
    0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x02, 0x91, 0x02,
    0x09, 0x5C, 0x09, 0x5E,        //   Attack Time, Fade Time
    0x15, 0x00, 0x27, 0xFF, 0xFF, 0x00, 0x00,  // 0..65535 ms
    PID_TIME_MS,
    0x75, 0x10, 0x95, 0x02, 0x91, 0x02,
    PID_UNIT_NONE,
    0xC0,

    // --- Set Periodic (output 0x14) ---
    0x09, 0x6E,
    0xA1, 0x02,
    0x85, kRidSetPeriodic,
    PID_EBI,
    0x09, 0x70,                    //   Magnitude
    0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x01, 0x91, 0x02,
    0x09, 0x6F,                    //   Offset
    0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x91, 0x02,
    0x09, 0x71,                    //   Phase
    0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x01, 0x91, 0x02,
    0x09, 0x72,                    //   Period
    0x15, 0x00, 0x27, 0xFF, 0xFF, 0x00, 0x00,  // 0..65535 ms
    PID_TIME_MS,
    0x75, 0x10, 0x95, 0x01, 0x91, 0x02,
    PID_UNIT_NONE,
    0xC0,

    // --- Set Constant Force (output 0x15) ---
    0x09, 0x73,
    0xA1, 0x02,
    0x85, kRidSetConstant,
    PID_EBI,
    0x09, 0x70,                    //   Magnitude
    0x16, 0x01, 0xFF, 0x26, 0xFF, 0x00,  // Logical -255..255
    0x75, 0x10, 0x95, 0x01, 0x91, 0x02,
    0xC0,

    // --- Set Ramp Force (output 0x16) ---
    0x09, 0x74,
    0xA1, 0x02,
    0x85, kRidSetRamp,
    PID_EBI,
    0x09, 0x75, 0x09, 0x76,        //   Ramp Start, Ramp End
    0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x02, 0x91, 0x02,
    0xC0,

    // --- Effect Operation (output 0x1A) ---
    0x09, 0x77,
    0xA1, 0x02,
    0x85, kRidEffectOp,
    PID_EBI,
    0x09, 0x78,                    //   Effect Operation
    0xA1, 0x02,
    0x09, 0x79, 0x09, 0x7A, 0x09, 0x7B,  // Start, Start Solo, Stop
    0x15, 0x01, 0x25, 0x03, 0x75, 0x08, 0x95, 0x01, 0x91, 0x00,
    0xC0,
    0x09, 0x7C,                    //   Loop Count
    0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x01, 0x91, 0x02,
    0xC0,

    // --- Block Free (output 0x1B) ---
    0x09, 0x90,
    0xA1, 0x02,
    0x85, kRidBlockFree,
    PID_EBI,
    0xC0,

    // --- Device Control (output 0x1C) ---
    0x09, 0x95,
    0xA1, 0x02,
    0x85, kRidDeviceControl,
    0x09, 0x96,
    0xA1, 0x02,
    0x09, 0x97, 0x09, 0x98, 0x09, 0x99, 0x09, 0x9A, 0x09, 0x9B, 0x09, 0x9C,  // Enable, Disable, Stop All, Reset, Pause, Continue
    0x15, 0x01, 0x25, 0x06, 0x75, 0x08, 0x95, 0x01, 0x91, 0x00,
    0xC0,
    0xC0,

    // --- Device Gain (output 0x1D) ---
    0x09, 0x7D,
    0xA1, 0x02,
    0x85, kRidDeviceGain,
    0x09, 0x7E,
    0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x01, 0x91, 0x02,
    0xC0,

    // --- Create New Effect (feature 0x21) ---
    0x09, 0xAB,
    0xA1, 0x02,
    0x85, kRidCreateEffect,
    0x09, 0x25,
    0xA1, 0x02,
    PID_EFFECT_TYPES,
    0x15, 0x01, 0x25, 0x07, 0x75, 0x08, 0x95, 0x01, 0xB1, 0x00,  // Feature (Data, Array)
    0xC0,
    0x05, 0x01, 0x09, 0x3B,        //   Byte Count (Generic Desktop)
    0x15, 0x00, 0x26, 0xFF, 0x01, 0x75, 0x0A, 0x95, 0x01, 0xB1, 0x02,
    0x75, 0x06, 0x95, 0x01, 0xB1, 0x01,
    0x05, 0x0F,
    0xC0,

    // --- Block Load (feature 0x22) ---
    0x09, 0x89,
    0xA1, 0x02,
    0x85, kRidBlockLoad,
    PID_EBI_FEATURE,
    0x09, 0x8B,                    //   Block Load Status
    0xA1, 0x02,
    0x09, 0x8C, 0x09, 0x8D, 0x09, 0x8E,  // Success, Full, Error
    0x15, 0x01, 0x25, 0x03, 0x75, 0x08, 0x95, 0x01, 0xB1, 0x00,
    0xC0,
    0x09, 0xAC,                    //   RAM Pool Available
    0x15, 0x00, 0x27, 0xFF, 0xFF, 0x00, 0x00, 0x75, 0x10, 0x95, 0x01, 0xB1, 0x00,
    0xC0,

    // --- PID Pool (feature 0x23) ---
    0x09, 0x7F,
    0xA1, 0x02,
    0x85, kRidPool,
    0x09, 0x80,                    //   RAM Pool Size
    0x15, 0x00, 0x27, 0xFF, 0xFF, 0x00, 0x00, 0x75, 0x10, 0x95, 0x01, 0xB1, 0x02,
    0x09, 0x83,                    //   Simultaneous Effects Max
    0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x01, 0xB1, 0x02,
    0x09, 0xA9, 0x09, 0xAA,        //   Device Managed Pool, Shared Parameter Blocks
    0x25, 0x01, 0x75, 0x01, 0x95, 0x02, 0xB1, 0x02,
    0x75, 0x06, 0x95, 0x01, 0xB1, 0x03,
    0xC0,

    0xC0,                          // End Collection (Application)
};

#pragma pack(push, 1)
struct InputReport {
    uint8_t id = kRidInput;
    uint8_t buttons[3];
    uint8_t hat;
    uint16_t axes[4];
};
#pragma pack(pop)
static_assert(sizeof(InputReport) == 13, "report layout");

uint8_t Hat(uint32_t b)
{
    bool u = b & kBtnUp, d = b & kBtnDown, l = b & kBtnLeft, r = b & kBtnRight;
    if (u && !d) return r && !l ? 1 : (l && !r ? 7 : 0);
    if (d && !u) return r && !l ? 3 : (l && !r ? 5 : 4);
    if (r && !l) return 2;
    if (l && !r) return 6;
    return 8;
}

uint32_t MapButtons(uint32_t b)
{
    static const uint32_t order[] = {kBtnB,      kBtnA,       kBtnY,  kBtnX,    kBtnL,      kBtnR,
                                     kBtnZL,     kBtnZR,      kBtnMinus, kBtnPlus, kBtnLStick, kBtnRStick,
                                     kBtnHome,   kBtnCapture, kBtnC,  kBtnGL,   kBtnGR};
    uint32_t out = 0;
    for (int i = 0; i < (int)(sizeof(order) / sizeof(order[0])); i++) {
        if (b & order[i]) out |= 1u << i;
    }
    return out;
}

// Tracks the effects a game has loaded and turns whatever is playing into one
// rumble strength. The Pro Controller has no force-feedback motor, so direction,
// waveform shape and envelopes collapse to "how hard to vibrate".
class EffectEngine {
public:
    using Clock = std::chrono::steady_clock;

    struct Effect {
        bool used = false;
        uint8_t type = 0;
        uint16_t durationMs = 0;  // 0 or 0xFFFF means "until stopped"
        uint8_t gain = 255;
        int magnitude = 0;        // constant: -255..255; periodic: 0..255; ramp: max(|start|, |end|)
        int offset = 0;           // periodic only, -127..127
        bool playing = false;
        int loops = 1;
        Clock::time_point start;
    };

    // Returns the new block index, or 0 when full.
    int Allocate(uint8_t type)
    {
        std::lock_guard<std::mutex> lock(m_lock);
        for (int i = 0; i < kMaxEffects; i++) {
            if (!m_fx[i].used) {
                m_fx[i] = Effect{};
                m_fx[i].used = true;
                m_fx[i].type = type;
                return i + 1;
            }
        }
        return 0;
    }

    int FreeCount()
    {
        std::lock_guard<std::mutex> lock(m_lock);
        return (int)std::count_if(m_fx, m_fx + kMaxEffects, [](const Effect& e) { return !e.used; });
    }

    template <typename F>
    void Edit(int ebi, F f)
    {
        if (ebi < 1 || ebi > kMaxEffects) return;
        {
            std::lock_guard<std::mutex> lock(m_lock);
            f(m_fx[ebi - 1]);
        }
        m_cv.notify_all();
    }

    void StopAll(bool freeToo)
    {
        {
            std::lock_guard<std::mutex> lock(m_lock);
            for (auto& e : m_fx) {
                e.playing = false;
                if (freeToo) e.used = false;
            }
        }
        m_cv.notify_all();
    }

    void SetDeviceGain(uint8_t g)
    {
        {
            std::lock_guard<std::mutex> lock(m_lock);
            m_deviceGain = g;
        }
        m_cv.notify_all();
    }

    void SetEnabled(bool on)
    {
        {
            std::lock_guard<std::mutex> lock(m_lock);
            m_enabled = on;
        }
        m_cv.notify_all();
    }

    void SetPaused(bool on)
    {
        {
            std::lock_guard<std::mutex> lock(m_lock);
            m_paused = on;
        }
        m_cv.notify_all();
    }

    void Start(std::function<void(float)> out)
    {
        m_out = std::move(out);
        m_quit = false;
        m_thread = std::thread([this] { Run(); });
    }

    void Stop()
    {
        {
            std::lock_guard<std::mutex> lock(m_lock);
            m_quit = true;
        }
        m_cv.notify_all();
        if (m_thread.joinable()) m_thread.join();
    }

private:
    // Strength of everything currently playing, 0..1; expires finished effects.
    float Evaluate(Clock::time_point now, bool& anyPlaying)
    {
        anyPlaying = false;
        if (!m_enabled || m_paused) return 0;
        float sum = 0;
        for (auto& e : m_fx) {
            if (!e.used || !e.playing) continue;
            bool infinite = e.durationMs == 0 || e.durationMs == 0xFFFF;
            if (!infinite && now - e.start >= std::chrono::milliseconds((int64_t)e.durationMs * e.loops)) {
                e.playing = false;
                continue;
            }
            anyPlaying = true;
            float level;
            switch (e.type) {
            case kEtConstant: level = std::abs(e.magnitude) / 255.f; break;
            case kEtRamp: level = e.magnitude / 127.f; break;
            default: level = (e.magnitude + std::abs(e.offset) * 2) / 255.f; break;
            }
            sum += level * e.gain / 255.f;
        }
        return std::min(1.f, sum * m_deviceGain / 255.f);
    }

    void Run()
    {
        float last = -1;
        std::unique_lock<std::mutex> lock(m_lock);
        while (!m_quit) {
            bool anyPlaying;
            float level = Evaluate(Clock::now(), anyPlaying);
            if (level != last) {
                last = level;
                lock.unlock();
                m_out(level);
                lock.lock();
            }
            // Poll only while something is playing (to catch durations ending).
            if (anyPlaying) m_cv.wait_for(lock, std::chrono::milliseconds(10));
            else m_cv.wait(lock);
        }
    }

    std::mutex m_lock;
    std::condition_variable m_cv;
    std::thread m_thread;
    bool m_quit = false;
    Effect m_fx[kMaxEffects];
    uint8_t m_deviceGain = 255;
    bool m_enabled = true;
    bool m_paused = false;
    std::function<void(float)> m_out;
};

class DirectInputOutput : public OutputBackend {
public:
    ~DirectInputOutput() override { Destroy(); }

    bool Create(int slot) override
    {
        wchar_t instanceId[32];
        swprintf_s(instanceId, L"Switch2Pro%d", slot);

        WINUHID_DEVICE_CONFIG config = {};
        config.SupportedEvents =
            (WINUHID_EVENT_TYPE)(WINUHID_EVENT_WRITE_REPORT | WINUHID_EVENT_GET_FEATURE | WINUHID_EVENT_SET_FEATURE);
        config.VendorID = kVirtualVid;
        config.ProductID = kVirtualPid;
        config.VersionNumber = 0x0100;
        config.ReportDescriptorLength = sizeof(kReportDescriptor);
        config.ReportDescriptor = kReportDescriptor;
        config.InstanceID = instanceId;

        m_dev = WinUHidCreateDevice(&config);
        if (!m_dev) {
            Log("WinUHidCreateDevice failed: %lu", GetLastError());
            return false;
        }
        Submit(PadInput{});  // neutral state first so readers never see an uninitialised report
        SubmitPidState();

        m_fx.Start([this](float level) {
            RumbleCommand c;
            c.lowL = c.lowR = c.highL = c.highR = level;
            Rumble(c);
        });
        if (!WinUHidStartDevice(m_dev, &DirectInputOutput::OnEvent, this)) {
            Log("WinUHidStartDevice failed: %lu", GetLastError());
            Destroy();
            return false;
        }
        return true;
    }

    void Destroy() override
    {
        if (m_dev) {
            WinUHidStopDevice(m_dev);
            m_fx.Stop();
            WinUHidDestroyDevice(m_dev);
            m_dev = nullptr;
        }
    }

    bool Submit(const PadInput& in) override
    {
        if (!m_dev) return false;
        uint32_t buttons = MapButtons(in.buttons);
        InputReport r;
        r.buttons[0] = (uint8_t)(buttons);
        r.buttons[1] = (uint8_t)(buttons >> 8);
        r.buttons[2] = (uint8_t)(buttons >> 16) & 0x01;
        r.hat = Hat(in.buttons);
        r.axes[0] = AxisTo16(in.lx);
        r.axes[1] = AxisTo16(-in.ly);  // HID/DirectInput: +Y is down
        r.axes[2] = AxisTo16(in.rx);
        r.axes[3] = AxisTo16(-in.ry);
        return WinUHidSubmitInputReport(m_dev, &r, sizeof(r)) != FALSE;
    }

private:
    void SubmitPidState()
    {
        // Actuators enabled + powered; no effect playing.
        const uint8_t state[3] = {kRidPidState, 0x02 | 0x10, 0x00};
        WinUHidSubmitInputReport(m_dev, state, sizeof(state));
    }

    static VOID OnEvent(PVOID ctx, PWINUHID_DEVICE dev, PCWINUHID_EVENT ev)
    {
        auto* self = (DirectInputOutput*)ctx;
        if (ev->Type == WINUHID_EVENT_GET_FEATURE) {
            self->OnGetFeature(dev, ev);
        } else {
            bool ok = self->OnWrite(ev->Write.Data, ev->Write.DataLength);
            WinUHidCompleteWriteEvent(dev, ev, ok);
        }
    }

    void OnGetFeature(PWINUHID_DEVICE dev, PCWINUHID_EVENT ev)
    {
        uint8_t buf[8] = {ev->ReportId};
        DWORD len = 0;
        if (ev->ReportId == kRidBlockLoad) {
            // Answers the Create New Effect that came just before.
            buf[1] = (uint8_t)m_lastLoaded;
            buf[2] = m_lastLoaded ? 1 : 2;  // 1 success, 2 full
            uint16_t free = (uint16_t)(m_fx.FreeCount() * 64);
            memcpy(buf + 3, &free, 2);
            len = 5;
        } else if (ev->ReportId == kRidPool) {
            uint16_t pool = kMaxEffects * 64;
            memcpy(buf + 1, &pool, 2);
            buf[3] = kMaxEffects;  // simultaneous effects
            buf[4] = 0x01;         // device-managed pool, no shared parameter blocks
            len = 5;
        }
        WinUHidCompleteReadEvent(dev, ev, len ? buf : nullptr, len);
    }

    bool OnWrite(const uint8_t* d, ULONG n)
    {
        if (n < 2) return false;
        auto u16 = [&](int o) { return (uint16_t)(d[o] | (d[o + 1] << 8)); };
        switch (d[0]) {
        case kRidCreateEffect:  // SET_FEATURE: [type][byte count]
            m_lastLoaded = m_fx.Allocate(d[1]);
            return true;
        case kRidSetEffect:
            if (n < 10) return false;
            m_fx.Edit(d[1], [&](auto& e) {
                e.type = d[2];
                e.durationMs = u16(3);
                e.gain = d[9];
            });
            return true;
        case kRidSetPeriodic:
            if (n < 4) return false;
            m_fx.Edit(d[1], [&](auto& e) {
                e.magnitude = d[2];
                e.offset = (int8_t)d[3];
            });
            return true;
        case kRidSetConstant:
            if (n < 4) return false;
            m_fx.Edit(d[1], [&](auto& e) { e.magnitude = (int16_t)u16(2); });
            return true;
        case kRidSetRamp:
            if (n < 4) return false;
            m_fx.Edit(d[1], [&](auto& e) { e.magnitude = std::max(std::abs((int8_t)d[2]), std::abs((int8_t)d[3])); });
            return true;
        case kRidEffectOp:
            if (n < 4) return false;
            if (d[2] == 2) m_fx.StopAll(false);  // start solo stops everything else first
            m_fx.Edit(d[1], [&](auto& e) {
                e.playing = d[2] != 3;
                e.loops = d[3] ? d[3] : 1;
                e.start = EffectEngine::Clock::now();
            });
            return true;
        case kRidBlockFree:
            m_fx.Edit(d[1], [](auto& e) { e = EffectEngine::Effect{}; });
            return true;
        case kRidDeviceControl:
            switch (d[1]) {
            case 1: m_fx.SetEnabled(true); break;
            case 2: m_fx.SetEnabled(false); break;
            case 3: m_fx.StopAll(false); break;
            case 4: m_fx.StopAll(true); m_fx.SetEnabled(true); m_fx.SetPaused(false); break;
            case 5: m_fx.SetPaused(true); break;
            case 6: m_fx.SetPaused(false); break;
            }
            return true;
        case kRidDeviceGain:
            m_fx.SetDeviceGain(d[1]);
            return true;
        default:
            return true;  // envelopes and anything else: accepted, not needed for rumble
        }
    }

    PWINUHID_DEVICE m_dev = nullptr;
    EffectEngine m_fx;
    int m_lastLoaded = 0;
};

}  // namespace

std::unique_ptr<OutputBackend> CreateXInputOutput();
std::unique_ptr<OutputBackend> CreateDualSenseOutput();

std::unique_ptr<OutputBackend> CreateOutput(OutputMode mode)
{
    switch (mode) {
    case OutputMode::XInput: return CreateXInputOutput();
    case OutputMode::DualSense: return CreateDualSenseOutput();
    default: return std::make_unique<DirectInputOutput>();
    }
}

const char* OutputModeName(OutputMode m)
{
    switch (m) {
    case OutputMode::XInput: return "XInput";
    case OutputMode::DualSense: return "DualSense";
    default: return "DirectInput";
    }
}

bool WinUHidAvailable()
{
    return WinUHidGetDriverInterfaceVersion() != 0;
}
