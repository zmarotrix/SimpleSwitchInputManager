#include "ipc.h"

#include "config.h"
#include "hub.h"

#include "nlohmann/json.hpp"

#include <sddl.h>

#include <algorithm>
#include <atomic>
#include <list>
#include <memory>
#include <mutex>
#include <thread>

using json = nlohmann::json;

namespace {

constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\SSIM";
constexpr size_t kMaxLine = 64 * 1024;

struct Client {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    std::mutex writeLock;
    std::atomic<bool> subscribed{false};
    std::atomic<bool> done{false};
    std::thread thread;
};

std::atomic<bool> g_running{false};
HANDLE g_stop = nullptr;
std::thread g_acceptThread, g_tickThread;
std::mutex g_clientsLock;
std::list<std::shared_ptr<Client>> g_clients;

json StatusJson()
{
    json controllers = json::array();
    for (auto& s : Hub::List()) {
        controllers.push_back({{"serial", s.serial},
                               {"name", s.name},
                               {"model", s.model},
                               {"transport", s.transport},
                               {"slot", s.slot},
                               {"outputMode", OutputModeKey(s.mode)},
                               {"battery", s.battery},
                               {"rumble", s.rumble},
                               {"bodyColor", s.bodyColor}});
    }
    json detected = json::array();
    for (auto& d : Hub::Detected()) {
        detected.push_back({{"id", d.id}, {"model", d.model}, {"transport", d.transport}, {"managed", d.managed}});
    }
    json known = json::array();
    for (auto& [serial, c] : Config::Get().AllControllers()) {
        known.push_back({{"serial", serial},
                         {"name", c.name},
                         {"outputMode", OutputModeKey(c.mode)},
                         {"slot", c.slot},
                         {"rumble", c.rumble},
                         {"managed", c.managed}});
    }
    GlobalSettings g = Config::Get().Global();
    return {{"type", "status"},
            {"controllers", controllers},
            {"knownControllers", known},
            {"detected", detected},
            {"global",
             {{"defaultOutputMode", OutputModeKey(g.defaultMode)},
              {"bluetooth", g.bluetooth},
              {"pairBluetooth", g.pairBluetooth},
              {"dsu", g.dsu},
              {"dsuPort", g.dsuPort},
              {"rumbleSafetySeconds", g.rumbleSafetySeconds}}}};
}

bool WriteLine(Client& c, const json& j)
{
    std::string line = j.dump() + "\n";
    std::lock_guard<std::mutex> lock(c.writeLock);
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD done = 0;
    bool ok = WriteFile(c.pipe, line.data(), (DWORD)line.size(), nullptr, &ov) || GetLastError() == ERROR_IO_PENDING;
    if (ok && WaitForSingleObject(ov.hEvent, 2000) != WAIT_OBJECT_0) {
        CancelIoEx(c.pipe, &ov);
        ok = false;
    }
    ok = ok && GetOverlappedResult(c.pipe, &ov, &done, TRUE);
    CloseHandle(ov.hEvent);
    return ok;
}

void Broadcast()
{
    json status = StatusJson();
    std::lock_guard<std::mutex> lock(g_clientsLock);
    for (auto& c : g_clients)
        if (c->subscribed && !c->done) WriteLine(*c, status);
}

json HandleSetController(const json& req)
{
    std::string serial = req.at("serial").get<std::string>();
    auto all = Config::Get().AllControllers();
    auto it = std::find_if(all.begin(), all.end(), [&](auto& p) { return p.first == serial; });
    ControllerSettings s;
    if (it != all.end()) {
        s = it->second;
    } else {
        // A detected controller that has never been enabled (Switch 1).
        auto det = Hub::Detected();
        auto d = std::find_if(det.begin(), det.end(), [&](auto& x) { return x.id == serial; });
        if (d == det.end()) return {{"ok", false}, {"error", "unknown controller"}};
        s.name = d->model;
        s.mode = Config::Get().Global().defaultMode;
    }
    if (req.contains("managed")) {
        // Takes effect within two seconds: the engine starts or releases the controller.
        s.managed = req["managed"].get<bool>();
        Config::Get().SetController(serial, s);
    }

    std::optional<OutputMode> mode;
    if (req.contains("outputMode")) {
        mode = ParseOutputMode(req["outputMode"].get<std::string>());
        if (!mode) return {{"ok", false}, {"error", "outputMode must be dinput, xinput or dualsense"}};
    }
    std::optional<float> rumble;
    if (req.contains("rumble")) rumble = std::clamp(req["rumble"].get<float>(), 0.f, 1.f);
    std::optional<std::string> name;
    if (req.contains("name")) name = req["name"].get<std::string>().substr(0, 40);
    std::optional<int> slot;
    if (req.contains("slot")) {
        slot = req["slot"].get<int>();
        if (*slot < 1 || *slot > kMaxSlots) return {{"ok", false}, {"error", "slot must be 1-8"}};
    }

    bool live = Hub::With(serial, [&](ControllerSession& cs) {
        if (mode) cs.SetOutputMode(*mode);
        if (rumble) cs.SetRumbleStrength(*rumble);
        if (name) cs.SetName(*name);
    });
    if (live) {
        if (slot) Hub::MoveToSlot(serial, *slot);
    } else {
        // Not connected: just remember for next time.
        if (mode) s.mode = *mode;
        if (rumble) s.rumble = *rumble;
        if (name) s.name = *name;
        if (slot) s.slot = *slot;
        Config::Get().SetController(serial, s);
        Hub::Changed();
    }
    return {{"ok", true}};
}

json HandleSetGlobal(const json& req)
{
    GlobalSettings g = Config::Get().Global();
    bool restart = false;
    if (req.contains("defaultOutputMode")) {
        auto m = ParseOutputMode(req["defaultOutputMode"].get<std::string>());
        if (!m) return {{"ok", false}, {"error", "bad defaultOutputMode"}};
        g.defaultMode = *m;
    }
    if (req.contains("bluetooth")) { restart |= g.bluetooth != req["bluetooth"].get<bool>(); g.bluetooth = req["bluetooth"]; }
    if (req.contains("pairBluetooth")) g.pairBluetooth = req["pairBluetooth"].get<bool>();
    if (req.contains("dsu")) { restart |= g.dsu != req["dsu"].get<bool>(); g.dsu = req["dsu"]; }
    if (req.contains("dsuPort")) {
        int p = std::clamp(req["dsuPort"].get<int>(), 1024, 65535);
        restart |= g.dsuPort != p;
        g.dsuPort = p;
    }
    if (req.contains("rumbleSafetySeconds")) g.rumbleSafetySeconds = std::clamp(req["rumbleSafetySeconds"].get<int>(), 0, 3600);
    Config::Get().SetGlobal(g);
    Hub::Changed();
    return {{"ok", true}, {"restartRequired", restart}};
}

json Handle(Client& c, const json& req)
{
    std::string cmd = req.at("cmd").get<std::string>();
    if (cmd == "status") {
        json j = StatusJson();
        j["ok"] = true;
        return j;
    }
    if (cmd == "subscribe") {
        c.subscribed = true;
        json j = StatusJson();
        j["ok"] = true;
        return j;
    }
    if (cmd == "setController") return HandleSetController(req);
    if (cmd == "identify") {
        bool live = Hub::With(req.at("serial").get<std::string>(), [](ControllerSession& cs) { cs.Identify(); });
        return live ? json{{"ok", true}} : json{{"ok", false}, {"error", "controller not connected"}};
    }
    if (cmd == "setGlobal") return HandleSetGlobal(req);
    return {{"ok", false}, {"error", "unknown cmd"}};
}

void ClientThread(std::shared_ptr<Client> c)
{
    std::string buffer;
    char chunk[4096];
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE waits[2] = {ov.hEvent, g_stop};
    for (;;) {
        DWORD got = 0;
        ResetEvent(ov.hEvent);
        if (!ReadFile(c->pipe, chunk, sizeof(chunk), nullptr, &ov) && GetLastError() != ERROR_IO_PENDING) break;
        if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0) {
            CancelIoEx(c->pipe, &ov);
            GetOverlappedResult(c->pipe, &ov, &got, TRUE);
            break;
        }
        if (!GetOverlappedResult(c->pipe, &ov, &got, FALSE) || got == 0) break;
        buffer.append(chunk, got);
        if (buffer.size() > kMaxLine) break;  // misbehaving client
        size_t nl;
        while ((nl = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, nl);
            buffer.erase(0, nl + 1);
            if (line.empty() || line == "\r") continue;
            json reply;
            json req;
            try {
                req = json::parse(line);
                reply = Handle(*c, req);
            } catch (const std::exception& e) {
                reply = {{"ok", false}, {"error", std::string("bad request: ") + e.what()}};
            }
            if (req.is_object() && req.contains("id")) reply["id"] = req["id"];
            if (!WriteLine(*c, reply)) goto out;
        }
    }
out:
    CloseHandle(ov.hEvent);
    c->done = true;
    DisconnectNamedPipe(c->pipe);
}

void AcceptThread()
{
    // SYSTEM and administrators: full access; interactive (signed-in) users: read/write.
    SECURITY_ATTRIBUTES sa = {sizeof(sa)};
    ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)", SDDL_REVISION_1,
                                                         &sa.lpSecurityDescriptor, nullptr);
    bool first = true;
    while (g_running) {
        HANDLE pipe = CreateNamedPipeW(kPipeName,
                                       PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
                                           (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                       PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, &sa);
        if (pipe == INVALID_HANDLE_VALUE) {
            Log("IPC: could not create %ls (%lu)", kPipeName, GetLastError());
            break;
        }
        first = false;
        OVERLAPPED ov = {};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        bool connected = ConnectNamedPipe(pipe, &ov) != FALSE;
        DWORD err = GetLastError();
        if (!connected && err == ERROR_IO_PENDING) {
            HANDLE waits[2] = {ov.hEvent, g_stop};
            DWORD dummy;
            connected = WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0 &&
                        GetOverlappedResult(pipe, &ov, &dummy, FALSE);
            if (!connected) CancelIoEx(pipe, &ov);
        } else if (!connected && err == ERROR_PIPE_CONNECTED) {
            connected = true;
        }
        CloseHandle(ov.hEvent);
        if (!connected) {
            CloseHandle(pipe);
            continue;
        }

        auto c = std::make_shared<Client>();
        c->pipe = pipe;
        std::lock_guard<std::mutex> lock(g_clientsLock);
        // Reap finished clients.
        for (auto it = g_clients.begin(); it != g_clients.end();) {
            if ((*it)->done) {
                if ((*it)->thread.joinable()) (*it)->thread.join();
                CloseHandle((*it)->pipe);
                it = g_clients.erase(it);
            } else {
                ++it;
            }
        }
        g_clients.push_back(c);
        c->thread = std::thread(ClientThread, c);
    }
    LocalFree(sa.lpSecurityDescriptor);
}

}  // namespace

namespace Ipc {

bool Start()
{
    g_running = true;
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Hub::OnChange([] {
        if (g_running) Broadcast();
    });
    g_acceptThread = std::thread(AcceptThread);
    // Battery levels change without events; refresh subscribers every few seconds.
    g_tickThread = std::thread([] {
        while (WaitForSingleObject(g_stop, 5000) == WAIT_TIMEOUT) Broadcast();
    });
    Log("Control interface listening on %ls", kPipeName);
    return true;
}

void Stop()
{
    if (!g_running) return;
    g_running = false;
    SetEvent(g_stop);
    if (g_acceptThread.joinable()) g_acceptThread.join();
    if (g_tickThread.joinable()) g_tickThread.join();
    std::lock_guard<std::mutex> lock(g_clientsLock);
    for (auto& c : g_clients) {
        if (c->thread.joinable()) c->thread.join();
        CloseHandle(c->pipe);
    }
    g_clients.clear();
    CloseHandle(g_stop);
}

}  // namespace Ipc
