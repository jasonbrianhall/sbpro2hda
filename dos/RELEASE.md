Sound Blaster Pro 2.0 emulation over HD Audio for DOS.

Unzip `sbpro-*.zip` to `C:\SBPRO`, then:

```
CONFIG.SYS:    DEVICE=C:\SBPRO\JEMMEX.EXE
AUTOEXEC.BAT:  CALL C:\SBPRO\SBPRO.BAT      (last line)
```

Start games from that prompt: real-mode and DOS/4GW games both get sound.
See README.TXT in the zip for options (/H2, /HDMI, /D).
