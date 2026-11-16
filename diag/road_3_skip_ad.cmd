@echo off
rem Super Speedway road test (road_3_skip_ad): F9 in the game logs one frame's draws.
rem The game's log is copied to diag\road_3_skip_ad.log when the game closes.
set GT3PC_PGL_TRACE=1
set GT3PC_PGL_SKIP=ad
start "" /wait "%~dp0..\build\bin\gt3pc.exe"
copy /y "%~dp0..\build\bin\gt3pc.log" "%~dp0road_3_skip_ad.log" >nul
