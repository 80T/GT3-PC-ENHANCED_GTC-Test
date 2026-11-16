@echo off
rem A measurement (2026-11-03): gpu_sections.cmd with the DATE copies left out
rem (GT3PC_PGL_DATE_COPY=0).  DATE draws (the cars' environment-map passes)
rem read their target's old copy, so the car bodies' reflections are wrong:
rem only the frame rate counts.  Run the same race as with gpu_sections.cmd;
rem the frames' times, the GPU sections and the executor's profile against
rem that run show what the copies cost the GPU (renderer.md 10).  The game's
rem log is copied to diag\date_nocopy.log when the game closes.
set GT3PC_PGL_DATE_COPY=0
set GT3PC_GPU_SECTIONS=120
set GT3PC_PGL_STATS=120
set GT3PC_PGL_PROFILE=120
set GT3PC_SAMPLE=10
start "" /wait "%~dp0..\build\bin\gt3pc.exe"
rem (the game may have started itself again: its last run's log, once none runs)
:wait
tasklist /FI "IMAGENAME eq gt3pc.exe" 2>nul | find /I "gt3pc.exe" >nul && (timeout /t 2 /nobreak >nul & goto wait)
copy /y "%~dp0..\build\bin\gt3pc.log" "%~dp0date_nocopy.log" >nul
