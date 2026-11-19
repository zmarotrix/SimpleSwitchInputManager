# SSIM installer

`build-installer.ps1` builds everything from source and produces `out\SSIM-<version>.msi`:

```powershell
powershell -ExecutionPolicy Bypass -File installer\build-installer.ps1
```

You need Visual Studio 2022 with C++ and the Windows Driver Kit, plus the .NET 10 SDK. WiX 6 comes from NuGet. If `deps\HidHide_x64.exe` is missing, the script downloads HidHide v1.5.230 from its GitHub release and checks the file's SHA-256 and its Nefarius signature.

## What the MSI does

Everything goes into `C:\Program Files\SSIM`:

| File | What it is |
|---|---|
| `SSIM.Engine.exe` | The engine. It runs as the `SSIM` Windows service ("Simple Switch Input Manager"), starts automatically and restarts if it crashes. |
| `SSIM.exe` | The tray app (self-contained .NET 10). It also gets a Start menu shortcut. |
| `WinUHid.dll`, `WinUHidDevs.dll` | WinUHid client libraries. |
| `driver\` | The WinUHid driver package as built. It is unsigned. |

During installation:

1. **HidHide:** if HidHide isn't installed, the bundled Nefarius installer runs first. It shows its own Windows permission prompt.
   - It runs from the setup dialog, not from the install script, because Windows can only run one MSI at a time.
   - A silent install (`/qn`) skips this step. Install HidHide first if you deploy SSIM silently.
2. **Driver signing:** `SsimSetup.dll` (in `ca\`) runs as SYSTEM:
   - It creates a self-signed code-signing certificate, `CN=SSIM Local Driver Signing`, on this PC.
   - It signs a temporary copy of `WinUHidDriver.dll` and `winuhiddriver.cat` with `SignerSignEx2` from Windows' `mssign32.dll`.
   - It adds the certificate to `LocalMachine\Root` and `LocalMachine\TrustedPublisher`.
   - It deletes the private key, then installs the driver and creates the `Root\WinUHid` device.
3. **DirectInput names:** it registers the names and force-feedback entries for VID 1209, PIDs 0001 to 0006. These are the same entries as `dev\register-ffb.ps1`.
4. **Service:** it starts the `SSIM` service. The last dialog offers to launch the tray app.

## Uninstalling

Remove it from Settings > Apps. That opens SSIM's own remove dialog, which has an **Also delete my SSIM settings** checkbox. Removal deletes:

- the service;
- the WinUHid device and its driver-store package;
- the SSIM certificate, from both stores;
- the DirectInput registry keys;
- the program folder.

Two things stay unless you remove them yourself:

- **HidHide:** other programs may use it. Remove it from Settings > Apps if you don't need it.
- **Your settings** in `%ProgramData%\SSIM`, unless you ticked the checkbox.

For a silent uninstall, use `msiexec /x SSIM-<version>.msi /qn`. Add `REMOVESETTINGS=1` to delete the settings too. To get a log, add `/l*v ssim.log`. Lines from the custom actions start with `SsimSetup:`.

## Why Windows warns about the installer

The MSI isn't code-signed: SSIM doesn't buy a code-signing certificate. When you open it, Windows SmartScreen says "Windows protected your PC". Click **More info**, then **Run anyway**.

The driver is a separate matter. Windows only loads drivers it trusts, so the installer signs the driver on your PC with a certificate it makes there. Here's how that certificate is limited:

- **One PC only:** it is trusted on your PC only.
- **No key left behind:** its private key is deleted seconds after it is created, so nobody can use it to sign anything else.
- **Gone on uninstall:** uninstalling removes it again.

You don't need test mode or to turn off driver signature enforcement.

## Notes for developers

- **Driver build:** the WDK's built-in INF check fails because it looks for `x86\InfVerif.dll`, which current WDKs no longer include. The script turns it off (`SkipPackageVerification=true`) and runs the standalone `infverif.exe /w` on the built INF instead.
- **Re-installing or upgrading:** the custom action first removes every `winuhiddriver.inf` package in the driver store and any old SSIM certificates, then installs a freshly signed copy. If another product also installed WinUHid, its driver package would be replaced too.
- **Dev setup:** remove it with `dev\uninstall.ps1` before installing the MSI.
