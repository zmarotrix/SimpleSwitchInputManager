#include "ble_controller.h"

#include "commands.h"
#include "decode_sw2.h"
#include "session.h"

#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>

using namespace winrt;
using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Bluetooth::Advertisement;
using namespace winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
using winrt::Windows::Storage::Streams::Buffer;
using winrt::Windows::Storage::Streams::IBuffer;

// Nintendo's Bluetooth company ID and the controller's GATT layout.
constexpr uint16_t kNintendoCompanyId = 0x0553;
static const guid kServiceUuid{0xab7de9be, 0x89fe, 0x49ad, {0x82, 0x8f, 0x11, 0x8f, 0x09, 0xdf, 0x7f, 0xd0}};
static const guid kInputUuid{0xab7de9be, 0x89fe, 0x49ad, {0x82, 0x8f, 0x11, 0x8f, 0x09, 0xdf, 0x7f, 0xd2}};
static const guid kVibrationUuid{0xcc483f51, 0x9258, 0x427d, {0xa9, 0x39, 0x63, 0x0c, 0x31, 0xf7, 0x2b, 0x05}};

// Fixed pairing keys used by the Switch 2 controllers (from Switch2Connect / ndeadly's research).
static const uint8_t kLtk1[] = {0x00, 0xea, 0xbd, 0x47, 0x13, 0x89, 0x35, 0x42, 0xc6,
                                0x79, 0xee, 0x07, 0xf2, 0x53, 0x2c, 0x6c, 0x31};
static const uint8_t kLtk2[] = {0x00, 0x40, 0xb0, 0x8a, 0x5f, 0xcd, 0x1f, 0x9b, 0x41,
                                0x12, 0x5c, 0xac, 0xc6, 0x3f, 0x38, 0xa0, 0x73};

static uint64_t g_hostAddress = 0;

static void EnsureApartment()
{
    try {
        init_apartment(apartment_type::multi_threaded);
    } catch (hresult_error const&) {
        // Already initialised on this thread.
    }
}

// ---------------------------------------------------------------------------
// Advertisement watcher
// ---------------------------------------------------------------------------

namespace Ble {

static std::mutex g_watchLock;
static BluetoothLEAdvertisementWatcher g_watcher{nullptr};
static std::function<void(uint64_t, bool)> g_onFound;

static void OnAdvertisement(BluetoothLEAdvertisementWatcher const&, BluetoothLEAdvertisementReceivedEventArgs const& a)
{
    for (auto const& md : a.Advertisement().ManufacturerData()) {
        if (md.CompanyId() != kNintendoCompanyId) continue;
        IBuffer buf = md.Data();
        if (buf.Length() < 16) continue;
        const uint8_t* d = buf.data();
        uint16_t vid = d[3] | (d[4] << 8), pid = d[5] | (d[6] << 8);
        if (vid != kNintendoVid || pid != kPro2Pid) continue;
        uint64_t reconnect = 0;
        for (int i = 5; i >= 0; i--) reconnect = (reconnect << 8) | d[10 + i];
        if (reconnect == 0) {
            g_onFound(a.BluetoothAddress(), true);  // sync button held
        } else if (reconnect == g_hostAddress) {
            g_onFound(a.BluetoothAddress(), false);  // paired to this PC, woken by a button
        }
        // Otherwise it belongs to another host (e.g. a Switch 2): leave it alone.
    }
}

bool StartWatcher(std::function<void(uint64_t, bool)> onFound)
{
    EnsureApartment();
    try {
        auto adapter = BluetoothAdapter::GetDefaultAsync().get();
        if (!adapter || !adapter.IsLowEnergySupported()) {
            Log("Bluetooth: no Bluetooth LE adapter found; wireless support off");
            return false;
        }
        g_hostAddress = adapter.BluetoothAddress();
        std::lock_guard<std::mutex> lock(g_watchLock);
        g_onFound = std::move(onFound);
        g_watcher = BluetoothLEAdvertisementWatcher();
        g_watcher.ScanningMode(BluetoothLEScanningMode::Active);
        g_watcher.Received(OnAdvertisement);
        g_watcher.Start();
        Log("Bluetooth: watching for controllers (hold sync to pair, or press a button on a paired one)");
        return true;
    } catch (hresult_error const& e) {
        Log("Bluetooth: watcher failed to start (0x%08X)", (unsigned)e.code());
        return false;
    }
}

void StopWatcher()
{
    std::lock_guard<std::mutex> lock(g_watchLock);
    if (g_watcher) {
        try {
            g_watcher.Stop();
        } catch (hresult_error const&) {
        }
        g_watcher = nullptr;
    }
}

void KeepWatcherAlive()
{
    std::lock_guard<std::mutex> lock(g_watchLock);
    if (!g_watcher) return;
    try {
        auto st = g_watcher.Status();
        if (st == BluetoothLEAdvertisementWatcherStatus::Aborted || st == BluetoothLEAdvertisementWatcherStatus::Stopped) {
            g_watcher.Start();
        }
    } catch (hresult_error const&) {
        // Radio still off; try again next time.
    }
}

}  // namespace Ble

// ---------------------------------------------------------------------------
// Command channel over GATT
// ---------------------------------------------------------------------------

class BleCommandChannel : public CommandTransport {
public:
    BleCommandChannel(GattCharacteristic write, GattCharacteristic reply) : m_write(write), m_reply(reply) {}

    ~BleCommandChannel() override
    {
        if (m_token) m_reply.ValueChanged(m_token);
    }

    bool Open()
    {
        m_token = m_reply.ValueChanged([this](GattCharacteristic const&, GattValueChangedEventArgs const& a) {
            IBuffer b = a.CharacteristicValue();
            std::lock_guard<std::mutex> lock(m_lock);
            m_replies.emplace_back(b.data(), b.data() + b.Length());
            m_cv.notify_all();
        });
        auto st = m_reply
                      .WriteClientCharacteristicConfigurationDescriptorAsync(
                          GattClientCharacteristicConfigurationDescriptorValue::Notify)
                      .get();
        return st == GattCommunicationStatus::Success;
    }

    using CommandTransport::Send;
    bool Send(uint8_t cmd, uint8_t sub, const uint8_t* data, uint8_t len, std::vector<uint8_t>* reply) override
    {
        {
            std::lock_guard<std::mutex> lock(m_lock);
            m_replies.clear();
        }
        Buffer pkt(8 + len);
        uint8_t* p = pkt.data();
        const uint8_t header[8] = {cmd, 0x91, 0x01 /* Bluetooth */, sub, 0x00, len, 0x00, 0x00};
        memcpy(p, header, 8);
        memcpy(p + 8, data, len);
        pkt.Length(8 + len);
        if (m_write.WriteValueAsync(pkt, GattWriteOption::WriteWithoutResponse).get() !=
            GattCommunicationStatus::Success) {
            return false;
        }

        std::unique_lock<std::mutex> lock(m_lock);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        for (;;) {
            for (auto& r : m_replies) {
                if (r.size() >= 8 && r[0] == cmd) {
                    if (r[1] != 0x01) return false;
                    if (reply) reply->assign(r.begin() + 8, r.end());
                    return true;
                }
            }
            if (m_cv.wait_until(lock, deadline) == std::cv_status::timeout) return false;
        }
    }

private:
    GattCharacteristic m_write;
    GattCharacteristic m_reply;
    event_token m_token{};
    std::mutex m_lock;
    std::condition_variable m_cv;
    std::deque<std::vector<uint8_t>> m_replies;
};

// ---------------------------------------------------------------------------
// BleController
// ---------------------------------------------------------------------------

std::wstring BleController::KeyFor(uint64_t address)
{
    wchar_t buf[32];
    swprintf_s(buf, L"BT:%012llX", address);
    return buf;
}

struct BleController::Vibration {
    GattCharacteristic ch{nullptr};
};

BleController::BleController(uint64_t address, bool pairingMode, bool allowPairing)
    : m_address(address),
      m_pairingMode(pairingMode),
      m_allowPairing(allowPairing),
      m_key(KeyFor(address)),
      m_stop(CreateEventW(nullptr, TRUE, FALSE, nullptr))
{
}

BleController::~BleController()
{
    Stop();
    CloseHandle(m_stop);
}

// Pro Controller vibration characteristic: 0x00, then left and right actuator blocks.
bool BleController::SendRumble(const RumbleCommand& c)
{
    static thread_local bool apartment = (EnsureApartment(), true);
    (void)apartment;
    if (!m_vibration || !m_vibration->ch) return false;
    uint8_t left[16], right[16];
    m_encoder.Encode(c, left, right);
    try {
        Buffer b(33);
        uint8_t* p = b.data();
        p[0] = 0x00;
        memcpy(p + 1, left, 16);
        memcpy(p + 17, right, 16);
        b.Length(33);
        return m_vibration->ch.WriteValueAsync(b, GattWriteOption::WriteWithoutResponse).get() ==
               GattCommunicationStatus::Success;
    } catch (hresult_error const&) {
        return false;
    }
}

void BleController::Start()
{
    m_thread = std::thread([this] {
        Run();
        m_finished = true;
    });
}

void BleController::Stop()
{
    SetEvent(m_stop);
    if (m_thread.joinable()) m_thread.join();
}

void BleController::Run()
{
    EnsureApartment();
    Log("Controller found over Bluetooth (%012llX)%s", m_address, m_pairingMode ? ", sync mode" : "");

    HANDLE disconnected = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::unique_ptr<BleCommandChannel> ch;
    std::unique_ptr<Sw2DeviceOps> ops;
    std::unique_ptr<SwitchSession> session;
    std::unique_ptr<Sw2Decoder> decoder;
    BluetoothLEDevice dev{nullptr};
    GattCharacteristic input{nullptr};
    event_token inputToken{}, statusToken{};

    try {
        dev = BluetoothLEDevice::FromBluetoothAddressAsync(m_address).get();
        if (!dev) throw hresult_error(E_FAIL, L"device not reachable");
        statusToken = dev.ConnectionStatusChanged([disconnected](BluetoothLEDevice const& d, auto&&) {
            if (d.ConnectionStatus() == BluetoothConnectionStatus::Disconnected) SetEvent(disconnected);
        });

        // Hold the link open for the whole session (otherwise Windows may drop it
        // between GATT calls), then discover the controller's service.
        auto gatt = GattSession::FromDeviceIdAsync(dev.BluetoothDeviceId()).get();
        gatt.MaintainConnection(true);
        GattDeviceServicesResult services{nullptr};
        for (int attempt = 0; attempt < 4; attempt++) {
            auto mode = attempt < 2 ? BluetoothCacheMode::Uncached : BluetoothCacheMode::Cached;
            services = dev.GetGattServicesForUuidAsync(kServiceUuid, mode).get();
            if (services.Status() == GattCommunicationStatus::Success && services.Services().Size() > 0) break;
            Log("  service discovery attempt %d: status %d, %u service(s)", attempt + 1, (int)services.Status(),
                services.Services() ? services.Services().Size() : 0u);
            if (WaitForSingleObject(m_stop, 500) == WAIT_OBJECT_0) throw hresult_error(E_ABORT, L"stopping");
        }
        if (services.Status() != GattCommunicationStatus::Success || services.Services().Size() == 0)
            throw hresult_error(E_FAIL, L"controller service not found");
        // Sort characteristics by handle, as the command channel is identified by position, not UUID.
        auto chars = services.Services().GetAt(0).GetCharacteristicsAsync(BluetoothCacheMode::Uncached).get();
        if (chars.Status() != GattCommunicationStatus::Success) throw hresult_error(E_FAIL, L"characteristics");
        std::vector<GattCharacteristic> all(begin(chars.Characteristics()), end(chars.Characteristics()));
        std::sort(all.begin(), all.end(),
                  [](auto& a, auto& b) { return a.AttributeHandle() < b.AttributeHandle(); });
        std::vector<GattCharacteristic> writes, notifies;
        for (auto& c : all) {
            auto p = c.CharacteristicProperties();
            if ((p & (GattCharacteristicProperties::WriteWithoutResponse | GattCharacteristicProperties::Write)) !=
                GattCharacteristicProperties::None)
                writes.push_back(c);
            if ((p & GattCharacteristicProperties::Notify) != GattCharacteristicProperties::None) notifies.push_back(c);
            if (c.Uuid() == kInputUuid) input = c;
            if (c.Uuid() == kVibrationUuid) m_vibration = std::make_unique<Vibration>(Vibration{c});
        }
        if (writes.size() < 2 || notifies.size() < 3 || !input) throw hresult_error(E_FAIL, L"unexpected GATT layout");

        // Ask Windows for its fastest connection interval (15 ms, about 65 reports/s).
        auto fast = dev.RequestPreferredConnectionParameters(BluetoothLEPreferredConnectionParameters::ThroughputOptimized());

        ch = std::make_unique<BleCommandChannel>(writes[1], notifies[2]);
        if (!ch->Open()) throw hresult_error(E_FAIL, L"command channel");

        // Same start-up sequence the console uses (via Switch2Connect), with the
        // full input report (buttons, sticks, motion, battery) selected.
        ch->Send(0x03, 0x0D, {0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
        ch->Send(0x07, 0x01, {});
        ch->Send(0x16, 0x01, {});
        ch->Send(0x15, 0x03, {0x00});
        ch->Send(0x0C, 0x02, {kFeatureMask, 0, 0, 0});
        ch->Send(0x11, 0x03, {});
        ch->Send(0x0A, 0x08, {0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x35,
                             0x00, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
        ch->Send(0x0C, 0x04, {kFeatureMask, 0, 0, 0});
        ch->Send(0x03, 0x0A, {0x05, 0, 0, 0});
        ch->Send(0x10, 0x01, {});
        ch->Send(0x01, 0x0C, {});
        StickCal leftCal = ReadStickCal(*ch, kCalLeftUser, kCalLeftFactory, "Left");
        StickCal rightCal = ReadStickCal(*ch, kCalRightUser, kCalRightFactory, "Right");

        if (m_pairingMode && m_allowPairing && g_hostAddress) {
            // Store this PC as the controller's host so a button press reconnects it.
            uint8_t mac[14] = {0x00, 0x02};
            for (int i = 0; i < 6; i++) mac[2 + i] = mac[8 + i] = (uint8_t)(g_hostAddress >> (8 * i));
            bool paired = ch->Send(0x15, 0x01, mac, (uint8_t)sizeof(mac), nullptr) &&
                          ch->Send(0x15, 0x04, kLtk1, (uint8_t)sizeof(kLtk1), nullptr) &&
                          ch->Send(0x15, 0x02, kLtk2, (uint8_t)sizeof(kLtk2), nullptr) && ch->Send(0x15, 0x03, {0x00});
            Log(paired ? "  paired to this PC: press any button to reconnect next time"
                       : "  pairing failed; hold sync again next time");
        }

        ops = std::make_unique<Sw2DeviceOps>(*ch);
        session = std::make_unique<SwitchSession>(*ops, false, "Switch 2 Pro Controller", kVirtualPidSwitch2Pro,
                                                  m_vibration ? this : nullptr, 15);
        session->Begin(65);
        decoder = std::make_unique<Sw2Decoder>(leftCal, rightCal, false);
        SwitchSession* sp = session.get();
        Sw2Decoder* dp = decoder.get();
        inputToken = input.ValueChanged([sp, dp](GattCharacteristic const&, GattValueChangedEventArgs const& a) {
            IBuffer b = a.CharacteristicValue();
            if (b.Length() >= 63) sp->Process(dp->Decode(b.data()));
        });
        if (input.WriteClientCharacteristicConfigurationDescriptorAsync(
                     GattClientCharacteristicConfigurationDescriptorValue::Notify)
                .get() != GattCommunicationStatus::Success)
            throw hresult_error(E_FAIL, L"input subscription");

        auto params = dev.GetConnectionParameters();
        Log("  Bluetooth link: %.2f ms interval (%s)", params.ConnectionInterval() * 1.25,
            fast.Status() == BluetoothLEPreferredConnectionParametersRequestStatus::Success ? "fast mode"
                                                                                             : "fast mode refused");

        if (dev.ConnectionStatus() == BluetoothConnectionStatus::Connected) {
            HANDLE waits[2] = {m_stop, disconnected};
            WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        }
        Log("Bluetooth controller disconnected (%012llX)", m_address);
        fast.Close();
    } catch (hresult_error const& e) {
        Log("  Bluetooth setup failed: %ls (0x%08X)", e.message().c_str(), (unsigned)e.code());
    }

    try {
        if (input && inputToken) input.ValueChanged(inputToken);
        if (dev && statusToken) dev.ConnectionStatusChanged(statusToken);
    } catch (hresult_error const&) {
    }
    if (session) session->End();  // stops rumble while the link may still be up
    session.reset();
    ch.reset();
    m_vibration.reset();
    if (dev) dev.Close();
    CloseHandle(disconnected);
}
