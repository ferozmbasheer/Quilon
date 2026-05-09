#!/bin/sh
set -e
. ./build.sh

mkdir -p isodir
mkdir -p isodir/boot
mkdir -p isodir/boot/grub

cp sysroot/boot/quilon.kernel isodir/boot/quilon.kernel

# Build initrd containing the PSF2 bitmap font
python3 tools/make_initrd.py isodir/boot/initrd.img \
    FONT.PSF=tools/Uni2-TerminusBold16-psf2.psf

cat > isodir/boot/grub/grub.cfg << EOF
set timeout=1
set default=0
menuentry "quilon" {
	multiboot /boot/quilon.kernel
	module /boot/initrd.img
}
EOF
grub-mkrescue -o quilon.iso isodir
