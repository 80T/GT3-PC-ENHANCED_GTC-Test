@echo off
rem A wrong picture on one screen: open the screen, click into the window,
rem then press F9 (not while paused with F5: no frame is drawn then).  F9
rem logs every draw of the next two frames ("pgl trace N:" lines: target,
rem states, texture, the draw's box in GS pixels and its texel range, the
rem scissor) and saves the first one's picture at full resolution as
rem diag\screen_trace\trace_frame_N.bmp, with every other buffer of the frame
rem as it held at the end (trace_frame_N_<base>_<w>x<h>.bmp: the views and
rem effects the picture was made of).  F9 again on another screen is fine.
rem The game's log is copied to diag\screen_trace\gt3pc.log when the game
rem closes.
set GT3PC_PGL_TRACE=1
set GT3PC_TRACE_DIR=%~dp0screen_trace
if not exist "%GT3PC_TRACE_DIR%" mkdir "%GT3PC_TRACE_DIR%"
start "" /wait "%~dp0..\build\bin\gt3pc.exe"
copy /y "%~dp0..\build\bin\gt3pc.log" "%GT3PC_TRACE_DIR%\gt3pc.log" >nul
