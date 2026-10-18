#include "dsu.h"

#include "protocol.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

// Protocol reference: https://v1993.github.io/cemuhook-protocol/

namespace {

constexpr uint16_t kProtocolVersion = 1001;
constexpr uint32_t kMsgVersion = 0x100000;
constexpr uint32_t kMsgPorts = 0x100001;
constexpr uint32_t kMsgData = 0x100002;
constexpr auto kClientTimeout = std::chrono::seconds(5);

using Clock = std::chrono::steady_clock;

struct Client {
    sockaddr_in addr;
    Clock::time_point allSlots{};
    Clock::time_point slot[kMaxControllers]{};
};

SOCKET g_sock = INVALID_SOCKET;
std::thread g_thread;
std::atomic<bool> g_running{false};
std::mutex g_lock;
std::vector<Client> g_clients;
MotionState g_slots[kMaxControllers];
uint32_t g_packet[kMaxControllers] = {};
const uint32_t g_serverId = GetTickCount();

uint32_t Crc32(const uint8_t* data, size_t len)
{
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320 & (0 - (crc & 1)));
    }
    return ~crc;
}

struct Writer {
    std::vector<uint8_t> b;
    void u8(uint8_t v) { b.push_back(v); }
    void u16(uint16_t v) { put(&v, 2); }
    void u32(uint32_t v) { put(&v, 4); }
    void u64(uint64_t v) { put(&v, 8); }
    void f32(float v) { put(&v, 4); }
    void put(const void* p, size_t n) { b.insert(b.end(), (const uint8_t*)p, (const uint8_t*)p + n); }
};

void Send(const sockaddr_in& to, uint32_t type, const std::vector<uint8_t>& payload)
{
    Writer w;
    w.put("DSUS", 4);
    w.u16(kProtocolVersion);
    w.u16((uint16_t)(payload.size() + 4));
    w.u32(0);  // CRC placeholder
    w.u32(g_serverId);
    w.u32(type);
    w.put(payload.data(), payload.size());
    uint32_t crc = Crc32(w.b.data(), w.b.size());
    memcpy(&w.b[8], &crc, 4);
    sendto(g_sock, (const char*)w.b.data(), (int)w.b.size(), 0, (const sockaddr*)&to, sizeof(to));
}

// The 11-byte header shared by port-info and data messages.
void SlotHeader(Writer& w, int slot, const MotionState& s)
{
    w.u8((uint8_t)slot);
    w.u8(s.connected ? 2 : 0);   // state
    w.u8(s.connected ? 2 : 0);   // model: full gyro
    w.u8(s.connected ? (s.bluetooth ? 2 : 1) : 0);  // connection: 1 USB, 2 Bluetooth
    uint8_t mac[6] = {0x53, 0x32, 0x50, 0x52, 0x00, (uint8_t)(slot + 1)};
    if (!s.connected) memset(mac, 0, 6);
    w.put(mac, 6);
    w.u8(s.connected ? s.battery : 0);
}

void SendPortInfo(const sockaddr_in& to, int slot)
{
    Writer w;
    SlotHeader(w, slot, g_slots[slot]);
    w.u8(0);
    Send(to, kMsgPorts, w.b);
}

uint8_t StickByte(float v)
{
    int b = (int)(128.f + v * 127.f);
    return (uint8_t)(b < 0 ? 0 : (b > 255 ? 255 : b));
}

void SendData(const sockaddr_in& to, int slot)
{
    const MotionState& s = g_slots[slot];
    const uint32_t b = s.buttons;
    Writer w;
    SlotHeader(w, slot, s);
    w.u8(1);
    w.u32(g_packet[slot]);

    // DS4-style buttons using Nintendo positions (B = Cross, A = Circle, ...).
    uint8_t b1 = 0, b2 = 0;
    if (b & kBtnMinus) b1 |= 0x01;
    if (b & kBtnLStick) b1 |= 0x02;
    if (b & kBtnRStick) b1 |= 0x04;
    if (b & kBtnPlus) b1 |= 0x08;
    if (b & kBtnUp) b1 |= 0x10;
    if (b & kBtnRight) b1 |= 0x20;
    if (b & kBtnDown) b1 |= 0x40;
    if (b & kBtnLeft) b1 |= 0x80;
    if (b & kBtnZL) b2 |= 0x01;
    if (b & kBtnZR) b2 |= 0x02;
    if (b & kBtnL) b2 |= 0x04;
    if (b & kBtnR) b2 |= 0x08;
    if (b & kBtnX) b2 |= 0x10;
    if (b & kBtnA) b2 |= 0x20;
    if (b & kBtnB) b2 |= 0x40;
    if (b & kBtnY) b2 |= 0x80;
    w.u8(b1);
    w.u8(b2);
    w.u8((b & kBtnHome) ? 1 : 0);
    w.u8((b & kBtnCapture) ? 1 : 0);
    w.u8(StickByte(s.lx));
    w.u8(StickByte(s.ly));
    w.u8(StickByte(s.rx));
    w.u8(StickByte(s.ry));
    auto analog = [&](uint32_t bit) { w.u8((b & bit) ? 255 : 0); };
    analog(kBtnLeft); analog(kBtnDown); analog(kBtnRight); analog(kBtnUp);
    analog(kBtnY); analog(kBtnB); analog(kBtnA); analog(kBtnX);
    analog(kBtnR); analog(kBtnL); analog(kBtnZR); analog(kBtnZL);
    for (int i = 0; i < 12; i++) w.u8(0);  // two touch points
    w.u64(s.timestampUs);
    w.f32(s.accel[0]); w.f32(s.accel[1]); w.f32(s.accel[2]);
    w.f32(s.gyro[0]); w.f32(s.gyro[1]); w.f32(s.gyro[2]);
    Send(to, kMsgData, w.b);
}

void HandleRequest(const uint8_t* p, int len, const sockaddr_in& from)
{
    if (len < 20 || memcmp(p, "DSUC", 4) != 0) return;
    uint32_t type;
    memcpy(&type, p + 16, 4);
    const uint8_t* body = p + 20;
    int bodyLen = len - 20;

    std::lock_guard<std::mutex> lock(g_lock);
    if (type == kMsgVersion) {
        Writer w;
        w.u16(kProtocolVersion);
        Send(from, kMsgVersion, w.b);
    } else if (type == kMsgPorts && bodyLen >= 4) {
        int32_t count;
        memcpy(&count, body, 4);
        for (int i = 0; i < count && 4 + i < bodyLen && i < kMaxControllers; i++) {
            uint8_t slot = body[4 + i];
            if (slot < kMaxControllers) SendPortInfo(from, slot);
        }
    } else if (type == kMsgData && bodyLen >= 2) {
        Client* c = nullptr;
        for (auto& e : g_clients) {
            if (e.addr.sin_addr.s_addr == from.sin_addr.s_addr && e.addr.sin_port == from.sin_port) c = &e;
        }
        if (!c) {
            g_clients.push_back(Client{from});
            c = &g_clients.back();
        }
        uint8_t flags = body[0], slot = body[1];
        auto now = Clock::now();
        if (flags == 0) c->allSlots = now;
        // MAC-based registration: our MACs encode the slot in the last byte.
        if ((flags & 1) && slot < kMaxControllers) c->slot[slot] = now;
        if ((flags & 2) && bodyLen >= 8 && body[7] >= 1 && body[7] <= kMaxControllers) c->slot[body[7] - 1] = now;
    }
}

void ServerThread()
{
    uint8_t buf[512];
    while (g_running) {
        sockaddr_in from{};
        int fromLen = sizeof(from);
        int n = recvfrom(g_sock, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);
        if (n > 0) HandleRequest(buf, n, from);
    }
}

}  // namespace

namespace Dsu {

bool Start(uint16_t port)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (bind(g_sock, (sockaddr*)&addr, sizeof(addr)) != 0) {
        Log("DSU: port %u is in use (is another DSU server running?) - motion server disabled", port);
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
        return false;
    }
    // A client that goes away triggers WSAECONNRESET on the next recvfrom; ignore it.
    BOOL off = FALSE;
    DWORD ret = 0;
    WSAIoctl(g_sock, _WSAIOW(IOC_VENDOR, 12), &off, sizeof(off), nullptr, 0, &ret, nullptr, nullptr);
    g_running = true;
    g_thread = std::thread(ServerThread);
    Log("DSU motion server listening on 127.0.0.1:%u", port);
    return true;
}

void Stop()
{
    if (!g_running) return;
    g_running = false;
    closesocket(g_sock);
    g_sock = INVALID_SOCKET;
    if (g_thread.joinable()) g_thread.join();
    WSACleanup();
}

void Update(int slot, const MotionState& s)
{
    if (!g_running || slot < 0 || slot >= kMaxControllers) return;
    std::lock_guard<std::mutex> lock(g_lock);
    g_slots[slot] = s;
    g_packet[slot]++;
    auto now = Clock::now();
    for (size_t i = 0; i < g_clients.size();) {
        Client& c = g_clients[i];
        bool wantsAll = now - c.allSlots < kClientTimeout;
        bool wantsSlot = now - c.slot[slot] < kClientTimeout;
        bool alive = wantsAll;
        for (auto& t : c.slot) alive = alive || now - t < kClientTimeout;
        if (!alive) {
            g_clients.erase(g_clients.begin() + i);
            continue;
        }
        if (wantsAll || wantsSlot) SendData(c.addr, slot);
        i++;
    }
}

void Disconnect(int slot)
{
    if (slot < 0 || slot >= kMaxControllers) return;
    std::lock_guard<std::mutex> lock(g_lock);
    g_slots[slot] = MotionState{};
}

}  // namespace Dsu
