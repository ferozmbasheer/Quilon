#!/bin/sh
set -e
. ./iso.sh

# Rebuild the FAT16 disk image if it doesn't exist or the ELF source changed.
if [ ! -f disk.img ] || [ user/hello.S -nt disk.img ] || [ user/link.ld -nt disk.img ]; then
    echo "Rebuilding disk.img (ELF source changed or image missing)..."
    ./create_disk.sh
fi

qemu-system-$(./target-triplet-to-arch.sh $HOST) \
    -boot order=d \
    -cdrom quilon.iso \
    -drive file=disk.img,format=raw,if=ide,index=1 \
    -serial stdio
