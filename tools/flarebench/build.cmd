@echo off
rem Builds flarebench.exe, the GPU-cost measurement of the lens-flare occlusion
rem queries (flarebench.cpp: the game's ps_flare against the batch of
rem pgl_exec.cpp flare_queries_flush, their texels compared), with the x86 MSVC
rem of tools\msvc.cmd and the static CRT, as the game is built.
rem   tools\msvc.cmd tools\flarebench\build.cmd [output folder]     (default: tools\flarebench\build)
rem   tools\flarebench\build\flarebench.exe [queries a frame] [mix]      (default 300; mix: 10 % octagons)
setlocal
set "HERE=%~dp0"
set "OUT=%~1"
if "%OUT%"=="" set "OUT=%HERE%build"
if not exist "%OUT%" mkdir "%OUT%" || exit /b 1
pushd "%OUT%"
cl /nologo /std:c++17 /EHsc /O2 /MT /W3 /DWIN32_LEAN_AND_MEAN /DNOMINMAX "%HERE%flarebench.cpp" /Fe:flarebench.exe /link d3d11.lib dxgi.lib d3dcompiler.lib
set "RC=%ERRORLEVEL%"
popd
exit /b %RC%
