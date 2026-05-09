#!/usr/bin/env python3
"""Build a Quilon initrd image.

Usage:
    make_initrd.py OUTPUT NAME=FILE [NAME=FILE ...]

Format:
    [uint32_t file_count]
    [N × { char name[16]; uint32_t data_size; uint8_t data[] }]

All integers are little-endian.  Names are truncated/NUL-padded to 16 bytes.
"""

import struct
import sys


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} OUTPUT NAME=FILE [NAME=FILE ...]",
              file=sys.stderr)
        sys.exit(1)

    output = sys.argv[1]
    entries = []
    for arg in sys.argv[2:]:
        if '=' not in arg:
            print(f"Error: expected NAME=FILE, got: {arg}", file=sys.stderr)
            sys.exit(1)
        name, path = arg.split('=', 1)
        if len(name) > 15:
            print(f"Error: name '{name}' exceeds 15 characters", file=sys.stderr)
            sys.exit(1)
        with open(path, 'rb') as f:
            data = f.read()
        entries.append((name, data))

    with open(output, 'wb') as f:
        f.write(struct.pack('<I', len(entries)))
        for name, data in entries:
            name_bytes = name.encode('ascii')[:15].ljust(16, b'\x00')
            f.write(name_bytes)
            f.write(struct.pack('<I', len(data)))
            f.write(data)

    total = 4 + sum(16 + 4 + len(d) for _, d in entries)
    print(f"initrd: {len(entries)} file(s), {total} bytes -> {output}")


if __name__ == '__main__':
    main()
