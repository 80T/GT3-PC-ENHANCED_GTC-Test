@echo off
rem GT3 PC with the SYNC diagnostics on: lines "SYNC: ..." in build\bin\gt3pc.log.
set GT3PC_SYNC_LOG=1
start "" "%~dp0build\bin\gt3pc.exe"
