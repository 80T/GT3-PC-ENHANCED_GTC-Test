@echo off
rem GPU A/B run 3 of 5: the alpha mirror's passes not drawn (counted still:
rem MIRROR PASS in the F3 view) - the DATE tests are wrong where a draw's
rem alpha bit varies, so the reflections may show in wrong places.  The
rem frame's GPU time against run 1 is what the mirror passes cost.  The log
rem goes to diag\gpu_ab_nomirror.log.
call "%~dp0gpu_ab.cmd" nomirror "GT3PC_PGL_MIRROR_PASS=0"
