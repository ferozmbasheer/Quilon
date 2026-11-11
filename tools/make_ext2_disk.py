#!/usr/bin/env python3
"""
make_ext2_disk.py — build an ext2 disk image for Quilon (section 16.2).

Usage:
    python3 tools/make_ext2_disk.py DISK_IMG [ELF_FILE ...]

The ext2 counterpart of make_disk.py.  It stages the same content into a
directory tree and hands that to mke2fs -d, so the image ends up holding the
same files as the FAT16 disk plus a couple that only ext2 can represent.

File naming matches make_disk.py — the uppercased basename, e.g.
shell/shell.elf → SHELL.ELF — because the kernel launches "SHELL.ELF" by
exact name and the ext2 driver, unlike FAT16, is case-sensitive.

Extras that exist only here, to exercise what FAT16 cannot do:
    /LONGFILENAME.TXT   a name longer than 8.3
    /SUB/NESTED.TXT     a file inside a subdirectory

Requires mke2fs (e2fsprogs) on the host.  Nothing else in the build depends
on this script: `make disk` still produces the FAT16 disk.img, and the kernel
mounts whichever filesystem it finds.

Called by:  user/Makefile — ext2disk target
"""

import os
import shutil
import subprocess
import sys
import tempfile

if len(sys.argv) < 2:
    print(f"usage: {sys.argv[0]} DISK_IMG [ELF_FILE ...]", file=sys.stderr)
    sys.exit(1)

OUT       = sys.argv[1]
ELF_PATHS = sys.argv[2:]

# 8 MiB with 1 KiB blocks.  1 KiB blocks keep the image close to the FAT16
# one in size and exercise the driver's indirect-block paths sooner.
BLOCK_SIZE   = 1024
BLOCK_COUNT  = 8192

if shutil.which("mke2fs") is None:
    print("error: mke2fs not found — install e2fsprogs to build an ext2 image",
          file=sys.stderr)
    sys.exit(1)

# ── Fixed text files (same content as the FAT16 image) ───────────────────────
FIXED = [
    ("README.TXT",
     b"Hello from Quilon OS!\r\n"
     b"This file lives on an ext2 disk read via the ATA driver.\r\n"
     b"Type 'ls' to list files, 'cat <file>' to read them.\r\n"
     b"Type 'exec SHELL.ELF' to start the ring-3 shell.\r\n"),

    ("GREET.TXT",
     b"Greetings, kernel hacker!\r\n"
     b"You are running Quilon on a bare-metal i386 kernel.\r\n"),

    ("NUMBERS.TXT",
     b"one two three four five six seven eight nine ten\r\n"),

    # ext2-only: a name FAT16's 8.3 format could not store.
    ("LONGFILENAME.TXT",
     b"ext2 stores names of up to 255 characters.\r\n"
     b"FAT16 could only manage 8.3, so this file proves the difference.\r\n"),
]

# ── Stage the tree, then let mke2fs turn it into a filesystem ────────────────
staging = tempfile.mkdtemp(prefix="quilon-ext2-")
try:
    for name, data in FIXED:
        with open(os.path.join(staging, name), "wb") as f:
            f.write(data)

    # ext2-only: a real subdirectory, which the FAT16 driver cannot descend.
    os.mkdir(os.path.join(staging, "SUB"))
    with open(os.path.join(staging, "SUB", "NESTED.TXT"), "wb") as f:
        f.write(b"This file lives in /SUB on an ext2 filesystem.\r\n"
                b"Reaching it needs real directory traversal.\r\n")

    for path in ELF_PATHS:
        if not os.path.isfile(path):
            print(f"  warning: {path} not found, skipping", file=sys.stderr)
            continue
        dest = os.path.basename(path).upper()
        shutil.copyfile(path, os.path.join(staging, dest))
        print(f"  added {dest:<16} {os.path.getsize(path):>7} bytes")

    if os.path.exists(OUT):
        os.remove(OUT)

    # -O ^dir_index: without it mke2fs may build hashed directory indexes,
    #                which the driver's plain linear directory walk ignores.
    # -O ^resize_inode: drops the reserved GDT blocks the driver never uses.
    # -I 128: 128-byte inodes, the classic ext2 size (the driver handles
    #         larger ones, but this keeps the image conventional and small).
    subprocess.run(
        ["mke2fs", "-q", "-F", "-t", "ext2",
         "-b", str(BLOCK_SIZE), "-I", "128",
         "-O", "^dir_index,^resize_inode",
         "-d", staging, OUT, str(BLOCK_COUNT)],
        check=True)
finally:
    shutil.rmtree(staging, ignore_errors=True)

print(f"  wrote {OUT}: {BLOCK_COUNT * BLOCK_SIZE} bytes "
      f"({BLOCK_COUNT} x {BLOCK_SIZE}-byte blocks, ext2)")
