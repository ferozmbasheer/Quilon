#!/bin/sh
set -e
. ./iso.sh

# Auto-create the FAT16 disk image if it doesn't exist yet.
if [ ! -f disk.img ]; then
    echo "disk.img not found — generating FAT16 demo disk..."
    ./create_disk.sh
fi

qemu-system-$(./target-triplet-to-arch.sh $HOST) \
    -boot order=d \
    -cdrom quilon.iso \
    -drive file=disk.img,format=raw,if=ide,index=1 \
    -serial stdio
