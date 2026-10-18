# Installs Switch2Pro. Run from an administrator PowerShell:
#   powershell -ExecutionPolicy Bypass -File install.ps1
#
# What it does:
#   1. Trusts the driver-signing certificate made by sign-driver.ps1 (this PC only).
#   2. Installs HidHide (signed by Nefarius) so games don't see the raw controller twice.
#   3. Installs the WinUHid virtual-controller driver.
#   4. Copies Switch2Pro to C:\Program Files\Switch2Pro and starts it as a service.

$ErrorActionPreference = 'Stop'
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host 'Please run this from an administrator PowerShell.' -ForegroundColor Red
    exit 1
}

$root = $PSScriptRoot
$dest = Join-Path $env:ProgramFiles 'Switch2Pro'
$cer = Join-Path $root 'driver\Switch2Pro-signing.cer'

Write-Host '1/4  Trusting the driver-signing certificate'
Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null

Write-Host '2/4  Installing HidHide'
$needReboot = $false
if (Get-Service HidHide -ErrorAction SilentlyContinue) {
    Write-Host '     already installed'
} else {
    $p = Start-Process (Join-Path $root 'deps\HidHide_x64.exe') -ArgumentList '/exenoui', '/qn', 'REBOOT=ReallySuppress' -Wait -PassThru
    if ($p.ExitCode -eq 3010) { $needReboot = $true }
    elseif ($p.ExitCode -ne 0) { throw "HidHide install failed ($($p.ExitCode))" }
}

Write-Host '3/4  Installing the WinUHid driver'
# Stop an older copy first so its files can be replaced.
& (Join-Path $root 'bin\Switch2Pro.exe') uninstall-service | Out-Null
New-Item -ItemType Directory -Force $dest | Out-Null
Copy-Item (Join-Path $root 'bin\Switch2Pro.exe'), (Join-Path $root 'bin\WinUHid.dll') $dest -Force
& (Join-Path $dest 'Switch2Pro.exe') install-driver (Join-Path $root 'driver\WinUHidDriver.inf')
if ($LASTEXITCODE) { throw 'WinUHid driver install failed' }

# Name shown in Game Controllers (joy.cpl) and most games.
$oem = 'HKLM:\SYSTEM\CurrentControlSet\Control\MediaProperties\PrivateProperties\Joystick\OEM\VID_1209&PID_0001'
New-Item -Force $oem | Out-Null
Set-ItemProperty $oem -Name OEMName -Value 'Switch 2 Pro Controller'

Write-Host '4/4  Starting the Switch2Pro service'
& (Join-Path $dest 'Switch2Pro.exe') install-service
if ($LASTEXITCODE) { throw 'service install failed' }

Write-Host ''
Write-Host 'Done. Plug in the controller over USB; it shows up in joy.cpl as "Switch 2 Pro Controller".' -ForegroundColor Green
Write-Host "Log file: $dest\Switch2Pro.log"
if ($needReboot) { Write-Host 'HidHide asks for a restart before it can hide the raw controller.' -ForegroundColor Yellow }
