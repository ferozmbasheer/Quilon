#!/bin/sh
set -e
. ./iso.sh

# Rebuild the FAT16 disk image if missing or any user source is newer.
# create_disk.sh delegates to user/Makefile which auto-discovers all programs.
_needs_disk=0
[ ! -f disk.img ] && _needs_disk=1
if [ "$_needs_disk" = "0" ]; then
    for _f in user/link.ld user/libc/Makefile; do
        [ "$_f" -nt disk.img ] && { _needs_disk=1; break; }
    done
fi
if [ "$_needs_disk" = "0" ]; then
    # Check any *.c or *.S under user/ (busybox-safe find)
    if find user -name "*.c" -newer disk.img -o -name "*.S" -newer disk.img \
            2>/dev/null | grep -q .; then
        _needs_disk=1
    fi
fi
if [ "$_needs_disk" = "1" ]; then
    echo "Rebuilding disk.img..."
    ./create_disk.sh
fi

GDK_BACKEND=x11 \
qemu-system-$(./target-triplet-to-arch.sh $HOST) \
    -boot order=d \
    -cdrom quilon.iso \
    -drive file=disk.img,format=raw,if=ide,index=1 \
    -device rtl8139,netdev=net0 \
    -netdev user,id=net0 \
    -smp 2 \
    -display gtk \
    -serial stdio
