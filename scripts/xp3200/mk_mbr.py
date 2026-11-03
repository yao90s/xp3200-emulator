import struct
import sys

path = sys.argv[1]
start_lba = int(sys.argv[2])
size = None
with open(path, "r+b") as f:
    f.seek(0, 2)
    total = f.tell() // 512
    count = total - start_lba
    # CHS values matching the real 128M card
    entry = struct.pack("<BBBBBBBBII",
                        0x00, 0x03, 0x00, 0x00,
                        0x06, 0x07, 0xE0, 0xC0,
                        start_lba, count)
    f.seek(446)
    f.write(entry)
    f.seek(510)
    f.write(b"\x55\xaa")
print("mbr: start=%d count=%d total=%d" % (start_lba, count, total))
