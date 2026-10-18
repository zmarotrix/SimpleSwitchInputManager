#include "hidhide.h"

#include <winioctl.h>

#include <algorithm>
#include <vector>

namespace {

// Same IOCTL contract as HidHideCLI.
constexpr DWORD kDeviceType = 32769;
constexpr DWORD Ioctl(DWORD function)
{
    return CTL_CODE(kDeviceType, function, METHOD_BUFFERED, FILE_READ_DATA);
}
const DWORD IOCTL_GET_WHITELIST = Ioctl(2048);
const DWORD IOCTL_SET_WHITELIST = Ioctl(2049);
const DWORD IOCTL_GET_BLACKLIST = Ioctl(2050);
const DWORD IOCTL_SET_BLACKLIST = Ioctl(2051);
const DWORD IOCTL_SET_ACTIVE = Ioctl(2053);

struct Handle {
    HANDLE h;
    Handle()
    {
        h = CreateFileW(L"\\\\.\\HidHide", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                        OPEN_EXISTING, 0, nullptr);
    }
    ~Handle()
    {
        if (ok()) CloseHandle(h);
    }
    bool ok() const { return h != INVALID_HANDLE_VALUE; }
};

std::vector<std::wstring> GetList(HANDLE h, DWORD code)
{
    std::vector<std::wstring> out;
    DWORD needed = 0;
    DeviceIoControl(h, code, nullptr, 0, nullptr, 0, &needed, nullptr);
    if (needed == 0) {
        return out;
    }
    std::vector<wchar_t> buf(needed / sizeof(wchar_t) + 1, 0);
    DWORD got = 0;
    if (!DeviceIoControl(h, code, nullptr, 0, buf.data(), needed, &got, nullptr)) {
        return out;
    }
    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) {
        out.emplace_back(p);
    }
    return out;
}

bool SetList(HANDLE h, DWORD code, const std::vector<std::wstring>& list)
{
    std::vector<wchar_t> buf;
    for (auto& s : list) {
        buf.insert(buf.end(), s.begin(), s.end());
        buf.push_back(0);
    }
    buf.push_back(0);
    DWORD got = 0;
    return DeviceIoControl(h, code, buf.data(), (DWORD)(buf.size() * sizeof(wchar_t)), nullptr, 0, &got, nullptr) !=
           FALSE;
}

bool SameText(const std::wstring& a, const std::wstring& b)
{
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

// C:\x\y.exe -> \Device\HarddiskVolumeN\x\y.exe (the form HidHide stores).
std::wstring DosDevicePath(const std::wstring& path)
{
    if (path.size() < 2 || path[1] != L':') {
        return L"";
    }
    wchar_t target[MAX_PATH];
    if (!QueryDosDeviceW(path.substr(0, 2).c_str(), target, MAX_PATH)) {
        return L"";
    }
    return target + path.substr(2);
}

}  // namespace

namespace HidHide {

bool Available()
{
    return Handle().ok();
}

bool Prepare()
{
    Handle h;
    if (!h.ok()) {
        return false;
    }
    std::wstring self = DosDevicePath(ExePath());
    auto allow = GetList(h.h, IOCTL_GET_WHITELIST);
    bool present = std::any_of(allow.begin(), allow.end(), [&](auto& s) { return SameText(s, self); });
    if (!present) {
        allow.push_back(self);
        if (!SetList(h.h, IOCTL_SET_WHITELIST, allow)) {
            Log("HidHide: failed to allow-list this program (%lu)", GetLastError());
            return false;
        }
    }
    BOOLEAN active = TRUE;
    DWORD got = 0;
    if (!DeviceIoControl(h.h, IOCTL_SET_ACTIVE, &active, sizeof(active), nullptr, 0, &got, nullptr)) {
        Log("HidHide: failed to enable cloaking (%lu)", GetLastError());
        return false;
    }
    return true;
}

bool Hide(const std::wstring& instanceId)
{
    Handle h;
    if (!h.ok()) {
        return false;
    }
    auto list = GetList(h.h, IOCTL_GET_BLACKLIST);
    if (std::any_of(list.begin(), list.end(), [&](auto& s) { return SameText(s, instanceId); })) {
        return true;
    }
    list.push_back(instanceId);
    return SetList(h.h, IOCTL_SET_BLACKLIST, list);
}

bool Unhide(const std::wstring& instanceId)
{
    Handle h;
    if (!h.ok()) {
        return false;
    }
    auto list = GetList(h.h, IOCTL_GET_BLACKLIST);
    auto end = std::remove_if(list.begin(), list.end(), [&](auto& s) { return SameText(s, instanceId); });
    if (end == list.end()) {
        return true;
    }
    list.erase(end, list.end());
    return SetList(h.h, IOCTL_SET_BLACKLIST, list);
}

}  // namespace HidHide
