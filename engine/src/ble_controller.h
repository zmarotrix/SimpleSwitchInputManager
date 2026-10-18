#pragma once

#include "common.h"
#include "forwarder.h"

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

namespace Ble {

// Watches for Switch 2 controller advertisements (Pro Controller, Joy-Con L/R). `onFound` runs on a WinRT thread
// with the controller's address and whether it is in sync (pairing) mode.
// Controllers paired to a different host (e.g. a Switch 2) are ignored.
bool StartWatcher(std::function<void(uint64_t address, bool pairingMode, uint16_t pid)> onFound);
void StopWatcher();
void KeepWatcherAlive();  // restarts the watcher if Bluetooth was turned off and on

}  // namespace Ble

// Owns one controller connected over Bluetooth LE.
class BleController : public PadSource, public RumbleSink {
public:
    BleController(uint64_t address, bool pairingMode, bool allowPairing, uint16_t pid);
    ~BleController() override;

    void Start() override;
    void Stop() override;
    bool Finished() const override { return m_finished; }
    const std::wstring& Key() const override { return m_key; }

    static std::wstring KeyFor(uint64_t address);
    bool SendRumble(const RumbleCommand& c) override;

private:
    void Run();

    uint64_t m_address;
    bool m_pairingMode;
    bool m_allowPairing;
    uint16_t m_pid;  // 0x2069 Pro, 0x2067 Joy-Con L, 0x2066 Joy-Con R
    struct Vibration;
    std::unique_ptr<Vibration> m_vibration;
    Sw2RumbleEncoder m_encoder{false};  // GATT characteristic, valid while connected
    std::wstring m_key;
    std::thread m_thread;
    HANDLE m_stop;
    std::atomic<bool> m_finished{false};
};
