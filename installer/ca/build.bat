@echo off
setlocal
rem Builds SsimSetup.dll (the installer's custom actions) into .\bin (needs Visual Studio 2022 with C++ tools).
cd /d "%~dp0"

set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
for /f "usebackq delims=" %%i in (`vswhere.exe -latest -products * -property installationPath`) do set "VS=%%i"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

if not exist bin mkdir bin
if not exist obj mkdir obj

cl /nologo /std:c++20 /O2 /EHsc /W4 /MT /LD /DUNICODE /D_UNICODE /Fo"obj\\" /Fe"bin\SsimSetup.dll" ^
   SsimSetup.cpp /link /DEF:SsimSetup.def || exit /b 1
echo Built bin\SsimSetup.dll
