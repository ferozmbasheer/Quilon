#!/bin/sh
set -e
. ./build.sh

mkdir -p isodir
mkdir -p isodir/boot
mkdir -p isodir/boot/grub

cp sysroot/boot/quilon.kernel isodir/boot/quilon.kernel
cat > isodir/boot/grub/grub.cfg << EOF
set timeout=1
set default=0
menuentry "quilon" {
	multiboot /boot/quilon.kernel
}
EOF
grub-mkrescue -o quilon.iso isodir
