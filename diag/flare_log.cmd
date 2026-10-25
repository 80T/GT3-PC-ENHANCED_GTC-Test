@echo off
rem Lens flare / light glow results: every frame the first 48 occlusion
rem results and the value each glow was drawn with ("pgl flare: frame ..."
rem lines, 600 at most).  The game's log is copied to diag\flare_log.log
rem when the game closes.
set GT3PC_FLARE_LOG=1
start "" /wait "%~dp0..\build\bin\gt3pc.exe"
copy /y "%~dp0..\build\bin\gt3pc.log" "%~dp0flare_log.log" >nul
