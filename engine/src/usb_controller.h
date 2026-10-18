#pragma once

#include "common.h"
#include "forwarder.h"

#include <atomic>
#include <thread>
#include <vector>

// Paths for one physical Pro Controller 2 on USB.
struct DeviceInfo {
    std::wstring hidPath;         // \\?\HID#VID_057E&PID_2069&MI_00#...
    std::wstring hidInstanceId;   // HID\VID_057E&PID_2069&MI_00\...
    std::wstring winusbPath;      // vendor command interface (MI_01)
};

std::vector<DeviceInfo> FindControllers();

// Owns one USB controller: wakes it up, then forwards its 250 Hz input on a
// dedicated high-priority thread.
class UsbController : public PadSource, public RumbleSink {
public:
    UsbController(DeviceInfo info, bool hide);
    ~UsbController() override;

    void Start() override;
    void Stop() override;
    bool Finished() const override { return m_finished; }
    const std::wstring& Key() const override { return m_info.hidPath; }
    bool SendRumble(const RumbleCommand& c) override;

private:
    void Run();

    DeviceInfo m_info;
    bool m_hide;
    HANDLE m_hid = INVALID_HANDLE_VALUE;
    HANDLE m_writeEvent;
    Sw2RumbleEncoder m_encoder{true};
    std::thread m_thread;
    HANDLE m_stop;
    std::atomic<bool> m_finished{false};
};
