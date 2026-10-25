@echo off
rem Builds GT3 PC (32-bit, MSVC + Ninja) into build\.  Extra arguments go to
rem "cmake --build", e.g.  build.cmd --target gt3pc
setlocal
set "ROOT=%~dp0"
set "BUILD=%ROOT%build"
if not exist "%BUILD%\build.ninja" (
    call "%ROOT%tools\msvc.cmd" cmake -S "%ROOT%." -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo || exit /b 1
)
call "%ROOT%tools\msvc.cmd" cmake --build "%BUILD%" %*
