@echo off
rem Letters missing on the name entry screen: open it, then press F9 in the
rem game (click into the window first) while letters are missing - during a
rem confirm, a few presses in a row (each one traces two frames).  Not while
rem paused (F5): no frame is drawn then, so none is traced.  Every draw
rem of a traced frame is logged ("pgl trace N:" lines: the glyph sprites
rem with their texture, its owner and mean texel, the draw states skipped
rem and why), and the Tex1 sets moved within a frame ("pgl tex1:" lines).
rem The game's log is copied to diag\name_entry_trace.log when the game
rem closes.
set GT3PC_PGL_TRACE=1
set GT3PC_TEX1_LOG=1
start "" /wait "%~dp0..\build\bin\gt3pc.exe"
copy /y "%~dp0..\build\bin\gt3pc.log" "%~dp0name_entry_trace.log" >nul
