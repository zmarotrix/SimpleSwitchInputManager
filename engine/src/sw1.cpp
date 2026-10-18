#include "sw1.h"

#include "devenum.h"
#include "hidhide.h"
#include "protocol.h"
#include "sw1_rumble_tables.h"

#include <hidclass.h>
#include <hidsdi.h>

#include <algorithm>
#include <chrono>
#include <cstring>

// Protocol: dekuNukem's Nintendo_Switch_Reverse_Engineering notes and SDL's
// SDL_hidapi_switch.c (calibration layout, IMU axes, rumble encoding).

namespace {

constexpr DWORD kReportLen = 49;  // Bluetooth output report size (0x01 / 0x10)

// ---------------------------------------------------------------------------
// Decoding (input report 0x30, "full mode")
// ---------------------------------------------------------------------------

// Stick calibration is three 12-bit pairs; the order differs per stick.
void ParseCal(const uint8_t* d, bool leftOrder, StickCal& out)
{
    auto x = [&](int i) { return ((d[i + 1] << 8) & 0xF00) | d[i]; };
    auto y = [&](int i) { return (d[i + 2] << 4) | (d[i + 1] >> 4); };
    int maxX, maxY, cX, cY, minX, minY;
    if (leftOrder) {  // max, center, min
        maxX = x(0); maxY = y(0); cX = x(3); cY = y(3); minX = x(6); minY = y(6);
    } else {  // center, min, max
        cX = x(0); cY = y(0); minX = x(3); minY = y(3); maxX = x(6); maxY = y(6);
    }
    if (cX == 0xFFF || cY == 0xFFF || maxX == 0xFFF || minX == 0xFFF) return;  // unprogrammed: keep defaults
    // Like SDL, treat 70% of the calibrated travel as full deflection.
    out.cx = cX; out.cy = cY;
    out.maxX = std::max(1, (int)(maxX * 0.7f)); out.maxY = std::max(1, (int)(maxY * 0.7f));
    out.minX = std::max(1, (int)(minX * 0.7f)); out.minY = std::max(1, (int)(minY * 0.7f));
}

}  // namespace

class Sw1Decoder {
public:
    Sw1Decoder(const StickCal& l, const StickCal& r) : m_left(l), m_right(r) { QueryPerformanceFrequency(&m_qpf); }

    // r: full report including the 0x30 id byte.
    PadInput Decode(const uint8_t* r)
    {
        PadInput in;
        // Same bit layout as Switch 2's button word. Drop "charging grip" (byte 4 bit 7).
        in.buttons = (r[3] | (r[4] << 8) | (r[5] << 16)) & ~0x8000u;
        int lx, ly, rx, ry;
        DecodeStick(r + 6, lx, ly);
        DecodeStick(r + 9, rx, ry);
        m_left.Apply(lx, ly, in.lx, in.ly);
        m_right.Apply(rx, ry, in.rx, in.ry);
        in.lt = (in.buttons & kBtnZL) ? 1.f : 0.f;
        in.rt = (in.buttons & kBtnZR) ? 1.f : 0.f;

        // Battery: high nibble of byte 2, level 0..8 in steps of 2.
        int level = (r[2] >> 5) & 0x07;
        in.batteryPercent = std::min(100, level * 25);
        in.wired = false;

        // Three IMU samples per report; use the newest. Raw axes -> DSU convention
        // (derived from SDL's mapping: SDL = (-Y, Z, -X); DSU accel = -SDL, gyro = (x, -y, -z)).
        const uint8_t* s = r + 13 + 24;
        int16_t a[3], g[3];
        memcpy(a, s, 6);
        memcpy(g, s + 6, 6);
        const float kAccel = 1.f / 4096.f;    // g per LSB (±8 g)
        const float kGyro = 1.f / 14.2842f;   // deg/s per LSB (SDL's default scale)
        in.hasMotion = true;
        in.accel[0] = a[1] * kAccel;
        in.accel[1] = -a[2] * kAccel;
        in.accel[2] = a[0] * kAccel;
        in.gyro[0] = -g[1] * kGyro;
        in.gyro[1] = -g[2] * kGyro;
        in.gyro[2] = g[0] * kGyro;
        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);
        in.motionTimestampUs = (uint64_t)(qpc.QuadPart * 1000000 / m_qpf.QuadPart);
        return in;
    }

private:
    StickCal m_left, m_right;
    LARGE_INTEGER m_qpf{};
};

// ---------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------

std::vector<Sw1DeviceInfo> FindSwitch1Controllers()
{
    static const struct {
        const wchar_t* tag;
        uint16_t pid;
        const char* model;
    } kinds[] = {{L"_VID&0002057E_PID&2009", 0x2009, "Switch 1 Pro Controller"},
                 {L"_VID&0002057E_PID&2006", 0x2006, "Switch 1 Joy-Con (L)"},
                 {L"_VID&0002057E_PID&2007", 0x2007, "Switch 1 Joy-Con (R)"}};
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    std::vector<Sw1DeviceInfo> out;
    for (auto& path : InterfaceList(hidGuid, nullptr)) {
        for (auto& k : kinds) {
            if (!ContainsNoCase(path, k.tag)) continue;
            Sw1DeviceInfo info;
            info.hidPath = path;
            info.hidInstanceId = InterfaceInstanceId(path);
            info.pid = k.pid;
            info.model = k.model;
            // The Bluetooth address is the tail of the parent BTHENUM instance:
            // ...\7&3562C725&0&E8DA203C7A51_C00000000
            DEVINST node, parent;
            if (CM_Locate_DevNodeW(&node, (DEVINSTID_W)info.hidInstanceId.c_str(), CM_LOCATE_DEVNODE_NORMAL) ==
                    CR_SUCCESS &&
                CM_Get_Parent(&parent, node, 0) == CR_SUCCESS) {
                std::wstring pid = NodeId(parent);
                size_t amp = pid.find_last_of(L'&'), us = pid.find(L'_', amp);
                if (amp != std::wstring::npos && us != std::wstring::npos && us - amp - 1 == 12) {
                    for (size_t i = amp + 1; i < us; i++) info.address += (char)towupper(pid[i]);
                }
            }
            if (!info.address.empty()) out.push_back(info);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Rumble encoding (4 bytes per side)
// ---------------------------------------------------------------------------

static void EncodeSw1Rumble(float low, float high, uint8_t out[4])
{
    if (low <= 0 && high <= 0) {
        const uint8_t neutral[4] = {0x00, 0x01, 0x40, 0x40};
        memcpy(out, neutral, 4);
        return;
    }
    auto lookup = [](const uint16_t table[101][2], float v) {
        uint16_t a = (uint16_t)std::clamp(v * 65535.f, 0.f, 65535.f);
        for (int i = 0; i < 101; i++)
            if (a <= table[i][0]) return table[i][1];
        return table[100][1];
    };
    const uint16_t hfFreq = 0x0074;  // ~320 Hz, as SDL uses
    const uint8_t lfFreq = 0x3D;     // ~160 Hz
    uint8_t hfAmp = (uint8_t)lookup(kSw1HighAmp, high);
    uint16_t lfAmp = lookup(kSw1LowAmp, low);
    out[0] = hfFreq & 0xFF;
    out[1] = hfAmp | ((hfFreq >> 8) & 0x01);
    out[2] = lfFreq | ((lfAmp >> 8) & 0x80);
    out[3] = lfAmp & 0xFF;
}

// ---------------------------------------------------------------------------
// Sw1Controller
// ---------------------------------------------------------------------------

Sw1Controller::Sw1Controller(Sw1DeviceInfo info, bool hide)
    : m_info(std::move(info)), m_hide(hide), m_stop(CreateEventW(nullptr, TRUE, FALSE, nullptr))
{
}

Sw1Controller::~Sw1Controller()
{
    Stop();
    CloseHandle(m_stop);
}

void Sw1Controller::Start()
{
    m_thread = std::thread([this] {
        Run();
        m_finished = true;
    });
}

void Sw1Controller::Stop()
{
    SetEvent(m_stop);
    if (m_thread.joinable()) m_thread.join();
}

bool Sw1Controller::Write(const uint8_t* data, DWORD len)
{
    std::lock_guard<std::mutex> lock(m_writeLock);
    if (m_hid == INVALID_HANDLE_VALUE) return false;
    std::vector<uint8_t> out(std::max(len, m_outLen), 0);
    memcpy(out.data(), data, len);
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD done = 0;
    bool ok = WriteFile(m_hid, out.data(), (DWORD)out.size(), nullptr, &ov) || GetLastError() == ERROR_IO_PENDING;
    if (ok && WaitForSingleObject(ov.hEvent, 200) != WAIT_OBJECT_0) {
        CancelIoEx(m_hid, &ov);
        ok = false;
    }
    ok = ok && GetOverlappedResult(m_hid, &ov, &done, TRUE);
    CloseHandle(ov.hEvent);
    return ok;
}

bool Sw1Controller::Subcommand(uint8_t id, const uint8_t* data, uint8_t len, std::vector<uint8_t>* reply)
{
    std::lock_guard<std::mutex> oneAtATime(m_subLock);  // replies are matched by id

    for (int attempt = 0; attempt < 3; attempt++) {
        uint8_t pkt[kReportLen] = {0x01};
        {
            std::lock_guard<std::mutex> lock(m_writeLock);
            pkt[1] = m_counter++ & 0x0F;
            memcpy(pkt + 2, m_rumble, 8);
        }
        pkt[10] = id;
        memcpy(pkt + 11, data, std::min<int>(len, kReportLen - 11));
        {
            std::lock_guard<std::mutex> lock(m_replyLock);
            m_reply.clear();
        }
        if (!Write(pkt, sizeof(pkt))) continue;

        std::vector<uint8_t> r;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
        if (m_loopRunning) {
            // The input loop hands 0x21 reports over.
            std::unique_lock<std::mutex> lock(m_replyLock);
            m_replyCv.wait_until(lock, deadline, [&] { return m_reply.size() > 14 && m_reply[14] == id; });
            r = m_reply;
        } else {
            // During setup nobody else reads: wait for the reply here.
            OVERLAPPED ov = {};
            ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            std::vector<uint8_t> in(m_inLen);
            uint8_t* buf = in.data();
            while (std::chrono::steady_clock::now() < deadline) {
                DWORD got = 0;
                ResetEvent(ov.hEvent);
                if (!ReadFile(m_hid, buf, m_inLen, nullptr, &ov) && GetLastError() != ERROR_IO_PENDING) break;
                if (WaitForSingleObject(ov.hEvent, 100) != WAIT_OBJECT_0) {
                    CancelIoEx(m_hid, &ov);
                    GetOverlappedResult(m_hid, &ov, &got, TRUE);
                    continue;
                }
                if (GetOverlappedResult(m_hid, &ov, &got, FALSE) && got > 15 && buf[0] == 0x21 && buf[14] == id) {
                    r.assign(buf, buf + got);
                    break;
                }
            }
            CloseHandle(ov.hEvent);
        }
        if (r.size() > 14 && r[14] == id && (r[13] & 0x80)) {
            if (reply) reply->assign(r.begin() + 15, r.end());
            return true;
        }
    }
    return false;
}

bool Sw1Controller::SpiRead(uint32_t addr, uint8_t len, uint8_t* out)
{
    uint8_t req[5] = {(uint8_t)addr, (uint8_t)(addr >> 8), (uint8_t)(addr >> 16), (uint8_t)(addr >> 24), len};
    std::vector<uint8_t> r;
    if (!Subcommand(0x10, req, sizeof(req), &r) || r.size() < 5u + len) return false;
    uint32_t echoed;
    memcpy(&echoed, r.data(), 4);
    if (echoed != addr) return false;
    memcpy(out, r.data() + 5, len);
    return true;
}

bool Sw1Controller::ReadIdentity(std::string& id, std::string& bodyColor)
{
    id = m_info.Id();
    uint8_t c[3];
    if (SpiRead(0x6050, 3, c)) {
        char buf[8];
        snprintf(buf, sizeof(buf), "#%02X%02X%02X", c[0], c[1], c[2]);
        bodyColor = buf;
    }
    return true;
}

void Sw1Controller::SetPlayerLeds(uint8_t pattern)
{
    Subcommand(0x30, &pattern, 1);
}

bool Sw1Controller::SendRumble(const RumbleCommand& c)
{
    uint8_t pkt[kReportLen] = {0x10};
    {
        std::lock_guard<std::mutex> lock(m_writeLock);
        EncodeSw1Rumble(c.lowL, c.highL, m_rumble);
        EncodeSw1Rumble(c.lowR, c.highR, m_rumble + 4);
        pkt[1] = m_counter++ & 0x0F;
        memcpy(pkt + 2, m_rumble, 8);
    }
    return Write(pkt, sizeof(pkt));
}

void Sw1Controller::HandleReport(const uint8_t* r, DWORD n)
{
    if (r[0] == 0x30 && n >= 49) {
        if (m_session && m_decoder) m_session->Process(m_decoder->Decode(r));
    } else if (r[0] == 0x21 && n > 15) {
        {
            std::lock_guard<std::mutex> lock(m_replyLock);
            m_reply.assign(r, r + n);
        }
        m_replyCv.notify_all();
    }
}

void Sw1Controller::Run()
{
    Log("%s %s: taking over (enabled in SSIM)", m_info.model, m_info.address.c_str());
    m_hid = CreateFileW(m_info.hidPath.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (m_hid == INVALID_HANDLE_VALUE) {
        Log("  could not open the controller (%lu)", GetLastError());
        return;
    }
    // Windows wants buffers of the sizes the HID descriptor declares (larger than
    // the 49 bytes the controller actually uses).
    PHIDP_PREPARSED_DATA pp = nullptr;
    HIDP_CAPS caps = {};
    if (HidD_GetPreparsedData(m_hid, &pp)) {
        HidP_GetCaps(pp, &caps);
        HidD_FreePreparsedData(pp);
    }
    m_inLen = std::max<DWORD>(caps.InputReportByteLength, 64);
    m_outLen = std::max<DWORD>(caps.OutputReportByteLength, kReportLen);
    if (m_hide && !HidHide::Hide(m_info.hidInstanceId)) Log("  HidHide: could not hide it; games may see it twice");

    // Full mode with motion and vibration on.
    const uint8_t fullMode = 0x30, on = 0x01;
    bool ok = Subcommand(0x03, &fullMode, 1) && Subcommand(0x40, &on, 1);
    Subcommand(0x48, &on, 1);
    if (!ok) Log("  setup commands were not acknowledged; trying anyway");

    // Stick calibration: user calibration (with magic 0xB2 0xA1) wins over factory.
    StickCal left, right;
    uint8_t user[22] = {}, factory[18] = {};
    bool haveFactory = SpiRead(0x603D, sizeof(factory), factory);
    bool haveUser = SpiRead(0x8010, sizeof(user), user);
    if (haveFactory) {
        ParseCal(factory, true, left);
        ParseCal(factory + 9, false, right);
    }
    if (haveUser && user[0] == 0xB2 && user[1] == 0xA1) ParseCal(user + 2, true, left);
    if (haveUser && user[11] == 0xB2 && user[12] == 0xA1) ParseCal(user + 13, false, right);
    Log("  stick calibration: %s", haveFactory || haveUser ? "read" : "defaults");

    Sw1Decoder decoder(left, right);
    SwitchSession session(*this, false, m_info.model, kVirtualPidSwitch1Pro, this, 40);
    m_decoder = &decoder;
    session.Begin(66);
    m_session = &session;

    // Input loop.
    m_loopRunning = true;
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE waits[2] = {ov.hEvent, m_stop};
    std::vector<uint8_t> in(m_inLen);
    uint8_t* buf = in.data();
    for (;;) {
        DWORD got = 0;
        ResetEvent(ov.hEvent);
        if (!ReadFile(m_hid, buf, m_inLen, nullptr, &ov) && GetLastError() != ERROR_IO_PENDING) {
            Log("%s %s disconnected (%lu)", m_info.model, m_info.address.c_str(), GetLastError());
            break;
        }
        if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0) {
            CancelIoEx(m_hid, &ov);
            GetOverlappedResult(m_hid, &ov, &got, TRUE);
            break;
        }
        if (!GetOverlappedResult(m_hid, &ov, &got, FALSE)) {
            Log("%s %s disconnected (%lu)", m_info.model, m_info.address.c_str(), GetLastError());
            break;
        }
        HandleReport(buf, got);
    }
    CloseHandle(ov.hEvent);
    m_loopRunning = false;
    m_session = nullptr;
    session.End();  // stops rumble
    m_decoder = nullptr;

    // Hand it back exactly as we found it: IMU off, simple (DirectInput) mode, visible.
    const uint8_t off = 0x00, simpleMode = 0x3F;
    Subcommand(0x40, &off, 1);
    if (Subcommand(0x03, &simpleMode, 1)) Log("%s %s: released (simple mode restored)", m_info.model, m_info.address.c_str());
    if (m_hide) HidHide::Unhide(m_info.hidInstanceId);
    {
        std::lock_guard<std::mutex> lock(m_writeLock);
        CloseHandle(m_hid);
        m_hid = INVALID_HANDLE_VALUE;
    }
}

bool Sw1SetInputMode(const std::string& id, bool simple)
{
    for (auto& dev : FindSwitch1Controllers()) {
        if (dev.Id() != id) continue;
        HANDLE h = CreateFileW(dev.hidPath.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        PHIDP_PREPARSED_DATA pp = nullptr;
        HIDP_CAPS caps = {};
        if (HidD_GetPreparsedData(h, &pp)) {
            HidP_GetCaps(pp, &caps);
            HidD_FreePreparsedData(pp);
        }
        std::vector<uint8_t> pkt(std::max<DWORD>(caps.OutputReportByteLength, kReportLen), 0);
        const uint8_t head[12] = {0x01, 0x00, 0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40,
                                  0x03, (uint8_t)(simple ? 0x3F : 0x30)};
        memcpy(pkt.data(), head, sizeof(head));
        DWORD done = 0;
        bool ok = WriteFile(h, pkt.data(), (DWORD)pkt.size(), &done, nullptr) != FALSE;
        CloseHandle(h);
        Log("%s %s: switched to %s mode on request", dev.model, dev.address.c_str(), simple ? "simple" : "full");
        return ok;
    }
    return false;
}
