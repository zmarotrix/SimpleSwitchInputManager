#include "usb_controller.h"

#include "commands.h"
#include "decode_sw2.h"
#include "session.h"
#include "devenum.h"
#include "hidhide.h"

#include <initguid.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <hidclass.h>
#include <hidsdi.h>
#include <objbase.h>
#include <winusb.h>

#include <cstring>

// ---------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------

// The vendor command interface (MI_01) is bound to WinUSB through the
// controller's own MS OS descriptors; its interface GUID is in Device Parameters.
static std::wstring WinUsbPathForInterface(DEVINST dn)
{
    HKEY key;
    if (CM_Open_DevNode_Key(dn, KEY_READ, 0, RegDisposition_OpenExisting, &key, CM_REGISTRY_HARDWARE) != CR_SUCCESS) {
        return L"";
    }
    wchar_t buf[512] = {};
    DWORD size = sizeof(buf) - sizeof(wchar_t);
    LSTATUS st = RegQueryValueExW(key, L"DeviceInterfaceGUIDs", nullptr, nullptr, (LPBYTE)buf, &size);
    if (st != ERROR_SUCCESS) {
        size = sizeof(buf) - sizeof(wchar_t);
        st = RegQueryValueExW(key, L"DeviceInterfaceGUID", nullptr, nullptr, (LPBYTE)buf, &size);
    }
    RegCloseKey(key);
    if (st != ERROR_SUCCESS) return L"";

    GUID guid;
    if (FAILED(CLSIDFromString(buf, &guid))) return L"";
    std::wstring id = NodeId(dn);
    auto paths = InterfaceList(guid, id.c_str());
    return paths.empty() ? L"" : paths[0];
}

std::vector<DeviceInfo> FindControllers()
{
    std::vector<DeviceInfo> found;
    for (auto& path : InterfaceList(GUID_DEVINTERFACE_HID, nullptr)) {
        if (!ContainsNoCase(path, L"VID_057E&PID_2069&MI_00")) continue;

        DeviceInfo info;
        info.hidPath = path;
        info.hidInstanceId = InterfaceInstanceId(path);

        // HID child -> USB interface MI_00 -> composite device -> sibling MI_01.
        DEVINST hidNode, mi00, composite, child;
        if (CM_Locate_DevNodeW(&hidNode, (DEVINSTID_W)info.hidInstanceId.c_str(), CM_LOCATE_DEVNODE_NORMAL) !=
                CR_SUCCESS ||
            CM_Get_Parent(&mi00, hidNode, 0) != CR_SUCCESS || CM_Get_Parent(&composite, mi00, 0) != CR_SUCCESS) {
            continue;
        }
        for (CONFIGRET cr = CM_Get_Child(&child, composite, 0); cr == CR_SUCCESS;
             cr = CM_Get_Sibling(&child, child, 0)) {
            if (ContainsNoCase(NodeId(child), L"&MI_01")) {
                info.winusbPath = WinUsbPathForInterface(child);
                break;
            }
        }
        found.push_back(info);
    }
    return found;
}

// ---------------------------------------------------------------------------
// Vendor command channel (WinUSB, interface 1)
// ---------------------------------------------------------------------------

class UsbCommandChannel : public CommandTransport {
public:
    ~UsbCommandChannel() override { Close(); }

    bool Open(const std::wstring& path)
    {
        m_file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
        if (m_file == INVALID_HANDLE_VALUE) return false;
        if (!WinUsb_Initialize(m_file, &m_usb)) return false;

        USB_INTERFACE_DESCRIPTOR desc;
        if (!WinUsb_QueryInterfaceSettings(m_usb, 0, &desc)) return false;
        for (UCHAR i = 0; i < desc.bNumEndpoints; i++) {
            WINUSB_PIPE_INFORMATION pipe;
            if (!WinUsb_QueryPipe(m_usb, 0, i, &pipe) || pipe.PipeType != UsbdPipeTypeBulk) continue;
            if (USB_ENDPOINT_DIRECTION_IN(pipe.PipeId)) m_in = pipe.PipeId;
            else m_out = pipe.PipeId;
        }
        if (!m_in || !m_out) return false;
        ULONG timeout = 500;
        WinUsb_SetPipePolicy(m_usb, m_in, PIPE_TRANSFER_TIMEOUT, sizeof(timeout), &timeout);
        WinUsb_SetPipePolicy(m_usb, m_out, PIPE_TRANSFER_TIMEOUT, sizeof(timeout), &timeout);
        return true;
    }

    void Close()
    {
        if (m_usb) WinUsb_Free(m_usb);
        if (m_file != INVALID_HANDLE_VALUE) CloseHandle(m_file);
        m_usb = nullptr;
        m_file = INVALID_HANDLE_VALUE;
    }

    using CommandTransport::Send;
    // Right after a Bluetooth -> USB handover the controller ignores commands for a
    // moment, so retry a few times before giving up.
    bool Send(uint8_t cmd, uint8_t sub, const uint8_t* data, uint8_t len, std::vector<uint8_t>* reply) override
    {
        for (int attempt = 0; attempt < 5; attempt++) {
            if (SendOnce(cmd, sub, data, len, reply)) return true;
            Sleep(150);
        }
        return false;
    }

private:
    bool SendOnce(uint8_t cmd, uint8_t sub, const uint8_t* data, uint8_t len, std::vector<uint8_t>* reply)
    {
        uint8_t pkt[64] = {cmd, 0x91, 0x00 /* USB */, sub, 0x00, len, 0x00, 0x00};
        memcpy(pkt + 8, data, len);
        WinUsb_FlushPipe(m_usb, m_in);
        ULONG n = 0;
        if (!WinUsb_WritePipe(m_usb, m_out, pkt, 8 + len, &n, nullptr)) return false;
        uint8_t resp[64];
        if (!WinUsb_ReadPipe(m_usb, m_in, resp, sizeof(resp), &n, nullptr)) return false;
        if (n < 8 || resp[0] != cmd || resp[1] != 0x01) return false;
        if (reply) reply->assign(resp + 8, resp + n);
        return true;
    }

    HANDLE m_file = INVALID_HANDLE_VALUE;
    WINUSB_INTERFACE_HANDLE m_usb = nullptr;
    UCHAR m_in = 0, m_out = 0;
};

// ---------------------------------------------------------------------------
// UsbController
// ---------------------------------------------------------------------------

UsbController::UsbController(DeviceInfo info, bool hide)
    : m_info(std::move(info)),
      m_hide(hide),
      m_writeEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
      m_stop(CreateEventW(nullptr, TRUE, FALSE, nullptr))
{
}

UsbController::~UsbController()
{
    Stop();
    CloseHandle(m_stop);
    CloseHandle(m_writeEvent);
}

// HID output report 0x02: left actuator block, then right (hid_reports.md).
bool UsbController::SendRumble(const RumbleCommand& c)
{
    if (m_hid == INVALID_HANDLE_VALUE) return false;
    uint8_t left[16], right[16];
    m_encoder.Encode(c, left, right);
    uint8_t report[64] = {0x02};
    memcpy(report + 1, left, 16);
    memcpy(report + 17, right, 16);
    OVERLAPPED ov = {};
    ov.hEvent = m_writeEvent;
    ResetEvent(m_writeEvent);
    DWORD done = 0;
    if (!WriteFile(m_hid, report, sizeof(report), nullptr, &ov) && GetLastError() != ERROR_IO_PENDING) return false;
    if (WaitForSingleObject(m_writeEvent, 50) != WAIT_OBJECT_0) {
        CancelIoEx(m_hid, &ov);
        GetOverlappedResult(m_hid, &ov, &done, TRUE);
        return false;
    }
    return GetOverlappedResult(m_hid, &ov, &done, FALSE) != FALSE;
}

void UsbController::Start()
{
    m_thread = std::thread([this] {
        Run();
        m_finished = true;
    });
}

void UsbController::Stop()
{
    SetEvent(m_stop);
    if (m_thread.joinable()) m_thread.join();
}

void UsbController::Run()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    Log("Controller connected over USB");

    // 1. Wake the controller and switch it to the full report (buttons, sticks, motion, battery).
    UsbCommandChannel ch;
    if (m_info.winusbPath.empty() || !ch.Open(m_info.winusbPath)) {
        Log("  could not open the command interface (%lu)", GetLastError());
        return;
    }
    if (!ch.Send(0x03, 0x0D, {0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF})) {
        Log("  wake-up command failed");
        return;
    }
    bool ok = ch.Send(0x0C, 0x02, {kFeatureMask, 0, 0, 0}) && ch.Send(0x0C, 0x04, {kFeatureMask, 0, 0, 0}) &&
              ch.Send(0x03, 0x0A, {0x05, 0, 0, 0});
    if (!ok) Log("  feature setup failed; motion may be unavailable");

    // 2. Open the controller's own HID interface for its 250 Hz input stream.
    HANDLE hid = CreateFileW(m_info.hidPath.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (hid == INVALID_HANDLE_VALUE) {
        Log("  could not open the HID interface (%lu)", GetLastError());
        return;
    }
    // A short queue means a late reader always gets fresh input, never a backlog.
    HidD_SetNumInputBuffers(hid, 4);

    if (m_hide && !HidHide::Hide(m_info.hidInstanceId)) {
        Log("  HidHide: could not hide the raw controller; games may see it twice");
    }

    StickCal leftCal = ReadStickCal(ch, kCalLeftUser, kCalLeftFactory, "Left");
    StickCal rightCal = ReadStickCal(ch, kCalRightUser, kCalRightFactory, "Right");
    Sw2Decoder decoder(leftCal, rightCal, true);

    m_hid = hid;
    Sw2DeviceOps ops(ch);
    SwitchSession session(ops, true, "Switch 2 Pro Controller", kVirtualPidSwitch2Pro, this, 8);
    session.Begin(250);

    // 3. Forward every report the moment it arrives.
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE waits[2] = {ov.hEvent, m_stop};
    uint8_t buf[64];
    for (;;) {
        DWORD got = 0;
        ResetEvent(ov.hEvent);
        if (!ReadFile(hid, buf, sizeof(buf), nullptr, &ov) && GetLastError() != ERROR_IO_PENDING) {
            Log("USB controller disconnected (%lu)", GetLastError());
            break;
        }
        if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0) {
            CancelIoEx(hid, &ov);
            GetOverlappedResult(hid, &ov, &got, TRUE);
            break;
        }
        if (!GetOverlappedResult(hid, &ov, &got, FALSE)) {
            Log("USB controller disconnected (%lu)", GetLastError());
            break;
        }
        if (got == 64 && buf[0] == 0x05) session.Process(decoder.Decode(buf + 1));
    }

    CloseHandle(ov.hEvent);
    session.End();  // stops rumble before the handle goes away
    m_hid = INVALID_HANDLE_VALUE;
    if (m_hide) HidHide::Unhide(m_info.hidInstanceId);
    CloseHandle(hid);
}
