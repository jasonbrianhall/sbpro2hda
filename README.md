# SBPRO – Sound Blaster Pro 2.0 emulation over HD Audio for DOS

SBPRO lets DOS games play sound on machines that only have Intel HD Audio
(roughly every PC since 2004, plus QEMU, VirtualBox and libvirt/KVM guests).
Games see a Sound Blaster Pro 2.0 with an OPL3 FM chip and an MPU-401; the
sound comes out of the HD Audio controller.

It works for both real-mode games (Prince of Persia, ...) and DOS/4GW
protected-mode games (DOOM, ...), booting straight into FreeDOS or MS-DOS.

## What is emulated

- **Sound Blaster Pro 2.0 DSP** (version 3.02): 8-bit mono/stereo DMA playback,
  single-cycle and auto-init, high-speed modes, direct DAC, pause/continue,
  speaker on/off, IRQ on command F2h, mixer.
- **OPL3 FM** at 388h and the SB ports (DOSBox's dbopl), including AdLib
  detection.
- **MPU-401 UART** at 330h with a built-in General MIDI synthesizer (36 voices
  on two extra OPL3s, instrument bank from felixterminal).
- **8237 DMA and 8259 PIC** behaviour the games rely on: DMA count/address
  readback, interrupt masks, IRQ priority.

Not emulated: Sound Blaster 16 (16-bit DMA, DSP 4.xx) and ADPCM.

## Requirements

- DOS (FreeDOS 1.3/1.4 or MS-DOS) on a 386 or later.
- **JEMMEX**, loaded from CONFIG.SYS (5.86 is included in the release; tested
  down to 5.83). Before 5.84, JEMM can't share the DMA ports, so SBPRO says so
  at load and skips DMA counter readback; games that don't poll the DMA
  count are unaffected.
- An Intel HD Audio controller with speakers, line out, headphones or an
  HDMI/DisplayPort monitor.

## Install

Download `sbpro-VERSION.zip` from the releases page and unzip it to
`C:\SBPRO`. Then:

```
CONFIG.SYS (FDCONFIG.SYS):   DEVICE=C:\SBPRO\JEMMEX.EXE
AUTOEXEC.BAT (last line):    CALL C:\SBPRO\SBPRO.BAT
```

Start games from the prompt `SBPRO.BAT` opens. Real-mode and protected-mode
games both have sound there; `EXIT` leaves it.

| Script       | Loads                                                     |
|--------------|-----------------------------------------------------------|
| `SBPRO.BAT`  | driver + DOS extender (HDPMI32i) + SBPM prompt: all games |
| `SBREAL.BAT` | driver only: real-mode games, no extra prompt             |

Games should be set up for **Sound Blaster Pro (or compatible), port 220,
IRQ 5, DMA 1**, and General MIDI at port 330 where offered.

### Options

Options go after the script name, e.g. `CALL C:\SBPRO\SBPRO.BAT /H2`.

| Option  | Meaning                                                        |
|---------|----------------------------------------------------------------|
| `/H2`   | use the second HD Audio controller (`/H1`, `/H3`, ...)         |
| `/HDMI` | play through HDMI/DisplayPort instead of speakers/line out     |
| `/D`    | debug log on COM1, 115200 baud                                 |
| `/T`    | test tone instead of emulation                                 |

`BLASTER` defaults to `A220 I5 D1 P330 T4`; set it before calling the script
to use other resources (`P0` turns the MPU-401 off). If HD Audio itself uses
IRQ 5, SBPRO says so: pick another IRQ, e.g. `I7`.

Set `SBDIR` first if SBPRO is not in `C:\SBPRO`.

## Test programs

| Program        | Checks                                                        |
|----------------|---------------------------------------------------------------|
| `SBTEST.COM`   | detection, DMA, IRQs, FM and MIDI from real mode              |
| `SBTESTPM.EXE` | the same from protected mode (run it from the SBPRO prompt)   |
| `KBTEST.COM`   | keyboard presses/releases and PS/2 mouse data arrive during sound (boot without a mouse driver for the mouse part) |

## How it works

- `SBPRO.DLL` is a Jemm Loadable Module (loaded with JLOAD). It traps the Sound
  Blaster, FM, MPU-401, DMA and PIC ports for virtual-8086 code, runs the DSP,
  FM and MIDI emulation, mixes everything and streams it to HD Audio from the
  controller's interrupt. The Sound Blaster IRQ is delivered to the game from
  there.
- `SBPM.EXE` (DJGPP) covers protected-mode games: under HDPMI32i it registers
  port traps for the same ports and forwards the accesses to SBPRO. With no
  arguments it opens a command prompt; `SBPM GAME.EXE` runs one program.
- When nothing plays for a second, SBPRO pauses the HD Audio stream; the next
  access to the sound ports starts it again.

Memory: SBPRO itself uses about 170 KB of extended memory; the protected-mode
side (HDPMI32i + SBPM) about 1 MB more. On an 8 MB machine DOOM still gets
about 3.3 MB.

## Testing in QEMU

This command avoids doubled/stuck keys under TCG (`-cpu pentium3` and the
`clock=vm` RTC matter):

```
qemu-system-i386 \
  -machine pc,accel=tcg \
  -cpu pentium3 \
  -m 64 \
  -rtc base=localtime,clock=vm,driftfix=none \
  -audiodev id=hostaudio,driver=pa \
  -device intel-hda,id=hda0 \
  -device hda-duplex,audiodev=hostaudio,bus=hda0.0 \
  -hda /var/lib/libvirt/images/freedos14.qcow2 \
  -boot c
```

Also tested under VirtualBox. For libvirt: CPU model `pentium3`, clock
`offset="localtime"` with `<timer name="rtc" tickpolicy="delay" track="guest"/>`,
PS/2 mouse and keyboard only (no USB tablet), sound model `ich6` or `ich9`.

`make run` starts QEMU with the same settings on `freedos.img`, with the
build output as a second drive.

## Building

```
make msdos package     # Docker: MinGW + DJGPP, downloads JEMM and HDPMI32i,
                       # builds everything and sbpro-VERSION.zip
make local package     # same with locally installed i686-w64-mingw32 and
                       # i586-pc-msdosdjgpp toolchains
```

The version lives in the Makefile (`VERSION=`). GitHub Actions
(`.github/workflows/release.yml`) builds every push; a push to the default
branch publishes release `vVERSION` with the zip attached.

## Real hardware

So far SBPRO has only been tested in QEMU and VirtualBox. It sets up the
controller the way Linux does (Intel TCSEL, snooping on AMD/ATI, NVIDIA and
Intel SCH), but real codecs and laptops vary. If you try it on a real PC,
please open an issue with:

- the machine or motherboard, and whether it worked,
- the COM1 log from loading with `/D` (null-modem cable, 115200 baud). It
  starts with the controller's PCI ID and a dump of the codec's widgets and
  pin configuration, which is usually enough to see why a machine stays
  silent.

## Debugging

Load with `/D` and capture COM1 (QEMU: `-serial file:sbpro.log`). The log
shows the HD Audio controller and codec layout, port traffic, transfers, IRQ delivery, and once a second the time spent
in SBPRO's interrupt handler.

## Credits

- OPL3 emulation: DOSBox's dbopl, via
  [felixterminal](https://github.com/jasonbrianhall/felixterminal), which also
  supplied the General MIDI instrument bank.
- JEMMEX, JLOAD and HDPMI32i by Japheth
  ([Jemm](https://github.com/Baron-von-Riedesel/Jemm),
  [HX](https://github.com/Baron-von-Riedesel/HX)); the release includes them
  with their own license texts.
