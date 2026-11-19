# SSIM roadmap

## Next

- **Installer:** tested on the development PC (install, uninstall, same-version upgrade). Still to do: a clean Windows 11 VM test.
- **N64 controller:** Home and Capture reported not working; the engine passes both through, so check what the controller sends.

## Requested features

### Pairing guard: only pair when the user allows it

Holding sync on a controller that is meant for the Switch console must not pair it to the PC by accident.

- **New setting** "Accept new controllers" (pairing mode). Off by default, or on only for a time-limited window opened from the tray ("Pair a new controller…", e.g. 60 s).
- **Unaffected:** controllers already paired to this PC keep reconnecting by button press.
- **When it's off** and a controller in sync mode is seen:
  - leave it alone (don't connect, don't write the PC's address to it);
  - show a tray notification: "A Switch controller is trying to pair. Open SSIM to allow it." Notify once per controller, rate-limited, with a button to open the pairing window.
- **Engine:** gate `pairingMode` sightings in `Ble::StartWatcher` / main loop on the setting. The tray needs an event for "pairing attempt seen".

### Mouse mode for Switch 2 Joy-Cons

Joy-Con 2s have an optical mouse sensor on the rail side.

- **Data:** enable feature bit 4 (mouse) and read report 0x05 offsets 0x10–0x17: X u16, Y u16, plus two unknown u16s (surface quality? lift distance?).
- **Output:** a virtual HID mouse through WinUHid's mouse preset (`WinUHidMouse`), so no SendInput or focus issues.
- **Per-Joy-Con toggle** "Use as mouse":
  - movement from the sensor;
  - buttons mapped to left/right click (e.g. ZL/ZR or L/R), stick to scroll;
  - sensitivity setting.
- **Pairs:** decide whether a mouse-mode Joy-Con leaves its pair or keeps its gamepad buttons with mouse movement on top.
- **Reference:** Switch2Connect's IR/mouse handling (`Example/src/ir_mouse_activation.py`, `raw_input_mouse.py`).

## Known gaps

- Switch 1 Joy-Cons: built, untested on hardware.
- Wired (USB) Switch 1 Pro Controller: needs its USB handshake.
- Switch 1 motion: uses SDL's default scale, not each controller's factory IMU calibration.
- A released Switch 1 controller keeps its last player light until it reconnects.
- Switch 2 battery percent comes from a curve fitted to one controller's full-charge voltage (about 3.72 V); check it against more controllers.
