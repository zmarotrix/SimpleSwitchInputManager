#pragma once

#include "common.h"
#include "forwarder.h"

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

namespace Ble {

// Watches for Pro Controller 2 advertisements. `onFound` runs on a WinRT thread
// with the controller's address and whether it is in sync (pairing) mode.
// Controllers paired to a different host (e.g. a Switch 2) are ignored.
bool StartWatcher(std::function<void(uint64_t address, bool pairingMode)> onFound);
void StopWatcher();
void KeepWatcherAlive();  // restarts the watcher if Bluetooth was turned off and on

}  // namespace Ble

// Owns one controller connected over Bluetooth LE.
class BleController : public PadSource, public RumbleSink {
public:
    BleController(int slot, uint64_t address, bool pairingMode, bool allowPairing, OutputMode mode);
    ~BleController() override;

    void Start() override;
    void Stop() override;
    bool Finished() const override { return m_finished; }
    const std::wstring& Key() const override { return m_key; }

    static std::wstring KeyFor(uint64_t address);
    bool SendRumble(const uint8_t left[16], const uint8_t right[16]) override;

private:
    void Run();

    int m_slot;
    uint64_t m_address;
    bool m_pairingMode;
    bool m_allowPairing;
    OutputMode m_mode;
    struct Vibration;
    std::unique_ptr<Vibration> m_vibration;  // GATT characteristic, valid while connected
    std::wstring m_key;
    std::thread m_thread;
    HANDLE m_stop;
    std::atomic<bool> m_finished{false};
};
