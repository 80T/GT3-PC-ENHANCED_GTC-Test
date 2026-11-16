@echo off
rem GT3 PC diagnosis on:
set GT3PC_PGL_ALPHA_LOG=1
start "" /wait "%~dp0build\bin\gt3pc.exe"
if not exist "%~dp0diag" mkdir "%~dp0diag"
copy /y "%~dp0build\bin\gt3pc.log" "%~dp0diag\sound_trace.log" >nul
