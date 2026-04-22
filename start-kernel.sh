#!/bin/sh
set -e
. ./build.sh

# Auto-create the FAT16 disk image if it doesn't exist yet.
if [ ! -f disk.img ]; then
    echo "disk.img not found — generating FAT16 demo disk..."
    ./create_disk.sh
fi

qemu-system-$(./target-triplet-to-arch.sh $HOST) \
    -kernel sysroot/boot/quilon.kernel \
    -drive file=disk.img,format=raw,if=ide,index=0 \
    -serial stdio
