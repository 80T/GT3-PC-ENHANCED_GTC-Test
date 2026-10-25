@echo off
rem A picture wrong for one frame: play as usual; right after a flicker,
rem press F8 in the game (click into the window first).  The game keeps the
rem last 180 pictures (3 s of the menus; F5 right after the flicker stops
rem the keeping, then take your time for F8) and their frames' draws.  F8
rem writes the pictures that differ from both neighbours to diag\flicker
rem (flicker_N_M_a/b/c.bmp: before, the odd one, after; flicker_N.txt: every
rem picture kept) and what their frames drew differently to the log ("pgl
rem flicker" lines).  F8 again after the next flicker is fine.  The game's log
rem is copied to diag\flicker\gt3pc.log when the game closes.
set GT3PC_FLICKER=1
set GT3PC_FLICKER_DIR=%~dp0flicker
start "" /wait "%~dp0..\build\bin\gt3pc.exe"
copy /y "%~dp0..\build\bin\gt3pc.log" "%~dp0flicker\gt3pc.log" >nul
