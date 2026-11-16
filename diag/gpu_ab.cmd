@echo off
rem GPU A/B (PC, 2026-11-05): the race frame's GPU time with one kind of work
rem left out, to measure what that kind costs - the GPU sections' timestamps
rem credit a heavy section's pixel backlog to the section after it, so the
rem reliable number is the whole frame's GPU time (the F3 monitor's GPU line,
rem the log's "pgl gpu sections ... total") with and without.
rem
rem   gpu_ab.cmd <name> [VAR=value ...]
rem
rem runs the game with the variables set and the GPU sections on, and copies
rem the log to diag\gpu_ab_<name>.log when it closes.  The pictures are wrong
rem in these runs: they are measurements.  Kinds worth a run, each against a
rem plain run (gpu_ab.cmd plain):
rem   gpu_ab.cmd noenv     GT3PC_PGL_SKIP=envmtx,envmap     (the cars' env passes, with their mirror passes)
rem   gpu_ab.cmd nomirror  GT3PC_PGL_MIRROR_PASS=0          (the alpha mirror's passes alone)
rem   gpu_ab.cmd adonce    GT3PC_PGL_AD_ONCE=1              (the second rasterization of every C = Ad draw)
rem   gpu_ab.cmd noad      GT3PC_PGL_SKIP=ad                (every C = Ad draw: env passes, tyres, glint)
rem   gpu_ab.cmd noself    GT3PC_PGL_SKIP=self              (the draws that sample their own target)
rem and the road reflection detail Original against Full from the settings menu.
if "%~1"=="" (
    echo usage: gpu_ab.cmd ^<name^> [VAR=value ...]
    exit /b 1
)
set AB_NAME=%~1
shift
:vars
if "%~1"=="" goto run
set "%~1"
shift
goto vars
:run
set GT3PC_GPU_SECTIONS=120
set GT3PC_PGL_STATS=120
set GT3PC_PGL_PROFILE=120
start "" /wait "%~dp0..\build\bin\gt3pc.exe"
:wait
tasklist /FI "IMAGENAME eq gt3pc.exe" 2>nul | find /I "gt3pc.exe" >nul && (timeout /t 2 /nobreak >nul & goto wait)
copy /y "%~dp0..\build\bin\gt3pc.log" "%~dp0gpu_ab_%AB_NAME%.log" >nul
echo the log: %~dp0gpu_ab_%AB_NAME%.log
