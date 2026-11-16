@echo off
rem Crackles in the sound: where they come from.  The game writes every
rem sample it sends to the output into diag\audio_crackle.wav (after the
rem output stage: what the device is given), and with kernel streaming logs
rem the refill timing every 10 s ("audio: timing of N refills ...": how
rem close each refill came to the hardware, how many were tight or late).
rem Play until a few crackles were heard (note roughly when), then close the
rem game.  The log is copied to diag\audio_crackle.log.
rem   A crackle in the .wav: the sound itself (the mixer).
rem   A clean .wav but a crackle heard: the output's timing or the device.
set GT3PC_AUDIO_DUMP=%~dp0audio_crackle.wav
set GT3PC_AUDIO_TIMING=1
start "" /wait "%~dp0..\build\bin\gt3pc.exe"
copy /y "%~dp0..\build\bin\gt3pc.log" "%~dp0audio_crackle.log" >nul
