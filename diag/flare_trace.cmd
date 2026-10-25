@echo off
rem Light glows showing through objects: press F9 in the game (click into the
rem window first) while the glow is on screen.  Two frames are traced: the
rem first logs every flare query with the depth it saw ("light Z ...; ... px
rem in the shape, ... passed; scene Z ..."), the second reads back the
rem centres of the queries that passed after every draw ("pgl probe" lines).
rem The game's log is copied to diag\flare_trace.log when the game closes.
set GT3PC_PGL_TRACE=1
start "" /wait "%~dp0..\build\bin\gt3pc.exe"
copy /y "%~dp0..\build\bin\gt3pc.log" "%~dp0flare_trace.log" >nul
