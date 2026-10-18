#pragma once

#include "pad_input.h"
#include "session.h"

#include <memory>
#include <string>

// Joy-Cons, both generations. A transport (Bluetooth LE for Switch 2, classic
// Bluetooth for Switch 1) registers each connected Joy-Con as a JoyConUnit and
// feeds it decoded input in the upright orientation. The manager decides how
// Joy-Cons are presented:
//   - paired: an L and an R together are one full controller;
//   - single: one Joy-Con held sideways is a small controller of its own.
// L+R pair up automatically unless the user splits them (or turns automatic
// pairing off), and changes take effect without reconnecting.

enum class JoyConSide { Left, Right };

class JoyConUnit {
public:
    virtual ~JoyConUnit() = default;
    virtual JoyConSide Side() const = 0;
    virtual int Generation() const = 0;           // 1 or 2 (only same-generation Joy-Cons pair)
    virtual const std::string& Id() const = 0;    // serial number / Bluetooth address
    virtual DeviceOps& Ops() = 0;                 // identity + player lights
    virtual bool Rumble(float low, float high) = 0;  // its single actuator
};

namespace JoyCons {

void Add(JoyConUnit* unit);     // after the transport set the Joy-Con up
void Remove(JoyConUnit* unit);  // before the transport goes away
void Input(JoyConUnit* unit, const PadInput& upright);
// Re-reads pairing preferences and regroups (call after a settings change).
void Rearrange();
// The engine is stopping: Joy-Cons leaving one by one should not regroup.
void Shutdown();

}  // namespace JoyCons

// Button bits for SL/SR (report 0x05 layout; not in protocol.h's Pro set).
enum : uint32_t {
    kBtnSRR = 1u << 4,   // right Joy-Con SR
    kBtnSLR = 1u << 5,   // right Joy-Con SL
    kBtnSRL = 1u << 20,  // left Joy-Con SR
    kBtnSLL = 1u << 21,  // left Joy-Con SL
};
