@echo off
rem GPU A/B run 5 of 5: the road reflection detail at Original (the PS2's
rem small reflection buffer) instead of Full, everything else as run 1.  The
rem setting is changed in gt3pc.ini for this run only and put back when the
rem game closes (do not change it in the menu meanwhile).  The frame's GPU
rem time against run 1 is what the Full reflection costs.  The log goes to
rem diag\gpu_ab_reflorig.log.
set "INI=%~dp0..\build\bin\gt3pc.ini"
set "OLD="
for /f "usebackq delims=" %%v in (`powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0ini_set.ps1" "%INI%" Graphics ReflectionDetail original`) do set "OLD=%%v"
echo road reflection detail: original for this run (was "%OLD%")
call "%~dp0gpu_ab.cmd" reflorig
if "%OLD%"=="" set "OLD=full"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0ini_set.ps1" "%INI%" Graphics ReflectionDetail "%OLD%" >nul
echo road reflection detail put back to "%OLD%"
