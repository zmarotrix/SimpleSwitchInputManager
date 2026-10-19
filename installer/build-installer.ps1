# Builds everything and packages it as installer\out\SSIM-<version>.msi.
#   powershell -ExecutionPolicy Bypass -File installer\build-installer.ps1 [-Version 0.1.0]
#
# Needs: Visual Studio 2022 with C++ and the Windows Driver Kit, the .NET 10 SDK. WiX comes from
# NuGet during the build. No administrator rights needed.
#
# Steps: WinUHid libraries and driver (MSBuild) -> engine (engine\build.bat) -> installer custom
# actions (installer\ca\build.bat) -> tray app (dotnet publish) -> MSI (WiX).

param(
    [string]$Version,
    [switch]$SkipNativeBuild   # reuse the already-built WinUHid, engine and custom actions
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$winuhid = Join-Path $root 'third_party\WinUHid'
$stage = Join-Path $PSScriptRoot 'stage'

function Step($text) { Write-Host "==> $text" -ForegroundColor Cyan }
function Check($what) { if ($LASTEXITCODE) { throw "$what failed (exit code $LASTEXITCODE)" } }

if (-not $Version) {
    $Version = ([xml](Get-Content (Join-Path $root 'tray\SSIM.Tray.csproj'))).Project.PropertyGroup.Version | Select-Object -First 1
}

# HidHide (signed by Nefarius) is shipped inside the MSI. deps\ is not in git, so fetch it if missing.
$hidhide = Join-Path $root 'deps\HidHide_x64.exe'
$hidhideUrl = 'https://github.com/nefarius/HidHide/releases/download/v1.5.230.0/HidHide_1.5.230_x64.exe'
$hidhideSha256 = 'F4BBBCB82E6258641B887C74BC81C4C5F66E4AA811808DFC304347687B7605F6'
if (-not (Test-Path $hidhide)) {
    Step "Downloading HidHide from $hidhideUrl"
    New-Item -ItemType Directory -Force (Split-Path $hidhide) | Out-Null
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest $hidhideUrl -OutFile $hidhide -UseBasicParsing
    if ((Get-FileHash $hidhide -Algorithm SHA256).Hash -ne $hidhideSha256) {
        Remove-Item $hidhide
        throw 'The downloaded HidHide installer does not match the expected SHA-256'
    }
}
$sig = Get-AuthenticodeSignature $hidhide
if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notlike 'CN=Nefarius Software Solutions*') {
    throw "deps\HidHide_x64.exe is not validly signed by Nefarius ($($sig.Status))"
}

if (-not $SkipNativeBuild) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
    if (-not $msbuild) { throw 'MSBuild not found; install Visual Studio 2022 with C++' }
    $msbuildArgs = '-nologo', '-v:minimal', '-p:Configuration=Release', '-p:Platform=x64',
                   '-p:SpectreMitigation=false', '-p:SignMode=Off'

    Step 'Building WinUHid libraries'
    & $msbuild (Join-Path $winuhid 'WinUHid\WinUHid.vcxproj') @msbuildArgs; Check 'WinUHid build'
    & $msbuild (Join-Path $winuhid 'WinUHidDevs\WinUHidDevs.vcxproj') @msbuildArgs; Check 'WinUHidDevs build'

    Step 'Building the WinUHid driver'
    # The WDK's in-build INF check looks for x86\InfVerif.dll, which current WDKs no longer ship
    # (x86 tools were dropped), so it always fails. Skip it and run the standalone infverif.exe instead.
    & $msbuild (Join-Path $winuhid 'WinUHid Driver\WinUHid Driver.vcxproj') @msbuildArgs '-p:SkipPackageVerification=true'
    Check 'WinUHid driver build'
    $infverif = Get-ChildItem (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Tools\*\x64\infverif.exe') |
        Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $infverif) { throw 'infverif.exe not found; install the Windows Driver Kit' }
    & $infverif.FullName /w (Join-Path $winuhid 'WinUHid Driver\build\Release\x64\WinUHid Driver\WinUHidDriver.inf')
    Check 'INF verification'

    Step 'Building the engine'
    & cmd /c (Join-Path $root 'engine\build.bat'); Check 'Engine build'

    Step 'Building the installer custom actions'
    & cmd /c (Join-Path $PSScriptRoot 'ca\build.bat'); Check 'Custom action build'
}

Step 'Publishing the tray app'
# Self-contained so users don't need the .NET 10 Desktop Runtime.
dotnet publish (Join-Path $root 'tray\SSIM.Tray.csproj') -nologo -c Release -r win-x64 --self-contained `
    -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true -p:EnableCompressionInSingleFile=true `
    -p:DebugType=none -p:Version=$Version -o (Join-Path $PSScriptRoot 'obj\tray')
Check 'Tray publish'

Step 'Staging files'
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force (Join-Path $stage 'driver'), (Join-Path $stage 'tray') | Out-Null
Copy-Item (Join-Path $root 'engine\bin\Switch2Pro.exe'), (Join-Path $root 'engine\bin\WinUHid.dll'),
          (Join-Path $root 'engine\bin\WinUHidDevs.dll'), (Join-Path $PSScriptRoot 'ca\bin\SsimSetup.dll'),
          $hidhide $stage
Copy-Item (Join-Path $PSScriptRoot 'obj\tray\SSIM.exe') (Join-Path $stage 'tray')
$driverOut = Join-Path $winuhid 'WinUHid Driver\build\Release\x64\WinUHid Driver'
Copy-Item (Join-Path $driverOut 'WinUHidDriver.inf'), (Join-Path $driverOut 'WinUHidDriver.dll'),
          (Join-Path $driverOut 'winuhiddriver.cat') (Join-Path $stage 'driver')

Step "Building the MSI (version $Version)"
dotnet build (Join-Path $PSScriptRoot 'wix\SSIM.wixproj') -nologo -c Release -p:SsimVersion=$Version "-p:StageDir=$stage"
Check 'MSI build'

$msi = Join-Path $PSScriptRoot "out\SSIM-$Version.msi"
Write-Host ''
Write-Host "Built $msi ($([math]::Round((Get-Item $msi).Length / 1MB, 1)) MB)" -ForegroundColor Green
