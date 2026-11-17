@echo off
rem Builds GT3 PC (32-bit, MSVC + Ninja) into build\.  Extra arguments go to
rem "cmake --build", e.g.  build.cmd --target gt3pc
rem The interface's translations (lang\*.po, docs\languages.md): first
rem brought up to date with the code's strings (tools\lang_sync); then, when
rem some have no translation and a person is at the console, it asks for each
rem one (blank skips it, skipall the rest) or, e, writes them all to
rem build\translate.txt to fill in and reads it back; then the build builds
rem them in.
rem Whatever is left - shown in English meanwhile - is said at the end
rem (build\languages.txt, languages_todo.txt).
setlocal
set "ROOT=%~dp0"
set "BUILD=%ROOT%build"
if not exist "%BUILD%\build.ninja" (
    call "%ROOT%tools\msvc.cmd" cmake -S "%ROOT%." -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo || goto end
)
rem (PC 2026-11-16: the catalogues' sync, the translations asked for, then the build)
call "%ROOT%tools\msvc.cmd" cmake --build "%BUILD%" --target gt3_lang_sync_run
if not errorlevel 1 if exist "%BUILD%\tools\lang_sync.exe" "%BUILD%\tools\lang_sync.exe" --root "%ROOT%." --ask
call "%ROOT%tools\msvc.cmd" cmake --build "%BUILD%" %*
:end
set "RESULT=%ERRORLEVEL%"
rem (what is still open, the translations not taken: said last)
if exist "%BUILD%\tools\lang_sync.exe" "%BUILD%\tools\lang_sync.exe" --root "%ROOT%." --status
rem (PC 2026-11-16: the build's success, with where its programs are - no ( ) block:
rem a folder's name with a parenthesis would end it)
if not "%RESULT%"=="0" goto done
echo.
echo Build succeeded.
if exist "%BUILD%\bin\gt3launcher.exe" echo   The launcher:  %BUILD%\bin\gt3launcher.exe
if exist "%BUILD%\bin\gt3pc.exe" echo   The game:      %BUILD%\bin\gt3pc.exe
:done
rem (started from Explorer - double-clicked: keep the window open; not when
rem another script or a tool runs it: cmd /c build.cmd waits for nobody)
echo %cmdcmdline% | findstr /i /c:"%~nx0" >nul && call "%ROOT%tools\from_explorer.cmd" && pause
exit /b %RESULT%