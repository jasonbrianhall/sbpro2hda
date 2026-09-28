#!/bin/sh
# JLOAD only accepts "PX" binaries: change the PE signature "PE\0\0" to "PX\0\0".
set -e
off=$(od -An -tu4 -j60 -N4 "$1" | tr -d ' ')
printf 'X' | dd of="$1" bs=1 seek=$((off + 1)) conv=notrunc 2>/dev/null
