@echo off
rem GPU A/B run 6: every lens-flare occlusion query drawn as a pass of its
rem own (ps_flare's loop in one pixel), as before 2026-11-05, instead of the
rem frame's queries as one batch.  The glows are the same either way; the
rem frame's GPU time against run 1 (gpu_ab_1_plain.cmd) is what the batch
rem saves.  The log goes to diag\gpu_ab_flareserial.log.
call "%~dp0gpu_ab.cmd" flareserial "GT3PC_PGL_FLARE_BATCH=0"
