@echo off
rem GPU A/B run 1 of 5 (diag\gpu_ab.cmd): the plain run - everything drawn,
rem the GPU sections on.  Read the F3 monitor's GPU line (ms) and the counts
rem MIRROR PASS / MIRROR SYNC during the SSR5 race at 150 %; the log goes to
rem diag\gpu_ab_plain.log.
call "%~dp0gpu_ab.cmd" plain
