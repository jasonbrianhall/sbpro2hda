# Makefile for SBPRO.DLL, a Jemm Loadable Module (load with JLOAD)
VERSION=0.22

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
VERDEF = -DSBPRO_VERSION_NUM=$(VERSION)
PM_BUILD = $(DJGPP_CC) -O2 -Wall $(VERDEF) -o $(PM_TARGET) sbpm.c sbpmtrap.S && \
           $(DJGPP_CC) -O2 -Wall $(VERDEF) -o $(PMTEST_TARGET) sbtestpm.c

# Docker images: MinGW for the JEMM module, DJGPP for the DPMI programs
MINGW_IMAGE = sbpro-mingw
DJGPP_IMAGE = djfdyuruiry/djgpp

# DOS-side tools: JEMM (JEMMEX + JLOAD, matching versions) and HDPMI32i
JEMM_ZIP = JemmB_v586.zip
HXRT_ZIP = HXRT223.zip
JEMM_URL = https://github.com/Baron-von-Riedesel/Jemm/releases/download/v5.86/$(JEMM_ZIP)
HXRT_URL = https://github.com/Baron-von-Riedesel/HX/releases/download/v2.23/$(HXRT_ZIP)
DIST     = dist

# Release package: sbpro-VERSION.zip with everything under SBPRO\ (DOS names)
PKG_DIR  = release/SBPRO
PKG_ZIP  = sbpro-$(VERSION).zip

# QEMU disk image with FreeDOS (JEMMEX + JLOAD)
DOS_IMAGE = freedos.img

# Get current user and group IDs for Docker
USER_ID = $(shell id -u)
GROUP_ID = $(shell id -g)

CC      = i686-w64-mingw32-gcc
CXX     = i686-w64-mingw32-g++
BASEFLAGS = -O2 -Wall -march=i486 -ffreestanding -fno-builtin -fno-stack-protector \
          -fno-asynchronous-unwind-tables -mno-stack-arg-probe $(VERDEF)
CFLAGS  = $(BASEFLAGS) -mgeneral-regs-only
FPFLAGS = $(BASEFLAGS) -mfpmath=387 -mno-sse
CXXFLAGS = $(FPFLAGS) -std=c++11 -fno-exceptions -fno-rtti -fno-threadsafe-statics -Wno-unused
LDFLAGS = -shared -nostdlib -Wl,--subsystem,native -Wl,-e,_DllMain@12 \
          -Wl,--image-base,0x10000000 -lgcc

# One shell line that builds the module (used both locally and in Docker)
BUILD = rm -f *.o && \
        $(CC) $(CFLAGS) -c $(SRCS) && \
        $(CC) $(FPFLAGS) -c $(FPSRCS) && \
        $(CXX) $(CXXFLAGS) -c $(CXXSRCS) && \
        $(CC) -o $(DLL_TARGET) *.o $(LDFLAGS) && \
        sh patchpx.sh $(DLL_TARGET) && \
        nasm -f bin $(VERDEF) -o $(TEST_TARGET) sbtest.asm && \
        nasm -f bin -o kbtest.com kbtest.asm

# Default target
all: msdos

# Target to build the MinGW Docker image
pull-mingw:
	printf 'FROM debian:stable-slim\nRUN apt-get update && apt-get install -y --no-install-recommends gcc-mingw-w64-i686 g++-mingw-w64-i686 nasm && rm -rf /var/lib/apt/lists/*\n' \
		| docker build -t $(MINGW_IMAGE) -

# Target to pull the DJGPP Docker image
pull-djgpp:
	docker pull $(DJGPP_IMAGE)

# Target to download JEMMEX, JLOAD and HDPMI32i (plus their licenses) into dist/
get-dos-tools:
	mkdir -p $(DIST)
	wget -N $(JEMM_URL)
	wget -N $(HXRT_URL)
	unzip -o -j $(JEMM_ZIP) JEMMEX.EXE JLOAD.EXE Artistic.txt -d $(DIST)
	unzip -o -j $(HXRT_ZIP) BIN/HDPMI32i.EXE HXRT.TXT -d $(DIST)

# Build pieces without Docker (these are what the GitLab CI jobs run)
jlm:
	$(BUILD)

pm:
	$(PM_BUILD)

# Target to build everything using Docker, then copy into dist/
msdos: pull-mingw pull-djgpp get-dos-tools
	docker run --rm -v $(PWD):/src:z -u $(USER_ID):$(GROUP_ID) $(MINGW_IMAGE) /bin/sh -c "cd /src && $(BUILD)"
	docker run --rm -v $(PWD):/src:z -u $(USER_ID):$(GROUP_ID) $(DJGPP_IMAGE) /bin/sh -c "cd /src && \
	$(subst $(DJGPP_CC),gcc,$(PM_BUILD))"
	cp $(DLL_TARGET) $(DIST)/SBPRO.DLL
	cp $(TEST_TARGET) $(DIST)/SBTEST.COM
	cp kbtest.com $(DIST)/KBTEST.COM
	cp $(PM_TARGET) $(DIST)/SBPM.EXE
	cp $(PMTEST_TARGET) $(DIST)/SBTESTPM.EXE
	cp dos/*.BAT $(DIST)/

# Target to build with a locally installed MinGW and DJGPP (no Docker)
local: jlm pm

# Target to make the release zip from already built files (make msdos package)
package: get-dos-tools
	rm -rf release
	mkdir -p $(PKG_DIR)
	cp $(DLL_TARGET) $(PKG_DIR)/SBPRO.DLL
	cp $(PM_TARGET) $(PKG_DIR)/SBPM.EXE
	cp $(TEST_TARGET) $(PKG_DIR)/SBTEST.COM
	cp kbtest.com $(PKG_DIR)/KBTEST.COM
	cp $(PMTEST_TARGET) $(PKG_DIR)/SBTESTPM.EXE
	cp $(DIST)/JEMMEX.EXE $(DIST)/JLOAD.EXE $(PKG_DIR)/
	cp $(DIST)/HDPMI32i.EXE $(PKG_DIR)/HDPMI32I.EXE
	cp $(DIST)/Artistic.txt $(PKG_DIR)/JEMM.TXT
	cp $(DIST)/HXRT.TXT $(PKG_DIR)/HXRT.TXT
	cp dos/SBPRO.BAT dos/SBREAL.BAT dos/README.TXT $(PKG_DIR)/
	rm -f $(PKG_ZIP)
	cd release && zip -r -X ../$(PKG_ZIP) SBPRO
	@echo "Built $(PKG_ZIP)"

print-version:
	@echo $(VERSION)

# Target to run in QEMU with Intel HD Audio
run: msdos
	qemu-system-i386 -m 64 -hda $(DOS_IMAGE) -hdb fat:rw:$(DIST) -serial file:sbpro.log \
		-audiodev pa,id=snd0 -device intel-hda -device hda-duplex,audiodev=snd0

# Clean target to remove generated files
clean:
	rm -f $(DLL_TARGET) $(TEST_TARGET) kbtest.com $(PM_TARGET) $(PMTEST_TARGET) *.o $(JEMM_ZIP) $(HXRT_ZIP) sbpro-*.zip || true
	rm -rf $(DIST) release || true
	rm *.DLL || true

.PHONY: all pull-mingw pull-djgpp get-dos-tools jlm pm msdos local package print-version run clean
