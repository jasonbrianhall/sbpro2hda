@echo off
rem Sound Blaster Pro emulation for real-mode games only (no DOS extender,
rem no extra prompt). DOS/4GW games like DOOM need SBPRO.BAT instead.
if "%SBDIR%"=="" set SBDIR=C:\SBPRO
if "%BLASTER%"=="" set BLASTER=A220 I5 D1 P330 T4
%SBDIR%\JLOAD.EXE %SBDIR%\SBPRO.DLL %BLASTER% %1 %2 %3
