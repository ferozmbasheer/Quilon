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

set -e
DISK=disk.img

python3 - "$DISK" <<'PYEOF'
import struct, sys

OUT   = sys.argv[1]
SS    = 512          # bytes per sector
TOTAL = 512          # total sectors  → 256 KB image

# ── BPB parameters ──────────────────────────────────────────────────────────
BPS  = 512   # bytes per sector
SPC  = 1     # sectors per cluster
RSC  = 1     # reserved sector count  (sector 0 = boot)
NF   = 2     # number of FAT copies
REC  = 64    # root entry count       (64 × 32 B = 2 sectors)
SPF  = 1     # sectors per FAT        (256 FAT16 entries → 504 clusters max)

# ── Derived layout ───────────────────────────────────────────────────────────
FAT_LBA      = RSC                          # 1
ROOT_LBA     = FAT_LBA + NF * SPF          # 3
ROOT_SECS    = (REC * 32 + SS - 1) // SS   # 4
DATA_LBA     = ROOT_LBA + ROOT_SECS        # 7  (cluster 2 = first data cluster)

disk = bytearray(TOTAL * SS)

# ── Sector 0: boot sector / BPB ─────────────────────────────────────────────
b = disk
b[0:3]   = b'\xEB\x58\x90'   # JMP short + NOP
b[3:11]  = b'QUILON  '        # OEM name
struct.pack_into('<H', b, 0x0B, BPS)
b[0x0D]  = SPC
struct.pack_into('<H', b, 0x0E, RSC)
b[0x10]  = NF
struct.pack_into('<H', b, 0x11, REC)
struct.pack_into('<H', b, 0x13, TOTAL)
b[0x15]  = 0xF8               # media: fixed disk
struct.pack_into('<H', b, 0x16, SPF)
struct.pack_into('<H', b, 0x18, 63)    # sectors per track
struct.pack_into('<H', b, 0x1A, 255)   # number of heads
struct.pack_into('<I', b, 0x1C, 0)     # hidden sectors
struct.pack_into('<I', b, 0x20, 0)     # total sectors 32 (0 = use 16-bit field)
b[0x1FE] = 0x55
b[0x1FF] = 0xAA

# ── FAT ──────────────────────────────────────────────────────────────────────
fat = bytearray(SS)
struct.pack_into('<H', fat, 0, 0xFFF8)   # entry 0: media descriptor copy
struct.pack_into('<H', fat, 2, 0xFFFF)   # entry 1: reserved

# ── Files ────────────────────────────────────────────────────────────────────
files = [
    ("README  ", "TXT",
     b"Hello from Quilon OS!\r\n"
     b"This file lives on a FAT16 disk read via the ATA PIO driver.\r\n"
     b"Type 'ls' to list files, 'cat <file>' to read them.\r\n"),

    ("GREET   ", "TXT",
     b"Greetings, kernel hacker!\r\n"
     b"You are running Quilon on a bare-metal i386 kernel.\r\n"),

    ("NUMBERS ", "TXT",
     b"one two three four five six seven eight nine ten\r\n"),
]

entries = []      # (name8, ext3, start_cluster, size)
next_cl = 2       # first allocatable cluster

for name8, ext3, content in files:
    start = next_cl
    n_sectors = (len(content) + SS - 1) // SS

    for i in range(n_sectors):
        lba   = DATA_LBA + (next_cl - 2) * SPC
        chunk = content[i * SS : (i + 1) * SS]
        disk[lba * SS : lba * SS + len(chunk)] = chunk

        nxt = next_cl + 1 if i < n_sectors - 1 else 0xFFFF
        struct.pack_into('<H', fat, next_cl * 2, nxt)
        next_cl += 1

    entries.append((name8, ext3, start, len(content)))

# Write FAT copies
for fi in range(NF):
    off = (FAT_LBA + fi * SPF) * SS
    disk[off : off + SS] = fat

# ── Root directory ────────────────────────────────────────────────────────────
for idx, (name8, ext3, cl, size) in enumerate(entries):
    e = ROOT_LBA * SS + idx * 32
    disk[e   : e+8]  = name8.encode('ascii')
    disk[e+8 : e+11] = ext3.encode('ascii')
    disk[e+11]       = 0x20   # archive attribute
    struct.pack_into('<H', disk, e + 26, cl)
    struct.pack_into('<I', disk, e + 28, size)

with open(OUT, 'wb') as f:
    f.write(disk)

print(f"Created {OUT}  ({TOTAL * SS // 1024} KB FAT16, {len(entries)} files)")
for name8, ext3, cl, size in entries:
    print(f"  {name8.strip()}.{ext3}  {size} bytes  (cluster {cl})")
PYEOF
