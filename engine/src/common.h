#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <string>

// Physical controller
constexpr USHORT kNintendoVid = 0x057E;
constexpr USHORT kPro2Pid = 0x2069;
constexpr USHORT kJoyCon2LPid = 0x2067;
constexpr USHORT kJoyCon2RPid = 0x2066;

// Virtual DirectInput pads presented to games use pid.codes' open vendor ID, so
// they can never collide with a real Nintendo (or other) device. Each controller
// family has its own product ID, because Windows names a DirectInput pad by
// VID/PID (OEMName in the registry; set by the installer).
constexpr USHORT kVirtualVid = 0x1209;
constexpr USHORT kVirtualPidSwitch2Pro = 0x0001;  // "Switch 2 Pro Controller"
constexpr USHORT kVirtualPidSwitch1Pro = 0x0002;  // "Switch 1 Pro Controller"
constexpr USHORT kVirtualPidJoyConL = 0x0003;     // "Joy-Con (L)"
constexpr USHORT kVirtualPidJoyConR = 0x0004;     // "Joy-Con (R)"
constexpr USHORT kVirtualPidJoyConPair = 0x0005;  // "Joy-Con pair"
constexpr USHORT kVirtualPidN64 = 0x0006;         // "N64 Controller"
constexpr uint16_t kN64Pid = 0x2019;              // Nintendo Switch Online N64 controller (Bluetooth)

constexpr int kMaxControllers = 4;

void LogInit(bool toConsole, bool toFile);
void Log(const char* fmt, ...);

std::wstring ExeDir();
std::wstring ExePath();
