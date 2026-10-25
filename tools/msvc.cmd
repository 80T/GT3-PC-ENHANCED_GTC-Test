@echo off
rem Runs a command in the 32-bit (x86) MSVC build environment of the installed
rem Visual Studio / Build Tools, e.g.  tools\msvc.cmd cmake --build build
rem (the newest one with CMake + Ninja too, as setup.cmd checks; else the
rem newest with the x86 tools)
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSROOT="
if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 Microsoft.VisualStudio.Component.VC.CMake.Project -property installationPath`) do set "VSROOT=%%i"
)
if not defined VSROOT if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
)
if not defined VSROOT if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\18\BuildTools" set "VSROOT=%ProgramFiles(x86)%\Microsoft Visual Studio\18\BuildTools"
if not defined VSROOT (
    echo msvc.cmd: no Visual Studio C++ installation found 1>&2
    exit /b 1
)
call "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul 2>nul || (
    echo msvc.cmd: vcvarsall.bat x86 failed 1>&2
    exit /b 1
)
%*
