@echo off
rem GPU time by section of the race frame: every 120 frames the log gets
rem "pgl gpu sections (ms a frame, ...)": scene, effects, copy, replay blur,
rem weather (the heat-haze / rain drops), smoke, flare, post, other.  Also
rem every 120th frame "pgl stats ... target copies whole N, partial M".
rem Play a replay (with the heat-haze hexagons) and a race for comparison.
rem The game's log is copied to diag\gpu_sections.log when the game closes.
set GT3PC_GPU_SECTIONS=120
set GT3PC_PGL_STATS=120
rem the executor's CPU profile, the D3D thread's busy time ("pgl cpu profile ...")
set GT3PC_PGL_PROFILE=120
rem the renderer's threads by function, every 10 s ("sample prof ...")
set GT3PC_SAMPLE=10
start "" /wait "%~dp0..\build\bin\gt3pc.exe"
rem (the game may have started itself again - Restart as new game: its last run's log, once none runs)
:wait
tasklist /FI "IMAGENAME eq gt3pc.exe" 2>nul | find /I "gt3pc.exe" >nul && (timeout /t 2 /nobreak >nul & goto wait)
copy /y "%~dp0..\build\bin\gt3pc.log" "%~dp0gpu_sections.log" >nul
copy /y "%~dp0..\build\bin\gt3pc.prev.log" "%~dp0gpu_sections.prev.log" >nul
