@echo off
rem Super Speedway road probe: press F9 in the game while the blue / bright
rem patches are on screen.  The four race-screen pixels below (640x448, top
rem left 0,0) are read back after every draw of that frame ("pgl probe"
rem lines).  The game's log is copied to diag\road_probe.log when it closes.
set GT3PC_PGL_TRACE=1
set GT3PC_PGL_PROBE=60,320;200,300;320,240;560,300
start "" /wait "%~dp0..\build\bin\gt3pc.exe"
copy /y "%~dp0..\build\bin\gt3pc.log" "%~dp0road_probe.log" >nul
