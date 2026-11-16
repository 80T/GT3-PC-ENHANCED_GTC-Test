@echo off
rem GPU A/B run 7: the alpha mirror's passes folded into their draws as one
rem more instance (GT3PC_PGL_MIRROR_FOLD=1; off by default since the user's
rem run of 2026-11-05: on a GTX 1660 SUPER the fold cost the GPU more than
rem the passes - cars 7.5 -> 11.3 ms, the frame 14.1 -> 17.9 - as writing
rem SV_Depth and reading the depth buffer as a texture lose the early depth
rem test).  The pictures are the same either way; the frame's GPU time
rem against run 1 (gpu_ab_1_plain.cmd) is what the fold costs or saves on
rem the GPU at hand.  The log goes to diag\gpu_ab_mirrorfold.log.
call "%~dp0gpu_ab.cmd" mirrorfold "GT3PC_PGL_MIRROR_FOLD=1"
