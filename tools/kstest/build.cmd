@echo off
rem Builds kstest.exe, the check of the kernel streaming sound output
rem (src\platform\audio_ks.cpp), with the x86 MSVC of tools\msvc.cmd and the
rem static CRT, as the game is built.
rem   build.cmd [output folder]     (default: tools\kstest\build)
setlocal
set "HERE=%~dp0"
set "OUT=%~1"
if "%OUT%"=="" set "OUT=%HERE%build"
if not exist "%OUT%" mkdir "%OUT%" || exit /b 1
pushd "%OUT%"
call "%HERE%..\msvc.cmd" cl /nologo /std:c++17 /EHsc /O2 /MT /W4 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I"%HERE%..\..\src" "%HERE%kstest.cpp" /Fe:kstest.exe /link setupapi.lib ksuser.lib ole32.lib avrt.lib advapi32.lib
set "RC=%ERRORLEVEL%"
popd
exit /b %RC%
