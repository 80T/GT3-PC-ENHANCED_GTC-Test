@echo off
rem racebench (racebench.cpp): run through tools\msvc.cmd from pc_enhanced, after the build's
rem gt3_render.lib is up to date (ninja -C build gt3_render).
rem   tools\msvc.cmd tools\racebench\build.cmd [LIB] [NAME]
rem LIB: another gt3_render.lib (a copy of an older build, to compare); NAME: the program's name
rem (default racebench.exe).  The program and its objects go to build\tools\racebench.
setlocal
set T=%~dp0..\..
set LIB_R=%~1
if "%LIB_R%"=="" set LIB_R=%T%\build\src\gt3_render.lib
set OUT=%~2
if "%OUT%"=="" set OUT=racebench.exe
set O=%T%\build\tools\racebench
if not exist "%O%" mkdir "%O%"
cl /nologo /TP -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_CRT_NONSTDC_NO_WARNINGS -D_CRT_SECURE_NO_WARNINGS -I"%T%\src" -I"%T%\include" /DWIN32 /D_WINDOWS /EHsc /O2 /Ob1 /DNDEBUG -std:c++17 -MT -Zi /utf-8 /W3 /wd4244 /wd4267 /wd4305 /wd4996 /Fo"%O%\\" /Fd"%O%\racebench.pdb" /FS -c "%~dp0racebench.cpp" "%T%\src\platform\sample_prof.cpp" || exit /b 1
rem (/FORCE:UNRESOLVED: the renderer's references to the game it never calls here)
link /nologo /DEBUG /OUT:"%O%\%OUT%" "%O%\racebench.obj" "%O%\sample_prof.obj" "%LIB_R%" d3d11.lib dxgi.lib d3dcompiler.lib dbghelp.lib dwrite.lib winmm.lib synchronization.lib user32.lib gdi32.lib ole32.lib /FORCE:UNRESOLVED > "%O%\link.txt"
findstr /C:"error LNK2019" /C:"error LNK2001" /C:"fatal" "%O%\link.txt"
echo racebench: %O%\%OUT%
exit /b 0
