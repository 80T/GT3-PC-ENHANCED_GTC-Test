@echo off
rem from_explorer.cmd - exit code 0 when the console this runs in was started
rem by Explorer (a .cmd double-clicked: its window closes when the script
rem ends), else 1.  build.cmd and "Build Mod Plugin.cmd" pause then, to keep
rem their last lines - the interface's languages' report - in sight; run by
rem another script or a tool (cmd /c build.cmd) they wait for nobody.
rem (the chain: Explorer - cmd.exe running the script - this PowerShell)
powershell -NoProfile -NonInteractive -Command "$c = (Get-CimInstance Win32_Process -Filter ('ProcessId=' + $PID)).ParentProcessId; $e = (Get-CimInstance Win32_Process -Filter ('ProcessId=' + $c)).ParentProcessId; if ((Get-Process -Id $e -ErrorAction SilentlyContinue).ProcessName -eq 'explorer') { exit 0 } else { exit 1 }"
exit /b %ERRORLEVEL%