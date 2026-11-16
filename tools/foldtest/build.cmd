@echo off
rem Builds foldtest.exe, the check of what the folded alpha-mirror pass relies
rem on in this PC's Direct3D 11 (foldtest.cpp; pgl_exec.cpp mirror_fold_ok):
rem SV_Position.z in the pixel shader is the depth the depth unit stores for
rem the same fragment, a draw with SV_Depth = the stored depth passes an EQUAL
rem test wherever it covers, and an instanced draw draws all of instance 0
rem before instance 1.  Built with the x86 MSVC of tools\msvc.cmd and the
rem static CRT, as the game is built.
rem   tools\msvc.cmd tools\foldtest\build.cmd [output folder]     (default: tools\foldtest\build)
rem   tools\foldtest\build\foldtest.exe
setlocal
set "HERE=%~dp0"
set "OUT=%~1"
if "%OUT%"=="" set "OUT=%HERE%build"
if not exist "%OUT%" mkdir "%OUT%" || exit /b 1
pushd "%OUT%"
cl /nologo /std:c++17 /EHsc /O2 /MT /W3 /DWIN32_LEAN_AND_MEAN /DNOMINMAX "%HERE%foldtest.cpp" /Fe:foldtest.exe /link d3d11.lib dxgi.lib d3dcompiler.lib
set "RC=%ERRORLEVEL%"
popd
exit /b %RC%
