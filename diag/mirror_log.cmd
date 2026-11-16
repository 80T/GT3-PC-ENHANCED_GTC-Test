@echo off
rem Why the alpha mirror passes are needed: a race run with
rem GT3PC_PGL_MIRROR_LOG=1 (the first 40 draws that need a pass logged with
rem their alpha ranges - the vertex alpha's bound, the texture's, TEXA - as
rem `pgl mirror:` lines) and the stats line's causes (`the texture's alpha
rem varies N, the vertex alpha N, both N, neither N`), with gpu_ab.cmd's GPU
rem sections and profile.  The log goes to diag\gpu_ab_mirrorlog.log.
call "%~dp0gpu_ab.cmd" mirrorlog "GT3PC_PGL_MIRROR_LOG=1"
