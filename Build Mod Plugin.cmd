@echo off
rem Builds GT3 PC as build.cmd does, then packages the build as an engine plugin:
rem ONE folder holding what swaps the game's engine - this tree's launcher and
rem game (gt3launcher.exe, gt3pc.exe, and SDL3.dll, which both load) and
rem engine.txt, the name the launcher's engine selector lists it by.
rem
rem     "Build Mod Plugin.cmd" [name]
rem
rem name: the engine's name and its folder's, build\plugins\<name> (default
rem "GT3 PC Mod").  The folder is what a user adds in the launcher's engine
rem selector (Add engine...).  The same programs run as an engine there - the
rem selector starts the folder's launcher with --engine --shared <its own
rem folder>, so the settings (gt3pc.ini) are the user's one file - and by
rem themselves when started directly.  What the engine keeps for itself is
rem written into the folder as it is used, never by this script: its mod
rem folders and disc images (gt3pc_engine.ini), its memory cards (memcard1,
rem memcard2; since 2026-11-15 its save folders, saves\<its name>\ beside the
rem shared gt3pc.ini, the slots' in gt3pc_engine.ini [Saves]), its installed
rem versions (data_US ...) and its caches.  Packaging
rem again copies the programs over the old ones and leaves all that alone.
rem A mod's data is not part of the plugin: a mod folder of its own
rem (docs\launcher.md "Engines").
setlocal
set "ROOT=%~dp0"
set "NAME=%~1"
set "NAMED=1"
if "%NAME%"=="" set "NAMED=" & set "NAME=GT3 PC Mod"
call "%ROOT%build.cmd"
if errorlevel 1 exit /b 1
set "OUT=%ROOT%build\plugins\%NAME%"
if not exist "%OUT%\" mkdir "%OUT%"
if not exist "%OUT%\" echo The folder "%OUT%" could not be made: a name a folder cannot have? & exit /b 1
rem (no parentheses in the texts of this block: one would end it)
for %%F in (gt3launcher.exe gt3pc.exe SDL3.dll) do (
    if not exist "%ROOT%build\bin\%%F" echo build\bin\%%F is missing: the build did not make it. & exit /b 1
    copy /Y "%ROOT%build\bin\%%F" "%OUT%\" >nul
    if errorlevel 1 echo "%OUT%\%%F" could not be written: is that engine running? & exit /b 1
)
rem (a name given, or none yet: an engine.txt edited by hand is kept otherwise)
if defined NAMED goto name
if exist "%OUT%\engine.txt" goto done
:name
>"%OUT%\engine.txt" echo(%NAME%
:done
echo.
echo Engine plugin "%NAME%":
echo   %OUT%
echo Add that folder in the launcher's engine selector (Add engine...).
exit /b 0
