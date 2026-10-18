# Simple Switch Input Manager (SSIM)

Lets Nintendo Switch 2 and Switch 1 controllers work on Windows 11 as DirectInput, XInput or DualSense controllers, with gyro, HD rumble and player lights. Work in progress.

## Layout

- **`engine/`:** the C++ Windows service that owns the controllers. Build it with `engine\build.bat`.
- **`dev/`:** developer scripts:
  - `sign-driver.ps1` signs the driver with a local certificate;
  - `install.ps1` / `uninstall.ps1` install and remove it;
  - `register-ffb.ps1` registers the DirectInput force-feedback entries.
- **`third_party/WinUHid/`:** the virtual HID driver and its client libraries, built from source.
- **`deps/`** (not in git): `HidHide_x64.exe`, the signed installer from https://github.com/nefarius/HidHide/releases.

## Status

| Feature | State |
|---|---|
| Switch 2 Pro Controller over USB (250 Hz) and Bluetooth LE (about 65 Hz, auto-reconnect) | done |
| Output modes: DirectInput (with force feedback), XInput, DualSense (native gyro) | done; one global mode for now |
| HD rumble on USB and Bluetooth | done |
| DSU / Cemuhook motion server | done |
| Per-controller settings, player lights, tray app, Switch 1, Joy-Cons, installer | planned |

## Third-party code

- **WinUHid:** https://github.com/cgutman/WinUHid at commit `d6cebbef5c7909168d1f881185be8f607d6aefd4`. MIT licence, copyright (c) 2025 Cameron Gutman.
- **Protocol references:**
  - ndeadly's switch2_controller_research;
  - Switch2Connect (GPL-3.0), used for reading only. No code was copied.
- **Force feedback:** the HID PID descriptor in `engine/src/output_dinput.cpp` was written from the USB HID PID 1.0 specification.
