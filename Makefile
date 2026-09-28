# Makefile for SBPRO.DLL, a Jemm Loadable Module (load with JLOAD)
VERSION=0.3

# Source files
SRCS = sbpro.c hda.c pci.c dsp.c libc.c jlm.S

# Output module
DLL_TARGET = sbpro.dll

# Docker image with the MinGW 32-bit cross compiler
MINGW_IMAGE = sbpro-mingw

# QEMU disk image with FreeDOS 1.3 (JEMMEX + JLOAD)
DOS_IMAGE = freedos.img

# Get current user and group IDs for Docker
USER_ID = $(shell id -u)
GROUP_ID = $(shell id -g)

CC      = i686-w64-mingw32-gcc
CFLAGS  = -O2 -Wall -march=i486 -mgeneral-regs-only -ffreestanding -fno-builtin \
          -fno-stack-protector -fno-asynchronous-unwind-tables -mno-stack-arg-probe
LDFLAGS = -shared -nostdlib -Wl,--subsystem,native -Wl,-e,_DllMain@12 \
          -Wl,--image-base,0x10000000 -lgcc

# JLOAD only accepts "PX" binaries: patch the "PE" signature
PATCH_PX = sh patchpx.sh $(DLL_TARGET)

# Default target
all: msdos

# Target to build the MinGW Docker image
pull-mingw:
	printf 'FROM debian:stable-slim\nRUN apt-get update && apt-get install -y --no-install-recommends gcc-mingw-w64-i686 && rm -rf /var/lib/apt/lists/*\n' \
		| docker build -t $(MINGW_IMAGE) -

# Target to build SBPRO.DLL using MinGW in Docker
msdos: pull-mingw
	docker run --rm -v $(PWD):/src:z -u $(USER_ID):$(GROUP_ID) $(MINGW_IMAGE) /bin/sh -c "cd /src && \
	$(CC) $(CFLAGS) $(SRCS) -o $(DLL_TARGET) $(LDFLAGS) && \
	$(PATCH_PX)"

# Target to build with a locally installed MinGW (no Docker)
local:
	$(CC) $(CFLAGS) $(SRCS) -o $(DLL_TARGET) $(LDFLAGS)
	$(PATCH_PX)

# Target to run in QEMU with Intel HD Audio
run: msdos
	qemu-system-i386 -m 64 -hda $(DOS_IMAGE) -hdb fat:rw:. \
		-audiodev pa,id=snd0 -device intel-hda -device hda-duplex,audiodev=snd0

# Clean target to remove generated files
clean:
	rm -f $(DLL_TARGET) *.o || true
	rm *.DLL || true

.PHONY: all pull-mingw msdos local run clean
