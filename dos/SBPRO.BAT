@echo off
rem Sound Blaster Pro emulation for real-mode AND protected-mode games.
rem Loads the driver, the DOS extender and opens a prompt with sound:
rem start any game from there. EXIT leaves the prompt.
rem Options go to the driver, e.g. SBPRO /H2 (second sound card) or /HDMI.
if "%SBDIR%"=="" set SBDIR=C:\SBPRO
if "%BLASTER%"=="" set BLASTER=A220 I5 D1 P330 T4
%SBDIR%\JLOAD.EXE %SBDIR%\SBPRO.DLL %BLASTER% %1 %2 %3
%SBDIR%\HDPMI32I.EXE -r -v
%SBDIR%\SBPM.EXE
