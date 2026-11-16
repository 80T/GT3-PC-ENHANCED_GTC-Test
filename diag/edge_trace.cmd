@echo off
rem The light line along the screen's bottom (2026-10-27): as screen_trace.cmd
rem (F9 on the screen that shows it, then close the game), with probes on the
rem bottom rows - 320,447 and 320,444 of the 640 x 448 race view, 320,223 and
rem 320,220 of the 640 x 224 buffers - so the log says which draws paint them
rem ("pgl probe" lines), and the frame's buffers saved beside its picture.
set GT3PC_PGL_TRACE=1
set "GT3PC_PGL_PROBE=320,447;320,444;320,223;320,220"
set GT3PC_TRACE_DIR=%~dp0edge_trace
if not exist "%GT3PC_TRACE_DIR%" mkdir "%GT3PC_TRACE_DIR%"
start "" /wait "%~dp0..\build\bin\gt3pc.exe"
copy /y "%~dp0..\build\bin\gt3pc.log" "%GT3PC_TRACE_DIR%\gt3pc.log" >nul
