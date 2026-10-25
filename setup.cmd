@echo off
rem Gets this machine ready to build GT3 PC (tools\setup.ps1): the x86 MSVC
rem tools, CMake + Ninja and a Windows SDK (from Visual Studio 2022 / 2026,
rem added or installed when missing), then configures build\.
rem   setup.cmd          get what is missing, configure build\
rem   setup.cmd -Check   only say what is there and what would be done
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\setup.ps1" %*
set "RESULT=%ERRORLEVEL%"
rem (started from Explorer: keep the window open)
echo %cmdcmdline% | findstr /i /c:"%~nx0" >nul && pause
exit /b %RESULT%
