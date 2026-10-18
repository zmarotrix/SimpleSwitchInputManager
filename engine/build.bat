@echo off
setlocal
rem Builds the SSIM engine into .\bin (needs Visual Studio 2022 with C++ tools).
cd /d "%~dp0"

set WINUHID=%~dp0..\third_party\WinUHid
set WINUHID_LIB=%WINUHID%\WinUHid\build\Release\x64
set WINUHIDDEVS_LIB=%WINUHID%\WinUHidDevs\build\Release\x64

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -property installationPath`) do set "VS=%%i"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

if not exist bin mkdir bin
if not exist obj mkdir obj

cl /nologo /std:c++20 /O2 /EHsc /W4 /wd4100 /MT /DUNICODE /D_UNICODE ^
   /I "%WINUHID%\WinUHid" /I "%WINUHID%\WinUHidDevs" /I "%~dp0..\third_party" /Fo"obj\\" /Fe"bin\Switch2Pro.exe" ^
   src\main.cpp src\usb_controller.cpp src\ble_controller.cpp src\forwarder.cpp src\rumble.cpp ^
   src\session.cpp src\hub.cpp src\config.cpp src\ipc.cpp src\decode_sw2.cpp src\sw1.cpp src\devenum.cpp ^
   src\output_dinput.cpp src\output_xinput.cpp src\output_dualsense.cpp ^
   src\hidhide.cpp src\dsu.cpp src\log.cpp ^
   /link /LIBPATH:"%WINUHID_LIB%" /LIBPATH:"%WINUHIDDEVS_LIB%" WinUHid.lib WinUHidDevs.lib ^
   setupapi.lib cfgmgr32.lib newdev.lib hid.lib winusb.lib ws2_32.lib ole32.lib advapi32.lib shell32.lib ^
   windowsapp.lib ^
   || exit /b 1

copy /y "%WINUHID_LIB%\WinUHid.dll" bin\ >nul
copy /y "%WINUHIDDEVS_LIB%\WinUHidDevs.dll" bin\ >nul
echo Built bin\Switch2Pro.exe
