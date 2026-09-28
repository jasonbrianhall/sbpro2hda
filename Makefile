# Makefile for MS-DOS with DPMI support (runs under HDPMI32i, not CWSDPMI)
VERSION=0.2

# Source files
SRCS = sbpro.c hda.c pci.c qpi.c dsp.c

# Output executable
DOS_TARGET = sbpro.exe
DOS_COFF = sbpro

# Docker image for DJGPP
DJGPP_IMAGE = djfdyuruiry/djgpp

# Correct CSDPMI URL
CSDPMI_URL = http://na.mirror.garr.it/mirrors/djgpp/current/v2misc/csdpmi7b.zip

# QEMU disk image with FreeDOS + JEMM + HDPMI32i
DOS_IMAGE = freedos.img

# Get current user and group IDs for Docker
USER_ID = $(shell id -u)
GROUP_ID = $(shell id -g)

# Default target
all: msdos

# Target to pull the DJGPP Docker image
pull-djgpp:
	docker pull $(DJGPP_IMAGE)

# Target to download CSDPMI
get-csdpmi:
	wget $(CSDPMI_URL)
	unzip -o csdpmi7b.zip -d csdpmi

# Target to build for MS-DOS using DJGPP in Docker.
# CWSDSTUB only starts its embedded CWSDPMI when no DPMI host is loaded;
# with HDPMI32i -r resident, HDPMI is used (and required to stay resident).
msdos: pull-djgpp get-csdpmi
	docker run --rm -v $(PWD):/src:z -u $(USER_ID):$(GROUP_ID) $(DJGPP_IMAGE) /bin/sh -c "cd /src && \
	gcc -s $(SRCS) -o $(DOS_TARGET) -O2 -Wall -march=i386 -mtune=i686 && \
	exe2coff $(DOS_TARGET) && \
	cat csdpmi/bin/CWSDSTUB.EXE $(DOS_COFF) > $(DOS_TARGET)"

# Target to run in QEMU with Intel HD Audio
run: msdos
	qemu-system-i386 -m 64 -hda $(DOS_IMAGE) -hdb fat:rw:. \
		-audiodev pa,id=snd0 -device intel-hda -device hda-duplex,audiodev=snd0

# Clean target to remove generated files
clean:
	rm -f $(DOS_TARGET) $(DOS_COFF) *.o csdpmi7b.zip || true
	rm -rf csdpmi || true
	rm *.EXE || true
	rm *.zip.* || true

.PHONY: all pull-djgpp get-csdpmi msdos run clean
