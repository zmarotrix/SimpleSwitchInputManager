# Simple Switch Input Manager (SSIM)

Use Nintendo Switch 2 and Switch 1 controllers on Windows 11. SSIM presents each controller as a DirectInput, XInput (Xbox) or DualSense (PS5) controller, with gyro, HD rumble and player lights.

## Supported controllers

| Controller | Connection | Notes |
|---|---|---|
| Switch 2 Pro Controller | USB (250 Hz) and Bluetooth LE (about 65 Hz) | Pairs to the PC once; after that a button press reconnects it. |
| Switch 2 Joy-Con (L/R) | Bluetooth LE | Paired as one controller, or each on its own held sideways. |
| Switch 1 Pro Controller | Bluetooth | Left alone until you enable it in SSIM. |
| Switch 1 Joy-Con (L/R) | Bluetooth | Same as the Switch 1 Pro Controller (less tested). |
| Nintendo Switch Online N64 controller | Bluetooth | Z and ZR are the triggers; the C-buttons drive the right stick. |

## Features

- **Output mode per controller:** DirectInput (with force feedback), XInput, or DualSense with native gyro. Switch modes live from the tray app.
- **HD rumble** over USB and Bluetooth, in every output mode.
- **Motion:** DualSense mode passes gyro to games directly, and a DSU (Cemuhook) server on port 26760 serves it to emulators.
- **Player lights and slots:** each controller keeps its player number; "Identify" flashes and buzzes one.
- **Joy-Cons:** automatic pairing of a left and a right, or Split / Pair from the tray.
- **Steam stays out of the way:** the raw controller is hidden with HidHide, and a Settings toggle turns off Steam Input for Switch controllers.
- **Options:** swap A/B and X/Y (match by label instead of position), recenter the sticks, battery level with a charging indicator and a low-battery notice.

## Install

1. Download `SSIM-<version>.msi` from [Releases](../../releases) and run it.
2. Windows SmartScreen warns about the installer because it isn't signed. Choose **More info → Run anyway**.
3. If HidHide isn't installed, its own installer runs first and asks for permission.

### How the driver gets signed

SSIM's virtual controllers use the open-source [WinUHid](https://github.com/cgutman/WinUHid) driver. Windows only loads signed drivers, so the installer:

1. creates a code-signing certificate on your PC only (`CN=SSIM Local Driver Signing`);
2. signs the driver with it and trusts the certificate on this PC;
3. deletes the certificate's private key, so nothing else can ever be signed with it.

Uninstalling removes the certificate, the driver, the service and the DirectInput entries. See [installer/README.md](installer/README.md) for the details.

## Use

- **Switch 2 controllers:** plug in with USB, or hold the sync button to pair over Bluetooth.
- **Switch 1 controllers:** pair them in Windows' Bluetooth settings, then select one in SSIM and click **Enable**.
- **Tray app:** the SSIM icon in the notification area opens the controller list and settings.

## Known limitations

- **Installer:** not signed, so SmartScreen warns about it.
- **Not yet tested:** Switch 1 Joy-Cons, and the Switch 1 Pro Controller over USB.
- **DirectInput rumble:** most SDL-based emulators can't rumble a DirectInput pad (an SDL limitation). Use XInput or DualSense mode for rumble there.

## Building from source

You need Visual Studio 2022 with C++ and the Windows Driver Kit, plus the .NET 10 SDK.

```powershell
powershell -ExecutionPolicy Bypass -File installer\build-installer.ps1
```

This builds the driver, engine, tray app and MSI into `installer\out\`.

| Folder | What it is |
|---|---|
| `engine/` | The C++ Windows service that owns the controllers (`engine\build.bat`). |
| `tray/` | The WPF tray app. |
| `installer/` | WiX MSI and the setup custom action that signs the driver. |
| `dev/` | Developer scripts: manual install/uninstall, `ssimctl.py` (talks to the engine's control pipe). |
| `third_party/` | WinUHid and nlohmann/json, built from source. |

## Credits

- **[WinUHid](https://github.com/cgutman/WinUHid)** by Cameron Gutman: virtual HID driver (MIT), at commit `d6cebbef5c7909168d1f881185be8f607d6aefd4`.
- **[HidHide](https://github.com/nefarius/HidHide)** by Nefarius: bundled, unmodified installer (MIT).
- **[nlohmann/json](https://github.com/nlohmann/json)** (MIT).
- **SDL:** Switch 1 rumble amplitude tables (zlib licence, notice in `engine/src/sw1_rumble_tables.h`).
- **Protocol references:**
  - ndeadly's switch2_controller_research;
  - dekuNukem's Nintendo_Switch_Reverse_Engineering;
  - Switch2Connect (GPL-3.0), used for reading only. No code was copied.

Not affiliated with or endorsed by Nintendo.

## Licence

MIT. See [LICENSE](LICENSE).
