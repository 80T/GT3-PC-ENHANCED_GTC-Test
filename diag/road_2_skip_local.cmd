@echo off
rem Super Speedway road test (road_2_skip_local): F9 in the game logs one frame's draws.
rem The game's log is copied to diag\road_2_skip_local.log when the game closes.
set GT3PC_PGL_TRACE=1
set GT3PC_PGL_SKIP=local
start "" /wait "%~dp0..\build\bin\gt3pc.exe"
copy /y "%~dp0..\build\bin\gt3pc.log" "%~dp0road_2_skip_local.log" >nul
