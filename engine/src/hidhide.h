#pragma once

#include "common.h"

// Minimal HidHide client (github.com/nefarius/HidHide). Hides the physical
// controller's HID interface from every process except this one, so games see
// only the corrected virtual pad.
namespace HidHide {

bool Available();

// Adds this executable to the allow-list and turns cloaking on.
bool Prepare();

// Hides/unhides one device instance (e.g. HID\VID_057E&PID_2069&MI_00\7&...).
bool Hide(const std::wstring& instanceId);
bool Unhide(const std::wstring& instanceId);

}  // namespace HidHide
