// Switch2Pro: Switch 2 Pro Controller -> DirectInput for Windows.
//
//   Switch2Pro.exe run                     run in this console (Ctrl+C to stop)
//   Switch2Pro.exe install-driver <inf>    create the WinUHid device and install its driver (admin)
//   Switch2Pro.exe uninstall-driver        remove the WinUHid device (admin)
//   Switch2Pro.exe install-service         start automatically with Windows (admin)
//   Switch2Pro.exe uninstall-service       (admin)
//   Switch2Pro.exe service                 entry point used by the Windows service

#include "common.h"

#include "ble_controller.h"
#include "usb_controller.h"
#include "dsu.h"
#include "hidhide.h"
#include "output.h"

#include <initguid.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <newdev.h>
#include <setupapi.h>

#include <map>
#include <memory>
#include <mutex>

static const wchar_t kServiceName[] = L"Switch2ProPad";
static HANDLE g_stopEvent;
static SERVICE_STATUS_HANDLE g_statusHandle;
static SERVICE_STATUS g_status;

// ---------------------------------------------------------------------------
// Main loop: watch for controllers, one worker each.
// ---------------------------------------------------------------------------

static DWORD CALLBACK OnHidArrival(HCMNOTIFICATION, PVOID ctx, CM_NOTIFY_ACTION action, PCM_NOTIFY_EVENT_DATA,
                                   DWORD)
{
    if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL) SetEvent((HANDLE)ctx);
    return ERROR_SUCCESS;
}

// Settings in HKLM\SOFTWARE\Switch2Pro (DWORD). Per-controller settings replace these in phase 2.
static DWORD SettingValue(const wchar_t* name, DWORD fallback)
{
    DWORD v = fallback, size = sizeof(v);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Switch2Pro", name, RRF_RT_REG_DWORD, nullptr, &v, &size);
    return v;
}

static bool Setting(const wchar_t* name)
{
    return SettingValue(name, 1) != 0;
}

static int RunLoop()
{
    if (!WinUHidAvailable()) {
        Log("The WinUHid driver is not installed or not running (error %lu). Run install.ps1 first.", GetLastError());
        return 1;
    }
    bool hide = HidHide::Available() && HidHide::Prepare();
    Log(hide ? "HidHide active: the raw controller will be hidden from games"
             : "HidHide not installed: games will also see the raw controller");
    Dsu::Start();

    HANDLE arrival = CreateEventW(nullptr, FALSE, TRUE, nullptr);
    CM_NOTIFY_FILTER filter = {};
    filter.cbSize = sizeof(filter);
    filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
    filter.u.DeviceInterface.ClassGuid = {0x4d1e55b2, 0xf16f, 0x11cf, {0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30}};
    HCMNOTIFICATION notify = nullptr;
    CM_Register_Notification(&filter, arrival, OnHidArrival, &notify);

    // Bluetooth sightings arrive on a WinRT thread; the loop below acts on them.
    std::mutex sightLock;
    std::map<uint64_t, bool> sightings;  // address -> sync mode
    bool bluetooth = Setting(L"EnableBluetooth");
    OutputMode mode = (OutputMode)SettingValue(L"OutputMode", 0);
    Log("Output mode: %s", OutputModeName(mode));
    bool allowPairing = Setting(L"PairBluetooth");
    if (bluetooth) {
        Ble::StartWatcher([&](uint64_t address, bool pairingMode) {
            std::lock_guard<std::mutex> lock(sightLock);
            sightings[address] = pairingMode;
            SetEvent(arrival);
        });
    }

    std::unique_ptr<PadSource> slots[kMaxControllers];
    std::map<std::wstring, ULONGLONG> lastAttempt;  // don't hammer a controller that keeps failing
    HANDLE waits[2] = {g_stopEvent, arrival};
    Log("Waiting for Switch 2 Pro Controllers...");

    auto active = [&](const std::wstring& key) {
        for (auto& s : slots)
            if (s && _wcsicmp(s->Key().c_str(), key.c_str()) == 0) return true;
        return false;
    };
    auto place = [&](auto make) {
        for (int i = 0; i < kMaxControllers; i++) {
            if (!slots[i]) {
                slots[i] = make(i);
                slots[i]->Start();
                return;
            }
        }
    };

    for (;;) {
        // Re-check every 2 s as well, in case a notification is missed.
        DWORD w = WaitForMultipleObjects(2, waits, FALSE, 2000);
        if (w == WAIT_OBJECT_0) break;

        for (auto& s : slots) {
            if (s && s->Finished()) s.reset();
        }
        for (auto& dev : FindControllers()) {
            if (!active(dev.hidPath)) place([&](int i) { return std::make_unique<UsbController>(i, dev, hide, mode); });
        }

        std::map<uint64_t, bool> seen;
        {
            std::lock_guard<std::mutex> lock(sightLock);
            seen.swap(sightings);
        }
        ULONGLONG now = GetTickCount64();
        for (auto& [address, pairingMode] : seen) {
            std::wstring key = BleController::KeyFor(address);
            if (active(key) || now - lastAttempt[key] < 3000) continue;
            lastAttempt[key] = now;
            bool pm = pairingMode;
            uint64_t addr = address;
            place([&](int i) { return std::make_unique<BleController>(i, addr, pm, allowPairing, mode); });
        }
        if (bluetooth) Ble::KeepWatcherAlive();
    }

    Log("Stopping");
    Ble::StopWatcher();
    if (notify) CM_Unregister_Notification(notify);
    for (auto& s : slots) s.reset();
    Dsu::Stop();
    CloseHandle(arrival);
    return 0;
}

// ---------------------------------------------------------------------------
// Windows service plumbing
// ---------------------------------------------------------------------------

static void SetState(DWORD state)
{
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = state;
    g_status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    SetServiceStatus(g_statusHandle, &g_status);
}

static DWORD WINAPI ServiceCtrl(DWORD control, DWORD, LPVOID, LPVOID)
{
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        SetState(SERVICE_STOP_PENDING);
        SetEvent(g_stopEvent);
    }
    return NO_ERROR;
}

static void WINAPI ServiceMain(DWORD, LPWSTR*)
{
    g_statusHandle = RegisterServiceCtrlHandlerExW(kServiceName, ServiceCtrl, nullptr);
    SetState(SERVICE_RUNNING);
    RunLoop();
    SetState(SERVICE_STOPPED);
}

static int InstallService()
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!scm) return Log("Open service manager failed (%lu) - run as administrator", GetLastError()), 1;
    std::wstring cmd = L"\"" + ExePath() + L"\" service";
    SC_HANDLE svc = CreateServiceW(scm, kServiceName, L"Switch 2 Pro Controller (DirectInput)", SERVICE_ALL_ACCESS,
                                   SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, cmd.c_str(),
                                   nullptr, nullptr, nullptr, nullptr, nullptr);
    if (!svc && GetLastError() == ERROR_SERVICE_EXISTS) svc = OpenServiceW(scm, kServiceName, SERVICE_ALL_ACCESS);
    if (!svc) return Log("Create service failed (%lu)", GetLastError()), CloseServiceHandle(scm), 1;

    SERVICE_DESCRIPTIONW desc = {(LPWSTR)L"Presents Switch 2 Pro Controllers as DirectInput game controllers."};
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc);
    // Restart automatically if it ever crashes.
    SC_ACTION actions[3] = {{SC_ACTION_RESTART, 2000}, {SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 10000}};
    SERVICE_FAILURE_ACTIONSW fa = {86400, nullptr, nullptr, 3, actions};
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);
    StartServiceW(svc, 0, nullptr);
    Log("Service installed and started");
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return 0;
}

static int UninstallService()
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    SC_HANDLE svc = scm ? OpenServiceW(scm, kServiceName, SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS) : nullptr;
    if (!svc) return Log("Service not installed"), scm && CloseServiceHandle(scm), 0;
    SERVICE_STATUS st;
    ControlService(svc, SERVICE_CONTROL_STOP, &st);
    for (int i = 0; i < 50 && QueryServiceStatus(svc, &st) && st.dwCurrentState != SERVICE_STOPPED; i++) Sleep(100);
    DeleteService(svc);
    Log("Service removed");
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return 0;
}

// ---------------------------------------------------------------------------
// WinUHid root device install (same steps as WinUHid's own installer)
// ---------------------------------------------------------------------------

static const wchar_t kWinUHidHwIds[] = L"Root\\WinUHid\0";

static int ForEachWinUHidDevice(bool remove)
{
    int count = 0;
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    SP_DEVINFO_DATA d = {sizeof(d)};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &d); i++) {
        wchar_t ids[1024] = {};
        DEVPROPTYPE type;
        if (!SetupDiGetDevicePropertyW(set, &d, &DEVPKEY_Device_HardwareIds, &type, (PBYTE)ids, sizeof(ids), nullptr,
                                       0) ||
            memcmp(ids, kWinUHidHwIds, sizeof(kWinUHidHwIds)) != 0) {
            continue;
        }
        count++;
        if (remove) DiUninstallDevice(nullptr, set, &d, 0, nullptr);
    }
    SetupDiDestroyDeviceInfoList(set);
    return count;
}

static int InstallDriver(const wchar_t* inf)
{
    wchar_t full[MAX_PATH];
    GetFullPathNameW(inf, MAX_PATH, full, nullptr);
    if (ForEachWinUHidDevice(false) == 0) {
        GUID cls;
        wchar_t clsName[MAX_CLASS_NAME_LEN];
        if (!SetupDiGetINFClassW(full, &cls, clsName, MAX_CLASS_NAME_LEN, nullptr))
            return Log("Bad INF %ls (%lu)", full, GetLastError()), 1;
        HDEVINFO set = SetupDiCreateDeviceInfoList(&cls, nullptr);
        SP_DEVINFO_DATA d = {sizeof(d)};
        if (!SetupDiCreateDeviceInfoW(set, L"WinUHid", &cls, nullptr, nullptr, DICD_GENERATE_ID, &d) ||
            !SetupDiSetDeviceRegistryPropertyW(set, &d, SPDRP_HARDWAREID, (const BYTE*)kWinUHidHwIds,
                                               sizeof(kWinUHidHwIds)) ||
            !SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, &d)) {
            Log("Creating the WinUHid device failed (%lu)", GetLastError());
            SetupDiDestroyDeviceInfoList(set);
            return 1;
        }
        SetupDiDestroyDeviceInfoList(set);
        Log("Created the WinUHid root device");
    }
    BOOL reboot = FALSE;
    if (!DiInstallDriverW(nullptr, full, DIIRFLAG_FORCE_INF, &reboot)) {
        DWORD e = GetLastError();
        Log("Driver install failed (0x%08lX)%s", e,
            e == ERROR_AUTHENTICODE_PUBLISHER_NOT_TRUSTED || e == TRUST_E_NOSIGNATURE || (e & 0xFFFF) == 0x0241
                ? " - is the signing certificate trusted?"
                : "");
        return 1;
    }
    Log("WinUHid driver installed%s", reboot ? " (reboot required)" : "");
    return 0;
}

static int UninstallDriver()
{
    int n = ForEachWinUHidDevice(true);
    Log("Removed %d WinUHid device(s)", n);
    return 0;
}

// ---------------------------------------------------------------------------

static BOOL WINAPI OnCtrlC(DWORD)
{
    SetEvent(g_stopEvent);
    return TRUE;
}

int wmain(int argc, wchar_t** argv)
{
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::wstring cmd = argc > 1 ? argv[1] : L"run";

    if (cmd == L"service") {
        LogInit(false, true);
        SERVICE_TABLE_ENTRYW table[] = {{(LPWSTR)kServiceName, ServiceMain}, {nullptr, nullptr}};
        return StartServiceCtrlDispatcherW(table) ? 0 : 1;
    }

    // Only the long-running modes write the log file; setup commands just print.
    LogInit(true, cmd == L"run");
    if (cmd == L"run") {
        SetConsoleCtrlHandler(OnCtrlC, TRUE);
        return RunLoop();
    }
    if (cmd == L"install-driver" && argc > 2) return InstallDriver(argv[2]);
    if (cmd == L"uninstall-driver") return UninstallDriver();
    if (cmd == L"install-service") return InstallService();
    if (cmd == L"uninstall-service") return UninstallService();

    Log("Usage: Switch2Pro.exe [run | install-driver <inf> | uninstall-driver | install-service | uninstall-service]");
    return 1;
}
