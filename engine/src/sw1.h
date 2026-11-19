#pragma once

#include "common.h"
#include "forwarder.h"
#include "session.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

// Switch 1 controllers on Bluetooth (classic HID): Pro Controller, Joy-Con L/R,
// and the Switch Online N64 controller.
// SSIM only lists these until the user enables one ("managed" in the config);
// an enabled controller is hidden from games, driven in full mode, and put
// back exactly as it was (simple mode, visible) when released.

struct Sw1DeviceInfo {
    std::wstring hidPath;
    std::wstring hidInstanceId;
    std::string address;  // Bluetooth address, "112233445566"
    uint16_t pid = 0;
    const char* model = "";
    std::string Id() const { return "SW1-" + address; }
};

// Lists Switch 1 controllers from the device tree only; never talks to them.
std::vector<Sw1DeviceInfo> FindSwitch1Controllers();

// One-shot, user-requested: switch a controller SSIM is NOT managing between
// simple mode (0x3F, DirectInput) and full mode (0x30, Steam/SDL). Fixes a
// controller another program left in full mode.
bool Sw1SetInputMode(const std::string& id, bool simple);

class Sw1Controller : public PadSource, public RumbleSink, public DeviceOps {
public:
    Sw1Controller(Sw1DeviceInfo info, bool hide);
    ~Sw1Controller() override;

    // PadSource
    void Start() override;
    void Stop() override;
    bool Finished() const override { return m_finished; }
    const std::wstring& Key() const override { return m_info.hidPath; }
    std::string Id() const { return m_info.Id(); }

    // RumbleSink
    bool SendRumble(const RumbleCommand& c) override;

    // DeviceOps
    bool ReadIdentity(std::string& id, std::string& bodyColor) override;
    void SetPlayerLeds(uint8_t pattern) override;

private:
    void Run();
    bool Write(const uint8_t* data, DWORD len);
    // Sends a subcommand (output report 0x01) and returns its 0x21 reply data.
    bool Subcommand(uint8_t id, const uint8_t* data, uint8_t len, std::vector<uint8_t>* reply = nullptr);
    bool SpiRead(uint32_t addr, uint8_t len, uint8_t* out);
    void HandleReport(const uint8_t* r, DWORD n);

    Sw1DeviceInfo m_info;
    bool m_hide;
    HANDLE m_hid = INVALID_HANDLE_VALUE;
    DWORD m_inLen = 64, m_outLen = 49;  // from the HID descriptor
    HANDLE m_stop;
    std::thread m_thread;
    std::atomic<bool> m_finished{false};

    std::mutex m_writeLock;
    std::mutex m_subLock;
    uint8_t m_counter = 0;
    ULONGLONG m_lastModeRequest = 0;
    uint8_t m_rumble[8] = {0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40};  // neutral

    // Subcommand replies: while the input loop runs it hands 0x21 reports over here.
    std::mutex m_replyLock;
    std::condition_variable m_replyCv;
    std::vector<uint8_t> m_reply;
    std::atomic<bool> m_loopRunning{false};

    SwitchSession* m_session = nullptr;        // Pro Controller
    class JoyConUnit* m_joycon = nullptr;      // Joy-Con (grouped by the Joy-Con manager)
    class Sw1Decoder* m_decoder = nullptr;
};
