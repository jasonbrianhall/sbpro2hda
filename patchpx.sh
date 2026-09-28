#!/bin/sh
# Make a MinGW DLL loadable by JLOAD:
#  - change the PE signature "PE\0\0" to "PX\0\0"
#  - clear the import directory entry (ld always emits an empty one, and
#    JLOAD refuses any module that has one)
set -e
off=$(od -An -tu4 -j60 -N4 "$1" | tr -d ' ')
printf 'X' | dd of="$1" bs=1 seek=$((off + 1)) conv=notrunc 2>/dev/null
# optional header at +24, data directories at +96, import entry is #1
dd if=/dev/zero of="$1" bs=1 seek=$((off + 24 + 96 + 8)) count=8 conv=notrunc 2>/dev/null
