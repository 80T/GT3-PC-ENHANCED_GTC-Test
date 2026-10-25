@echo off
rem The replay / camera-change afterimage (the cyan on the first change of a
rem race): "pgl afterimage" lines - each capture of the frame into the R / G /
rem B planes, what each plane texture resolved to, and any draw that takes a
rem plane's bytes before the fade reads them.  The game's log is copied to
rem diag\afterimage_log.log when the game closes.
set GT3PC_AFTERIMAGE_LOG=1
start "" /wait "%~dp0..\build\bin\gt3pc.exe"
copy /y "%~dp0..\build\bin\gt3pc.log" "%~dp0afterimage_log.log" >nul
