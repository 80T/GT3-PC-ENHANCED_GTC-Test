@echo off
rem GT3 PC with the sound and car-icon diagnostics on:
rem   "snd: ..." lines (sound memory uploads, key-ons with their reasons, the
rem   EE voice census) and "pgl readback: ..." lines in build\bin\gt3pc.log,
rem   everything heard recorded to build\bin\race.wav (about 11 MB a minute).
rem The log is also copied to diag\sound_trace.log when the game closes.
set GT3PC_SND_TRACE=2
set GT3PC_AUDIO_DUMP=%~dp0build\bin\race.wav
set GT3PC_READBACK_LOG=1
start "" /wait "%~dp0build\bin\gt3pc.exe"
if not exist "%~dp0diag" mkdir "%~dp0diag"
copy /y "%~dp0build\bin\gt3pc.log" "%~dp0diag\sound_trace.log" >nul
