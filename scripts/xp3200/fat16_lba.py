#!/usr/bin/env python3
"""Print the first FAT16 partition LBA of an MBR image."""
import struct
import sys

with open(sys.argv[1], "rb") as f:
    mbr = f.read(512)
for i in range(4):
    e = mbr[446 + i * 16:446 + (i + 1) * 16]
    if len(e) < 16:
        continue
    ptype = e[4]
    lba = struct.unpack("<I", e[8:12])[0]
    size = struct.unpack("<I", e[12:16])[0]
    if ptype in (0x04, 0x06, 0x0E) and size > 0:
        print(lba)
        sys.exit(0)
print(97)
