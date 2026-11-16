@echo off
rem GPU A/B run 2 of 5: the cars' env passes left out (the ENVMTX and ENVMAP
rem programs: the reflections on the cars, with their mirror passes) - the
rem cars look matte.  The frame's GPU time against run 1 is what the env
rem passes cost.  The log goes to diag\gpu_ab_noenv.log.
call "%~dp0gpu_ab.cmd" noenv "GT3PC_PGL_SKIP=envmtx,envmap"
