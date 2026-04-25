#!/usr/bin/env python3
"""
make_disk.py — build a FAT16 disk image for Quilon.

Usage:
    python3 tools/make_disk.py DISK_IMG [ELF_FILE ...]

Each ELF_FILE is added to the disk using its uppercased basename as the
FAT16 8.3 filename (e.g. shell/shell.elf → SHELL.ELF).  Missing files are
skipped with a warning.

Three fixed text files (README.TXT, GREET.TXT, NUMBERS.TXT) are always
written first.

This script is called by:
    user/Makefile  — disk target (first-class workflow)
    create_disk.sh — thin wrapper around `make -C user disk`
"""

import struct, sys, os

if len(sys.argv) < 2:
    print(f"usage: {sys.argv[0]} DISK_IMG [ELF_FILE ...]", file=sys.stderr)
    sys.exit(1)

OUT       = sys.argv[1]
ELF_PATHS = sys.argv[2:]

# ── Disk geometry ─────────────────────────────────────────────────────────────
SS    = 512   # bytes per sector
TOTAL = 2048  # total sectors → 1 MB image

# ── BPB parameters ────────────────────────────────────────────────────────────
BPS  = 512   # bytes per sector
SPC  = 1     # sectors per cluster
RSC  = 1     # reserved sector count (sector 0 = boot)
NF   = 2     # number of FAT copies
REC  = 64    # root entry count (64 × 32 B = 4 sectors)
SPF  = 4     # sectors per FAT (1024 FAT16 entries — enough for 1 MB)

# ── Derived layout ────────────────────────────────────────────────────────────
FAT_LBA   = RSC
ROOT_LBA  = FAT_LBA + NF * SPF
ROOT_SECS = (REC * 32 + SS - 1) // SS
DATA_LBA  = ROOT_LBA + ROOT_SECS   # cluster 2 starts here

disk = bytearray(TOTAL * SS)

# ── Sector 0: boot sector / BPB ───────────────────────────────────────────────
b = disk
b[0:3]   = b'\xEB\x58\x90'   # JMP short + NOP
b[3:11]  = b'QUILON  '        # OEM name
struct.pack_into('<H', b, 0x0B, BPS)
b[0x0D]  = SPC
struct.pack_into('<H', b, 0x0E, RSC)
b[0x10]  = NF
struct.pack_into('<H', b, 0x11, REC)
struct.pack_into('<H', b, 0x13, TOTAL)
b[0x15]  = 0xF8                # media descriptor: fixed disk
struct.pack_into('<H', b, 0x16, SPF)
struct.pack_into('<H', b, 0x18, 63)
struct.pack_into('<H', b, 0x1A, 255)
struct.pack_into('<I', b, 0x1C, 0)
struct.pack_into('<I', b, 0x20, 0)
b[0x1FE] = 0x55
b[0x1FF] = 0xAA

# ── FAT (two copies) ──────────────────────────────────────────────────────────
fat = bytearray(SPF * SS)
struct.pack_into('<H', fat, 0, 0xFFF8)   # entry 0: media descriptor
struct.pack_into('<H', fat, 2, 0xFFFF)   # entry 1: reserved

# ── Helper: convert basename to padded 8.3 fields ────────────────────────────
def name_to_83(filename):
    """'shell.elf' → ('SHELL   ', 'ELF')"""
    upper = filename.upper()
    if '.' in upper:
        base, ext = upper.rsplit('.', 1)
    else:
        base, ext = upper, ''
    return base[:8].ljust(8), ext[:3].ljust(3)

# ── Fixed text files ──────────────────────────────────────────────────────────
files = [
    ("README  ", "TXT",
     b"Hello from Quilon OS!\r\n"
     b"This file lives on a FAT16 disk read via the ATA PIO driver.\r\n"
     b"Type 'ls' to list files, 'cat <file>' to read them.\r\n"
     b"Type 'exec SHELL.ELF' to start the ring-3 shell.\r\n"),

    ("GREET   ", "TXT",
     b"Greetings, kernel hacker!\r\n"
     b"You are running Quilon on a bare-metal i386 kernel.\r\n"),

    ("NUMBERS ", "TXT",
     b"one two three four five six seven eight nine ten\r\n"),
]

# ── ELF binaries ─────────────────────────────────────────────────────────────
for path in ELF_PATHS:
    if not os.path.isfile(path):
        print(f"  warning: {path} not found, skipping", file=sys.stderr)
        continue
    with open(path, 'rb') as f:
        data = f.read()
    name8, ext3 = name_to_83(os.path.basename(path))
    files.append((name8, ext3, data))

# ── Write files into data region, building FAT chain ─────────────────────────
entries = []
next_cl = 2

for name8, ext3, content in files:
    start      = next_cl
    n_clusters = max(1, (len(content) + SS * SPC - 1) // (SS * SPC))

    for i in range(n_clusters):
        lba   = DATA_LBA + (next_cl - 2) * SPC
        chunk = content[i * SS * SPC : (i + 1) * SS * SPC]
        disk[lba * SS : lba * SS + len(chunk)] = chunk

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
    struct.pack_into('<H', disk, e + 26, cl)
    struct.pack_into('<I', disk, e + 28, size)

with open(OUT, 'wb') as f:
    f.write(disk)

print(f"disk: {OUT}  ({TOTAL * SS // 1024} KB FAT16, {len(entries)} files)")
for name8, ext3, cl, size in entries:
    n_cl = max(1, (size + SS * SPC - 1) // (SS * SPC))
    print(f"  {name8.strip()}.{ext3.strip():<3}  {size:>6} bytes"
          f"  clusters {cl}–{cl + n_cl - 1}")
