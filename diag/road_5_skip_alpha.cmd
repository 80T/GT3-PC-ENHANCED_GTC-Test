@echo off
rem Super Speedway road test (road_5_skip_alpha): F9 in the game logs one frame's draws.
rem The game's log is copied to diag\road_5_skip_alpha.log when the game closes.
set GT3PC_PGL_TRACE=1
set GT3PC_PGL_SKIP=alpha
start "" /wait "%~dp0..\build\bin\gt3pc.exe"
copy /y "%~dp0..\build\bin\gt3pc.log" "%~dp0road_5_skip_alpha.log" >nul
