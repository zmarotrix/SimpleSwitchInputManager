#pragma once

#include "common.h"

// Control interface for the tray app: \\.\pipe\SSIM, one JSON object per line.
//
// Requests (an optional "id" is echoed back in the reply):
//   {"cmd":"status"}
//   {"cmd":"subscribe"}                      then {"type":"status",...} on every change and every 5 s
//   {"cmd":"setController","serial":"...","outputMode":"dinput|xinput|dualsense","slot":1-8,
//    "rumble":0-1,"name":"..."}             any subset of fields
//   {"cmd":"identify","serial":"..."}
//   {"cmd":"setGlobal","defaultOutputMode":...,"bluetooth":bool,"pairBluetooth":bool,"dsu":bool,
//    "dsuPort":int,"rumbleSafetySeconds":int}
// Replies: {"ok":true,...} or {"ok":false,"error":"..."}.
namespace Ipc {

bool Start();
void Stop();

}  // namespace Ipc
