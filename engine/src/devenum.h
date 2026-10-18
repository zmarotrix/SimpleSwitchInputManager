#pragma once

#include "common.h"

#include <cfgmgr32.h>

#include <vector>

// Small device-tree helpers shared by the USB and Switch 1 discovery code.
std::vector<std::wstring> InterfaceList(const GUID& guid, const wchar_t* instanceId);  // present interfaces only
std::wstring InterfaceInstanceId(const std::wstring& interfacePath);
std::wstring NodeId(DEVINST dn);
bool ContainsNoCase(const std::wstring& haystack, const wchar_t* needle);
