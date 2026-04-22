#!/bin/sh
# create_disk.sh — create a FAT16 disk image for the Quilon filesystem demo.
#
# Uses Python 3 to write the image directly — no mkdosfs / dosfstools needed.
#
# Usage:
#   ./create_disk.sh         # creates disk.img
#   ./qemu.sh                # boots with disk attached (creates it first if needed)
#
# Files written to the disk:
#   README.TXT   — general greeting
#   GREET.TXT    — short message
#   NUMBERS.TXT  — a line of numbers
#   HELLO.ELF    — real ELF32 i386 user program (built from user/hello.S)
#
# To run the ELF once the kernel boots:
#   quilon> exec HELLO.ELF

set -e
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
DISK="$SCRIPT_DIR/disk.img"
ELF_FILE=""

# ── Build user/hello.elf if the cross-compiler is available ─────────────────
if command -v i686-elf-gcc >/dev/null 2>&1; then
    echo "Building user/hello.elf..."
    (cd "$SCRIPT_DIR/user" && make -s)
    ELF_FILE="$SCRIPT_DIR/user/hello.elf"
    echo "Built: $ELF_FILE ($(wc -c < "$ELF_FILE") bytes)"
else
    echo "Warning: i686-elf-gcc not found — HELLO.ELF will not be included."
    echo "         Install the cross-compiler to get the ELF demo on disk."
fi

# ── Build the FAT16 image ────────────────────────────────────────────────────
python3 - "$DISK" "$ELF_FILE" <<'PYEOF'
import struct, sys, os

OUT      = sys.argv[1]
ELF_PATH = sys.argv[2] if len(sys.argv) > 2 else ""

SS    = 512    # bytes per sector
TOTAL = 1024   # total sectors → 512 KB image (room for the ELF + text files)

# ── BPB parameters ───────────────────────────────────────────────────────────
BPS  = 512   # bytes per sector
SPC  = 1     # sectors per cluster
RSC  = 1     # reserved sector count  (sector 0 = boot)
NF   = 2     # number of FAT copies
REC  = 64    # root entry count       (64 × 32 B = 4 sectors)
SPF  = 2     # sectors per FAT        (512 FAT16 entries → enough for 512 KB)

# ── Derived layout ───────────────────────────────────────────────────────────
FAT_LBA   = RSC                          # 1
ROOT_LBA  = FAT_LBA + NF * SPF          # 5
ROOT_SECS = (REC * 32 + SS - 1) // SS   # 4  (64 entries × 32 B = 2048 B)
DATA_LBA  = ROOT_LBA + ROOT_SECS        # 9  (cluster 2 = first data cluster)

disk = bytearray(TOTAL * SS)

# ── Sector 0: boot sector / BPB ──────────────────────────────────────────────
b = disk
b[0:3]   = b'\xEB\x58\x90'   # JMP short + NOP
b[3:11]  = b'QUILON  '        # OEM name (8 bytes)
struct.pack_into('<H', b, 0x0B, BPS)
b[0x0D]  = SPC
struct.pack_into('<H', b, 0x0E, RSC)
b[0x10]  = NF
struct.pack_into('<H', b, 0x11, REC)
struct.pack_into('<H', b, 0x13, TOTAL)  # total sector count (16-bit field)
b[0x15]  = 0xF8                         # media descriptor: fixed disk
struct.pack_into('<H', b, 0x16, SPF)
struct.pack_into('<H', b, 0x18, 63)     # sectors per track (CHS, ignored)
struct.pack_into('<H', b, 0x1A, 255)    # number of heads (CHS, ignored)
struct.pack_into('<I', b, 0x1C, 0)      # hidden sectors
struct.pack_into('<I', b, 0x20, 0)      # total sectors 32-bit (0 = use 16-bit)
b[0x1FE] = 0x55
b[0x1FF] = 0xAA

# ── FAT (two copies) ─────────────────────────────────────────────────────────
fat = bytearray(SPF * SS)
struct.pack_into('<H', fat, 0, 0xFFF8)  # entry 0: media descriptor copy
struct.pack_into('<H', fat, 2, 0xFFFF)  # entry 1: reserved

# ── File list ─────────────────────────────────────────────────────────────────
# Each tuple: (8-char name, 3-char ext, bytes content)
files = [
    ("README  ", "TXT",
     b"Hello from Quilon OS!\r\n"
     b"This file lives on a FAT16 disk read via the ATA PIO driver.\r\n"
     b"Type 'ls' to list files, 'cat <file>' to read them.\r\n"
     b"Type 'exec HELLO.ELF' to run the ELF demo program.\r\n"),

    ("GREET   ", "TXT",
     b"Greetings, kernel hacker!\r\n"
     b"You are running Quilon on a bare-metal i386 kernel.\r\n"),

    ("NUMBERS ", "TXT",
     b"one two three four five six seven eight nine ten\r\n"),
]

# Add the ELF binary if it was built
if ELF_PATH and os.path.isfile(ELF_PATH):
    with open(ELF_PATH, 'rb') as f:
        elf_data = f.read()
    files.append(("HELLO   ", "ELF", elf_data))

# ── Write files into data region, building FAT chain ─────────────────────────
entries = []    # (name8, ext3, start_cluster, byte_size)
next_cl = 2     # cluster 2 is the first allocatable cluster

for name8, ext3, content in files:
    start      = next_cl
    n_clusters = (len(content) + SS * SPC - 1) // (SS * SPC)

    for i in range(n_clusters):
        # Write one cluster's worth of data (SPC sectors = SPC × 512 bytes)
        lba   = DATA_LBA + (next_cl - 2) * SPC
        chunk = content[i * SS * SPC : (i + 1) * SS * SPC]
        disk[lba * SS : lba * SS + len(chunk)] = chunk

        # FAT chain: point this cluster at the next, or mark end-of-chain
        nxt = next_cl + 1 if i < n_clusters - 1 else 0xFFFF
        struct.pack_into('<H', fat, next_cl * 2, nxt)
        next_cl += 1

    entries.append((name8, ext3, start, len(content)))

# Write both FAT copies
for fi in range(NF):
    off = (FAT_LBA + fi * SPF) * SS
    disk[off : off + SPF * SS] = fat

# ── Root directory entries ────────────────────────────────────────────────────
for idx, (name8, ext3, cl, size) in enumerate(entries):
    e = ROOT_LBA * SS + idx * 32
    disk[e   : e+8]  = name8.encode('ascii')
    disk[e+8 : e+11] = ext3.encode('ascii')
    disk[e+11]       = 0x20   # attribute: archive
    struct.pack_into('<H', disk, e + 26, cl)    # first cluster
    struct.pack_into('<I', disk, e + 28, size)  # file size in bytes

with open(OUT, 'wb') as f:
    f.write(disk)

print(f"Created {OUT}  ({TOTAL * SS // 1024} KB FAT16, {len(entries)} files)")
for name8, ext3, cl, size in entries:
    n_cl = (size + SS * SPC - 1) // (SS * SPC) if size > 0 else 1
    print(f"  {name8.strip()}.{ext3:<3}  {size:>6} bytes  "
          f"clusters {cl}–{cl + n_cl - 1}")
PYEOF
