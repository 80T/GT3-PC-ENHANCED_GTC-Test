@echo off
rem GPU A/B run 4 of 5: every C = Ad draw of two instances (the env passes,
rem the tyres, the course's glint) drawn once - the reflections at half
rem strength.  The frame's GPU time against run 1 is what their second
rem rasterization costs.  The log goes to diag\gpu_ab_adonce.log.
call "%~dp0gpu_ab.cmd" adonce "GT3PC_PGL_AD_ONCE=1"
