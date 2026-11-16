@echo off
rem Super Speedway road test (road_1_plain): F9 in the game logs one frame's draws.
rem The game's log is copied to diag\road_1_plain.log when the game closes.
set GT3PC_PGL_TRACE=1
start "" /wait "%~dp0..\build\bin\gt3pc.exe"
copy /y "%~dp0..\build\bin\gt3pc.log" "%~dp0road_1_plain.log" >nul
