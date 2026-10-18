#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <string>

// Physical controller
constexpr USHORT kNintendoVid = 0x057E;
constexpr USHORT kPro2Pid = 0x2069;

// Virtual DirectInput pad presented to games. 0x1209/0x0001 is the pid.codes
// test ID, so it can never collide with a real Nintendo (or other) device.
constexpr USHORT kVirtualVid = 0x1209;
constexpr USHORT kVirtualPid = 0x0001;

constexpr int kMaxControllers = 4;

void LogInit(bool toConsole, bool toFile);
void Log(const char* fmt, ...);

std::wstring ExeDir();
std::wstring ExePath();
