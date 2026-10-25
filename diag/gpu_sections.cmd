@echo off
rem GPU time by section of the race frame: every 120 frames the log gets
rem "pgl gpu sections (ms a frame, ...)": scene, effects, copy, replay blur,
rem weather (the heat-haze / rain drops), smoke, flare, post, other.  Also
rem every 120th frame "pgl stats ... target copies whole N, partial M".
rem Play a replay (with the heat-haze hexagons) and a race for comparison.
rem The game's log is copied to diag\gpu_sections.log when the game closes.
set GT3PC_GPU_SECTIONS=120
set GT3PC_PGL_STATS=120
start "" /wait "%~dp0..\build\bin\gt3pc.exe"
copy /y "%~dp0..\build\bin\gt3pc.log" "%~dp0gpu_sections.log" >nul
