#!/usr/bin/env python3
"""List the contents of a FAT16 image that has an MBR (XP3200 SD card).

Prints a JSON array of {path, size, dir} objects.
"""
import json
import struct
import sys


def read_at(f, off, n):
    f.seek(off)
    return f.read(n)


def main():
    path = sys.argv[1]
    with open(path, "rb") as f:
        mbr = read_at(f, 0, 512)
        start = 0
        for i in range(4):
            e = mbr[446 + i * 16:446 + (i + 1) * 16]
            if len(e) < 16:
                continue
            ptype = e[4]
            lba = struct.unpack("<I", e[8:12])[0]
            size = struct.unpack("<I", e[12:16])[0]
            if ptype in (0x04, 0x06, 0x0E) and size > 0:
                start = lba
                break
        base = start * 512
        bpb = read_at(f, base, 512)
        bps = struct.unpack("<H", bpb[11:13])[0]
        spc = bpb[13]
        reserved = struct.unpack("<H", bpb[14:16])[0]
        fats = bpb[16]
        root_entries = struct.unpack("<H", bpb[17:19])[0]
        spf = struct.unpack("<H", bpb[22:24])[0]
        if bps == 0 or spc == 0:
            print("[]")
            return
        fat_off = base + reserved * bps
        root_off = base + (reserved + fats * spf) * bps
        data_off = root_off + root_entries * 32
        cluster_bytes = spc * bps

        fat = read_at(f, fat_off, spf * bps)

        def next_cluster(c):
            if c * 2 + 2 > len(fat):
                return 0xFFFF
            return struct.unpack("<H", fat[c * 2:c * 2 + 2])[0]

        def cluster_off(c):
            return data_off + (c - 2) * cluster_bytes

        def read_chain(c):
            out = b""
            seen = set()
            while 2 <= c < 0xFFF8 and c not in seen:
                seen.add(c)
                out += read_at(f, cluster_off(c), cluster_bytes)
                c = next_cluster(c)
            return out

        entries = []

        def parse_dir(data, prefix):
            lfn = ""
            lfn_parts = []
            for off in range(0, len(data), 32):
                e = data[off:off + 32]
                if len(e) < 32:
                    break
                if e[0] == 0x00:
                    break
                if e[0] == 0xE5:
                    continue
                attr = e[11]
                if attr == 0x0F:
                    raw = bytearray()
                    for pos in (1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30):
                        raw += e[pos:pos + 2]
                    lfn_parts.append(bytes(raw))
                    continue
                if attr & 0x08:
                    continue
                name = ""
                if lfn_parts:
                    joined = b"".join(reversed(lfn_parts))
                    try:
                        name = joined.decode("utf-16-le", "ignore")
                    except Exception:
                        name = ""
                    name = name.split("\x00")[0]
                    lfn_parts = []
                if not name:
                    base = e[0:8].decode("latin-1").rstrip()
                    ext = e[8:11].decode("latin-1").rstrip()
                    name = base + ("." + ext if ext else "")
                if name in (".", ".."):
                    continue
                cluster = struct.unpack("<H", e[26:28])[0]
                size = struct.unpack("<I", e[28:32])[0]
                full = prefix + name
                is_dir = bool(attr & 0x10)
                entries.append({"path": full, "size": size, "dir": is_dir})
                if is_dir and cluster >= 2:
                    sub = read_chain(cluster)
                    parse_dir(sub, full + "/")

        root = read_at(f, root_off, root_entries * 32)
        parse_dir(root, "")
        print(json.dumps(entries, ensure_ascii=False))


if __name__ == "__main__":
    main()
