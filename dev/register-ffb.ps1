# Registers SSIM's DirectInput pads: the name Windows shows for each controller
# family's VID/PID, and force feedback through Windows' built-in HID PID driver
# (pid.dll). Run as administrator. The installer does the same.

$ErrorActionPreference = 'Stop'
$pads = @(
    @{ Pid = '0001'; Name = 'Switch 2 Pro Controller' },
    @{ Pid = '0002'; Name = 'Switch 1 Pro Controller' },
    @{ Pid = '0003'; Name = 'Joy-Con (L)' },
    @{ Pid = '0004'; Name = 'Joy-Con (R)' },
    @{ Pid = '0005'; Name = 'Joy-Con pair' }
)
foreach ($pad in $pads) {
$oem = "HKLM:\SYSTEM\CurrentControlSet\Control\MediaProperties\PrivateProperties\Joystick\OEM\VID_1209&PID_$($pad.Pid)"
$ff = "$oem\OEMForceFeedback"

New-Item -Force $oem | Out-Null
Set-ItemProperty $oem -Name OEMName -Value $pad.Name
New-Item -Force $ff | Out-Null
Set-ItemProperty $ff -Name CLSID -Value '{EEC6993A-B3FD-11D2-A916-00C04FB98638}'  # Microsoft HID PID driver
# DIFFDEVICEATTRIBUTES: flags 0, sample period 1000 us, min time resolution 1000 us
Set-ItemProperty $ff -Name Attributes -Type Binary -Value ([byte[]](0,0,0,0, 0xE8,3,0,0, 0xE8,3,0,0))

# DIEFFECTATTRIBUTES per effect: PID usage id, DIEFT type, static params, dynamic params, coordinate systems.
# Params 0x1FF = duration, sample period, gain, trigger button/repeat, axes, direction, envelope, type-specific.
$effects = @(
    @{ Guid = '{13541C20-8E33-11D0-9AD0-00A0C9A06E35}'; Name = 'Constant';      Usage = 0x26; Type = 0x0601 },
    @{ Guid = '{13541C21-8E33-11D0-9AD0-00A0C9A06E35}'; Name = 'Ramp';          Usage = 0x27; Type = 0x0602 },
    @{ Guid = '{13541C22-8E33-11D0-9AD0-00A0C9A06E35}'; Name = 'Square Wave';   Usage = 0x30; Type = 0x0603 },
    @{ Guid = '{13541C23-8E33-11D0-9AD0-00A0C9A06E35}'; Name = 'Sine Wave';     Usage = 0x31; Type = 0x0603 },
    @{ Guid = '{13541C24-8E33-11D0-9AD0-00A0C9A06E35}'; Name = 'Triangle Wave'; Usage = 0x32; Type = 0x0603 },
    @{ Guid = '{13541C25-8E33-11D0-9AD0-00A0C9A06E35}'; Name = 'Sawtooth Up';   Usage = 0x33; Type = 0x0603 },
    @{ Guid = '{13541C26-8E33-11D0-9AD0-00A0C9A06E35}'; Name = 'Sawtooth Down'; Usage = 0x34; Type = 0x0603 }
)
foreach ($e in $effects) {
    $key = "$ff\Effects\$($e.Guid)"
    New-Item -Force $key | Out-Null
    Set-ItemProperty $key -Name '(default)' -Value $e.Name
    $attr = [BitConverter]::GetBytes([uint32]$e.Usage) + [BitConverter]::GetBytes([uint32]$e.Type) +
            [BitConverter]::GetBytes([uint32]0x1FF) + [BitConverter]::GetBytes([uint32]0x1FF) +
            [BitConverter]::GetBytes([uint32]0x30)
    Set-ItemProperty $key -Name Attributes -Type Binary -Value ([byte[]]$attr)
}
}
Write-Host 'Names and force feedback registered for the DirectInput pads.'
