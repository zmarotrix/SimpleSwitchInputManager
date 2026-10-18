#include "hub.h"

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace {

struct Slot {
    ControllerSession* owner = nullptr;
    std::string serial;
};

std::recursive_mutex g_lock;  // sessions may call back into the hub
std::condition_variable_any g_released;
Slot g_slots[kMaxSlots + 1];  // 1-based
std::vector<std::function<void()>> g_listeners;
std::mutex g_listenLock;

int SlotOf(ControllerSession* s)
{
    for (int i = 1; i <= kMaxSlots; i++)
        if (g_slots[i].owner == s) return i;
    return 0;
}

}  // namespace

namespace Hub {

int AcquireSlot(ControllerSession* s, const std::string& serial, int preferred)
{
    std::unique_lock<std::recursive_mutex> lock(g_lock);
    // Handover: the old transport is usually gone within milliseconds.
    g_released.wait_for(lock, std::chrono::seconds(3), [&] {
        for (int i = 1; i <= kMaxSlots; i++)
            if (g_slots[i].owner && g_slots[i].owner != s && g_slots[i].serial == serial) return false;
        return true;
    });
    int slot = 0;
    if (preferred >= 1 && preferred <= kMaxSlots && !g_slots[preferred].owner) {
        slot = preferred;
    } else {
        for (int i = 1; i <= kMaxSlots && !slot; i++)
            if (!g_slots[i].owner) slot = i;
    }
    if (slot) g_slots[slot] = {s, serial};
    return slot;
}

void Release(ControllerSession* s)
{
    {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        int i = SlotOf(s);
        if (i) g_slots[i] = {};
    }
    g_released.notify_all();
    Changed();
}

bool MoveToSlot(const std::string& serial, int slot)
{
    if (slot < 1 || slot > kMaxSlots) return false;
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    int from = 0;
    for (int i = 1; i <= kMaxSlots; i++)
        if (g_slots[i].owner && g_slots[i].serial == serial) from = i;
    if (!from) return false;
    if (from == slot) return true;
    std::swap(g_slots[from], g_slots[slot]);
    g_slots[slot].owner->ApplySlot(slot);
    if (g_slots[from].owner) g_slots[from].owner->ApplySlot(from);
    Changed();
    return true;
}

std::vector<SessionStatus> List()
{
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    std::vector<SessionStatus> out;
    for (int i = 1; i <= kMaxSlots; i++)
        if (g_slots[i].owner) out.push_back(g_slots[i].owner->Status());
    return out;
}

bool With(const std::string& serial, const std::function<void(ControllerSession&)>& fn)
{
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    for (int i = 1; i <= kMaxSlots; i++) {
        if (g_slots[i].owner && g_slots[i].serial == serial) {
            fn(*g_slots[i].owner);
            return true;
        }
    }
    return false;
}

void Changed()
{
    std::vector<std::function<void()>> copy;
    {
        std::lock_guard<std::mutex> lock(g_listenLock);
        copy = g_listeners;
    }
    for (auto& f : copy) f();
}

void OnChange(std::function<void()> fn)
{
    std::lock_guard<std::mutex> lock(g_listenLock);
    g_listeners.push_back(std::move(fn));
}

}  // namespace Hub
