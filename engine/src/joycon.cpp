#include "joycon.h"

#include "config.h"
#include "protocol.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>

namespace {

constexpr uint32_t kMaskL = kBtnDown | kBtnUp | kBtnRight | kBtnLeft | kBtnSRL | kBtnSLL | kBtnL | kBtnZL |
                            kBtnMinus | kBtnLStick | kBtnCapture | kBtnGL;
constexpr uint32_t kMaskR = kBtnY | kBtnX | kBtnB | kBtnA | kBtnSRR | kBtnSLR | kBtnR | kBtnZR | kBtnPlus |
                            kBtnRStick | kBtnHome | kBtnC | kBtnGR;

// PadInput motion uses the DSU convention; SDL's (which the sideways rotations
// come from) is accel_sdl = -accel_dsu, gyro_sdl = (x, -y, -z).
void ToSdl(const PadInput& in, float a[3], float g[3])
{
    for (int i = 0; i < 3; i++) a[i] = -in.accel[i];
    g[0] = in.gyro[0]; g[1] = -in.gyro[1]; g[2] = -in.gyro[2];
}

void FromSdl(PadInput& out, const float a[3], const float g[3])
{
    for (int i = 0; i < 3; i++) out.accel[i] = -a[i];
    out.gyro[0] = g[0]; out.gyro[1] = -g[1]; out.gyro[2] = -g[2];
}

// One Joy-Con held sideways (SL/SR on top) as a small controller of its own.
PadInput Sideways(const PadInput& in, JoyConSide side)
{
    PadInput out = in;
    out.buttons = 0;
    auto map = [&](uint32_t from, uint32_t to) {
        if (in.buttons & from) out.buttons |= to;
    };
    float a[3], g[3], ra[3], rg[3];
    ToSdl(in, a, g);
    if (side == JoyConSide::Left) {
        // Turned a quarter anticlockwise: the d-pad becomes the face buttons.
        map(kBtnUp, kBtnY);
        map(kBtnRight, kBtnX);
        map(kBtnDown, kBtnA);
        map(kBtnLeft, kBtnB);
        map(kBtnSLL, kBtnL);
        map(kBtnSRL, kBtnR);
        map(kBtnL, kBtnGL);
        map(kBtnZL, kBtnGR);
        map(kBtnMinus, kBtnPlus);
        map(kBtnLStick, kBtnLStick);
        map(kBtnCapture, kBtnHome);
        out.lx = -in.ly;
        out.ly = in.lx;
        ra[0] = a[2]; ra[1] = a[1]; ra[2] = -a[0];
        rg[0] = g[2]; rg[1] = g[1]; rg[2] = -g[0];
    } else {
        // Turned a quarter clockwise.
        map(kBtnX, kBtnA);
        map(kBtnA, kBtnB);
        map(kBtnB, kBtnY);
        map(kBtnY, kBtnX);
        map(kBtnSLR, kBtnL);
        map(kBtnSRR, kBtnR);
        map(kBtnR, kBtnGL);
        map(kBtnZR, kBtnGR);
        map(kBtnPlus, kBtnPlus);
        map(kBtnRStick, kBtnLStick);
        map(kBtnHome, kBtnHome);
        map(kBtnC, kBtnC);
        out.lx = in.ry;
        out.ly = -in.rx;
        ra[0] = -a[2]; ra[1] = a[1]; ra[2] = a[0];
        rg[0] = -g[2]; rg[1] = g[1]; rg[2] = g[0];
    }
    out.rx = out.ry = 0;
    out.lt = out.rt = 0;
    FromSdl(out, ra, rg);
    return out;
}

// L + R as one controller: left stick from L, right stick and motion from R.
PadInput Combine(const PadInput& l, const PadInput& r)
{
    PadInput out = r;
    out.buttons = (l.buttons & kMaskL) | (r.buttons & kMaskR);
    out.lx = l.lx;
    out.ly = l.ly;
    out.rx = r.rx;
    out.ry = r.ry;
    out.lt = (out.buttons & kBtnZL) ? 1.f : 0.f;
    out.rt = (out.buttons & kBtnZR) ? 1.f : 0.f;
    out.batteryPercent = std::min(l.batteryPercent, r.batteryPercent);
    return out;
}

// --- DeviceOps / rumble for a group ---------------------------------------

class SingleOps : public DeviceOps {
public:
    explicit SingleOps(JoyConUnit* u) : m_u(u) {}
    bool ReadIdentity(std::string& id, std::string& color) override { return m_u->Ops().ReadIdentity(id, color); }
    void SetPlayerLeds(uint8_t pattern) override { m_u->Ops().SetPlayerLeds(pattern); }

private:
    JoyConUnit* m_u;
};

class PairOps : public DeviceOps {
public:
    PairOps(JoyConUnit* l, JoyConUnit* r) : m_l(l), m_r(r) {}
    bool ReadIdentity(std::string& id, std::string& color) override
    {
        std::string ignored;
        m_l->Ops().ReadIdentity(ignored, color);  // the left Joy-Con's colour
        id = "JCP-" + m_l->Id() + "-" + m_r->Id();
        return true;
    }
    void SetPlayerLeds(uint8_t pattern) override
    {
        m_l->Ops().SetPlayerLeds(pattern);
        m_r->Ops().SetPlayerLeds(pattern);
    }

private:
    JoyConUnit *m_l, *m_r;
};

class GroupRumble : public RumbleSink {
public:
    GroupRumble(JoyConUnit* l, JoyConUnit* r) : m_l(l), m_r(r) {}
    bool SendRumble(const RumbleCommand& c) override
    {
        if (m_l && m_r) {
            bool ok = m_l->Rumble(c.lowL, c.highL);
            return m_r->Rumble(c.lowR, c.highR) && ok;
        }
        JoyConUnit* u = m_l ? m_l : m_r;
        return u->Rumble(std::max(c.lowL, c.lowR), std::max(c.highL, c.highR));
    }

private:
    JoyConUnit *m_l, *m_r;
};

struct Group {
    JoyConUnit* l = nullptr;  // either may be null for a single Joy-Con
    JoyConUnit* r = nullptr;
    std::unique_ptr<DeviceOps> ops;
    std::unique_ptr<GroupRumble> rumble;
    std::unique_ptr<SwitchSession> session;
    PadInput lastL, lastR;
    bool haveL = false, haveR = false;
};

std::mutex g_rearrange;  // one regroup at a time
std::mutex g_route;      // guards g_units / g_groups for Input()
std::vector<JoyConUnit*> g_units;
std::vector<std::unique_ptr<Group>> g_groups;
std::atomic<bool> g_shuttingDown{false};

std::string ModelName(int gen, const char* what)
{
    return std::string("Switch ") + (gen == 1 ? "1" : "2") + " " + what;
}

std::unique_ptr<Group> BuildGroup(JoyConUnit* l, JoyConUnit* r)
{
    auto g = std::make_unique<Group>();
    g->l = l;
    g->r = r;
    JoyConUnit* any = l ? l : r;
    int gen = any->Generation();
    std::string model;
    USHORT pid;
    if (l && r) {
        g->ops = std::make_unique<PairOps>(l, r);
        model = ModelName(gen, "Joy-Con pair");
        pid = kVirtualPidJoyConPair;
    } else {
        g->ops = std::make_unique<SingleOps>(any);
        model = ModelName(gen, l ? "Joy-Con (L)" : "Joy-Con (R)");
        pid = l ? kVirtualPidJoyConL : kVirtualPidJoyConR;
    }
    g->rumble = std::make_unique<GroupRumble>(l, r);
    g->session = std::make_unique<SwitchSession>(*g->ops, false, model, pid, g->rumble.get(), gen == 1 ? 40 : 15);
    std::vector<std::string> members;
    if (l) members.push_back(l->Id());
    if (r) members.push_back(r->Id());
    g->session->SetJoyCon(l && r ? "joyconPair" : "joycon", members);
    return g;
}

}  // namespace

namespace JoyCons {

void Add(JoyConUnit* unit)
{
    {
        std::lock_guard<std::mutex> lock(g_route);
        g_units.push_back(unit);
    }
    Rearrange();
}

void Remove(JoyConUnit* unit)
{
    {
        std::lock_guard<std::mutex> lock(g_route);
        g_units.erase(std::remove(g_units.begin(), g_units.end(), unit), g_units.end());
    }
    Rearrange();
}

void Shutdown()
{
    g_shuttingDown = true;
}

void Rearrange()
{
    std::lock_guard<std::mutex> one(g_rearrange);

    // Work out the wanted arrangement: pair each Left with the first free Right
    // of the same generation, unless either was split off by the user.
    std::vector<JoyConUnit*> units;
    {
        std::lock_guard<std::mutex> lock(g_route);
        units = g_units;
    }
    bool combine = Config::Get().Global().combineJoyCons;
    std::vector<std::pair<JoyConUnit*, JoyConUnit*>> wanted;
    std::vector<JoyConUnit*> taken;
    auto isTaken = [&](JoyConUnit* u) { return std::find(taken.begin(), taken.end(), u) != taken.end(); };
    for (auto* l : units) {
        if (l->Side() != JoyConSide::Left || !combine || Config::Get().IsJoyConSingle(l->Id())) continue;
        for (auto* r : units) {
            if (r->Side() == JoyConSide::Right && r->Generation() == l->Generation() && !isTaken(r) &&
                !Config::Get().IsJoyConSingle(r->Id())) {
                wanted.emplace_back(l, r);
                taken.push_back(l);
                taken.push_back(r);
                break;
            }
        }
    }
    for (auto* u : units)
        if (!isTaken(u)) wanted.emplace_back(u->Side() == JoyConSide::Left ? u : nullptr,
                                             u->Side() == JoyConSide::Right ? u : nullptr);

    // Take groups that no longer match out of the routing table first, so input
    // threads stop using them, then end their sessions outside the lock.
    std::vector<std::unique_ptr<Group>> stale;
    {
        std::lock_guard<std::mutex> lock(g_route);
        for (auto it = g_groups.begin(); it != g_groups.end();) {
            auto match = std::find(wanted.begin(), wanted.end(), std::make_pair((*it)->l, (*it)->r));
            if (match == wanted.end()) {
                stale.push_back(std::move(*it));
                it = g_groups.erase(it);
            } else {
                wanted.erase(match);  // already in place
                ++it;
            }
        }
    }
    for (auto& g : stale) g->session->End();
    stale.clear();

    for (auto& [l, r] : wanted) {
        if (g_shuttingDown) break;  // only tear down while stopping
        auto g = BuildGroup(l, r);
        g->session->Begin(65);
        std::lock_guard<std::mutex> lock(g_route);
        g_groups.push_back(std::move(g));
    }
}

void Input(JoyConUnit* unit, const PadInput& upright)
{
    std::lock_guard<std::mutex> lock(g_route);
    for (auto& g : g_groups) {
        if (g->l != unit && g->r != unit) continue;
        if (g->l && g->r) {
            bool isRight = unit == g->r;
            PadInput& last = isRight ? g->lastR : g->lastL;
            bool changed = !isRight && (!g->haveL || last.buttons != upright.buttons || last.lx != upright.lx ||
                                        last.ly != upright.ly);
            last = upright;
            (isRight ? g->haveR : g->haveL) = true;
            // Motion comes from the right Joy-Con, so send on its reports; also
            // send straight away when the left one's buttons or stick change.
            if (g->haveL && g->haveR && (isRight || changed)) g->session->Process(Combine(g->lastL, g->lastR));
        } else {
            g->session->Process(Sideways(upright, unit->Side()));
        }
        return;
    }
}

}  // namespace JoyCons
