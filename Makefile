# Makefile for SBPRO.DLL, a Jemm Loadable Module (load with JLOAD)
VERSION=0.15

# Source files
# C files that must never touch the FPU (they run inside interrupts)
SRCS    = sbpro.c hda.c pci.c dsp.c sbout.c pic.c mpu.c gmsynth.c libc.c jlm.S
# FM synth: dbopl's one-time table setup uses the x87, everything after is integer
FPSRCS  = fpmath.c gmtables.c
CXXSRCS = opl.cpp dbopl.cpp

# Output module, and a detection test program (assumes A220 I5 D1)
DLL_TARGET = sbpro.dll
TEST_TARGET = sbtest.com

# Protected-mode side (DJGPP): SBPM runs DOS/4GW games under HDPMI32i
PM_TARGET = sbpm.exe
PMTEST_TARGET = sbtestpm.exe
DJGPP_CC ?= i586-pc-msdosdjgpp-gcc
PM_BUILD = $(DJGPP_CC) -O2 -Wall -o $(PM_TARGET) sbpm.c sbpmtrap.S && \
           $(DJGPP_CC) -O2 -Wall -o $(PMTEST_TARGET) sbtestpm.c

# Docker images: MinGW for the JEMM module, DJGPP for the DPMI programs
MINGW_IMAGE = sbpro-mingw
DJGPP_IMAGE = djfdyuruiry/djgpp

# DOS-side tools: JEMM (JEMMEX + JLOAD, matching versions) and HDPMI32i
JEMM_URL = https://github.com/Baron-von-Riedesel/Jemm/releases/download/v5.86/JemmB_v586.zip
HXRT_URL = https://github.com/Baron-von-Riedesel/HX/releases/download/v2.23/HXRT223.zip
DIST     = dist

# QEMU disk image with FreeDOS (JEMMEX + JLOAD)
DOS_IMAGE = freedos.img

# Get current user and group IDs for Docker
USER_ID = $(shell id -u)
GROUP_ID = $(shell id -g)

CC      = i686-w64-mingw32-gcc
CXX     = i686-w64-mingw32-g++
BASEFLAGS = -O2 -Wall -march=i486 -ffreestanding -fno-builtin -fno-stack-protector \
          -fno-asynchronous-unwind-tables -mno-stack-arg-probe
CFLAGS  = $(BASEFLAGS) -mgeneral-regs-only
FPFLAGS = $(BASEFLAGS) -mfpmath=387 -mno-sse
CXXFLAGS = $(FPFLAGS) -std=c++11 -fno-exceptions -fno-rtti -fno-threadsafe-statics -Wno-unused
LDFLAGS = -shared -nostdlib -Wl,--subsystem,native -Wl,-e,_DllMain@12 \
          -Wl,--image-base,0x10000000 -lgcc

# One shell line that builds everything (used both locally and in Docker)
BUILD = $(CC) $(CFLAGS) -c $(SRCS) && \
        $(CC) $(FPFLAGS) -c $(FPSRCS) && \
        $(CXX) $(CXXFLAGS) -c $(CXXSRCS) && \
        $(CC) -o $(DLL_TARGET) *.o $(LDFLAGS)

# JLOAD only accepts "PX" binaries: patch the "PE" signature
PATCH_PX = sh patchpx.sh $(DLL_TARGET)

# Default target
all: msdos

# Target to build the MinGW Docker image
pull-mingw:
	printf 'FROM debian:stable-slim\nRUN apt-get update && apt-get install -y --no-install-recommends gcc-mingw-w64-i686 g++-mingw-w64-i686 nasm && rm -rf /var/lib/apt/lists/*\n' \
		| docker build -t $(MINGW_IMAGE) -

# Target to pull the DJGPP Docker image
pull-djgpp:
	docker pull $(DJGPP_IMAGE)

# Target to download JEMMEX, JLOAD and HDPMI32i into dist/
get-dos-tools:
	mkdir -p $(DIST)
	wget -N $(JEMM_URL)
	wget -N $(HXRT_URL)
	unzip -o -j JemmB_v586.zip JEMMEX.EXE JLOAD.EXE -d $(DIST)
	unzip -o -j HXRT223.zip BIN/HDPMI32i.EXE -d $(DIST)

# Target to build SBPRO.DLL using MinGW in Docker
msdos: pull-mingw pull-djgpp get-dos-tools
	docker run --rm -v $(PWD):/src:z -u $(USER_ID):$(GROUP_ID) $(MINGW_IMAGE) /bin/sh -c "cd /src && \
	rm -f *.o && $(BUILD) && \
	$(PATCH_PX) && \
	nasm -f bin -o $(TEST_TARGET) sbtest.asm"
	docker run --rm -v $(PWD):/src:z -u $(USER_ID):$(GROUP_ID) $(DJGPP_IMAGE) /bin/sh -c "cd /src && \
	$(subst $(DJGPP_CC),gcc,$(PM_BUILD))"
	cp $(DLL_TARGET) $(DIST)/SBPRO.DLL
	cp $(TEST_TARGET) $(DIST)/SBTEST.COM
	cp $(PM_TARGET) $(DIST)/SBPM.EXE
	cp $(PMTEST_TARGET) $(DIST)/SBTESTPM.EXE

# Target to build with a locally installed MinGW (no Docker)
local:
	rm -f *.o
	$(BUILD)
	$(PATCH_PX)
	nasm -f bin -o $(TEST_TARGET) sbtest.asm
	$(PM_BUILD)

# Target to run in QEMU with Intel HD Audio
run: msdos
	qemu-system-i386 -m 64 -hda $(DOS_IMAGE) -hdb fat:rw:$(DIST) -serial file:sbpro.log \
		-audiodev pa,id=snd0 -device intel-hda -device hda-duplex,audiodev=snd0

# Clean target to remove generated files
clean:
	rm -f $(DLL_TARGET) $(TEST_TARGET) $(PM_TARGET) $(PMTEST_TARGET) *.o JemmB_v586.zip HXRT223.zip || true
	rm -rf $(DIST) || true
	rm *.DLL || true

.PHONY: all pull-mingw pull-djgpp get-dos-tools msdos local run clean
