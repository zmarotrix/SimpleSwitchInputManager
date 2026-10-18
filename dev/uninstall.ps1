# Removes everything install.ps1 added (HidHide stays; remove it from Settings > Apps if you like).
# Run from an administrator PowerShell:
#   powershell -ExecutionPolicy Bypass -File uninstall.ps1

$ErrorActionPreference = 'Continue'
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host 'Please run this from an administrator PowerShell.' -ForegroundColor Red
    exit 1
}

$dest = Join-Path $env:ProgramFiles 'Switch2Pro'
$exe = Join-Path $dest 'Switch2Pro.exe'
if (Test-Path $exe) {
    & $exe uninstall-service    # stopping it also un-hides the controller
    & $exe uninstall-driver
}

# Remove the WinUHid package from the driver store.
$lines = pnputil /enum-drivers
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match 'Original Name:\s+winuhiddriver\.inf' -and $lines[$i - 1] -match 'Published Name:\s+(oem\d+\.inf)') {
        pnputil /delete-driver $Matches[1] /uninstall /force | Out-Null
    }
}

# Stop trusting the signing certificate.
Get-ChildItem Cert:\LocalMachine\Root, Cert:\LocalMachine\TrustedPublisher |
    Where-Object Subject -eq 'CN=Switch2Pro Local Driver Signing' | Remove-Item

Remove-Item -Recurse -Force 'HKLM:\SYSTEM\CurrentControlSet\Control\MediaProperties\PrivateProperties\Joystick\OEM\VID_1209&PID_0001' -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force $dest -ErrorAction SilentlyContinue
Write-Host 'Switch2Pro removed.' -ForegroundColor Green
