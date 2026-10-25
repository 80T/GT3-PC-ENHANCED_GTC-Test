@echo off
rem The dynamic frame rate on any display: in a race, 144 frames a second
rem between the game's 59.94 Hz ticks, interpolated (GT3PC_FRAME_RATE; on a
rem display faster than 60 Hz the game does it by itself, at the display's
rem rate).  Presented without vsync, so a 60 Hz display shows a part of the
rem frames, with tearing in exclusive fullscreen.  The game's log is copied
rem to diag\frame_rate_144.log when the game closes ("frame:" lines).
set GT3PC_FRAME_RATE=144
start "" /wait "%~dp0..\build\bin\gt3pc.exe"
copy /y "%~dp0..\build\bin\gt3pc.log" "%~dp0frame_rate_144.log" >nul
