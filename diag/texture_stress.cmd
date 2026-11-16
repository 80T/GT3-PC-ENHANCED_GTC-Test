@echo off
rem A wrong texture that comes rarely, made common: the renderer's texture
rem caches made tiny and its thread's timing shaken (GT3PC_TEX_STRESS,
rem renderer.md 10), so that whichever of them is at fault shows it often.
rem   texture_stress.cmd                  every stress (all)
rem   texture_stress.cmd fronts           one, or several: fronts,clut,evict,thread
rem Play races and menus as usual; the game runs slower.  When a texture goes
rem wrong: F5 (pause), F9 (trace), F5 (one tick: the traced frame), F6.  The
rem game's log is copied to diag\texture_stress.log when the game closes.
set "KINDS=%~1"
if "%KINDS%"=="" set "KINDS=all"
set GT3PC_TEX_STRESS=%KINDS%
start "" /wait "%~dp0..\build\bin\gt3pc.exe"
copy /y "%~dp0..\build\bin\gt3pc.log" "%~dp0texture_stress.log" >nul
