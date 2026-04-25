#!/bin/sh
# create_disk.sh — build user programs and write the FAT16 disk image.
#
# Delegates to user/Makefile which auto-discovers all user/*/main.c programs.
# Running this script is equivalent to:  cd user && make disk
#
# Usage:
#   ./create_disk.sh         — builds and writes disk.img
#   ./qemu.sh                — calls this automatically if disk.img is stale

set -e
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)

if ! command -v i686-elf-gcc >/dev/null 2>&1; then
    echo "Warning: i686-elf-gcc not found — ELF files will not be built."
    echo "         Install the cross-compiler toolchain to include user programs."
fi

make -C "$SCRIPT_DIR/user" disk DISK="$SCRIPT_DIR/disk.img"
