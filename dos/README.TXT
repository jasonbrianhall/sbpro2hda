SBPRO - Sound Blaster Pro 2.0 emulation over HD Audio for DOS

1. CONFIG.SYS (JEMMEX is required):
      DEVICE=C:\SBPRO\JEMMEX.EXE
2. AUTOEXEC.BAT, as the last line:
      CALL C:\SBPRO\SBPRO.BAT
   Then start games from the prompt as usual (DOOM, WAR2, PRINCE ...).
   Real-mode and protected-mode (DOS/4GW) games both get sound there.
   SBREAL.BAT loads real-mode support only, without the extra prompt.

Installed somewhere else? SET SBDIR=D:\WHERE before calling the script.
Options (after SBPRO.BAT or SBREAL.BAT):
   /H2    use the second HD Audio card (/H1, /H3 ...)
   /HDMI  play through HDMI/DisplayPort instead of speakers/line out
   /D     debug log on COM1 (115200 baud)
BLASTER defaults to A220 I5 D1 P330 T4; set it first to change it.
SBTEST.COM and SBTESTPM.EXE (run it from the SBPRO prompt) test the card.
KBTEST.COM checks that key presses/releases and PS/2 mouse data all arrive
during sound (for the mouse part, boot without a mouse driver).

Included third-party tools: JEMMEX/JLOAD by Japheth (JEMM.TXT),
HDPMI32i by Japheth (HXRT.TXT). OPL3 emulation from DOSBox.
