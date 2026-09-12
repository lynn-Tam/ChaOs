#!/usr/bin/env python3
"""Build a deterministic read-only FAT32 superfloppy from NAME=PATH files."""
import pathlib
import struct
import sys

SECTOR = 512
SECTORS = 131072
RESERVED = 32
FAT_SECTORS = 1024
DATA = RESERVED + 2 * FAT_SECTORS
CLUSTERS = SECTORS - DATA


def build(output, sources):
    if len(sources) > 128:
        raise ValueError("at most 128 root files")
    names = set()
    files = []
    for source in sources:
        name, path = source.split("=", 1)
        name = name.upper()
        pieces = name.split(".")
        if (len(pieces) > 2 or not 1 <= len(pieces[0]) <= 8
                or (len(pieces) == 2 and not 1 <= len(pieces[1]) <= 3)
                or any(c not in "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-" for p in pieces for c in p)
                or name in names):
            raise ValueError(f"invalid or duplicate short name: {name}")
        names.add(name)
        encoded = (pieces[0].ljust(8) + (pieces[1] if len(pieces) == 2 else "").ljust(3)).encode("ascii")
        files.append((encoded, pathlib.Path(path).read_bytes()))
    fat = bytearray(FAT_SECTORS * SECTOR)
    struct.pack_into("<II", fat, 0, 0x0ffffff8, 0x0fffffff)
    next_cluster = 2

    def allocate(size):
        nonlocal next_cluster
        count = (size + SECTOR - 1) // SECTOR
        if next_cluster + count > CLUSTERS + 2:
            raise ValueError("disk full")
        start = next_cluster if count else 0
        for cluster in range(next_cluster, next_cluster + count):
            following = cluster + 1 if cluster + 1 < next_cluster + count else 0x0fffffff
            struct.pack_into("<I", fat, cluster * 4, following)
        next_cluster += count
        return start

    directory = bytearray(((len(files) + 1) * 32 + SECTOR - 1) // SECTOR * SECTOR)
    root = allocate(len(directory))
    payloads = [(root, directory)]
    for i, (name, content) in enumerate(files):
        start = allocate(len(content))
        struct.pack_into("<11sB", directory, i * 32, name, 0x20)
        struct.pack_into("<H", directory, i * 32 + 20, start >> 16)
        struct.pack_into("<HI", directory, i * 32 + 26, start & 0xffff, len(content))
        if content:
            payloads.append((start, content))
    boot = bytearray(SECTOR)
    boot[:11] = b"\xeb\x58\x90MYOS    "
    struct.pack_into("<HBHBHHBHHHII", boot, 11, SECTOR, 1, RESERVED, 2, 0, 0, 0xf8, 0, 63, 255, 0, SECTORS)
    struct.pack_into("<IHHIHH", boot, 36, FAT_SECTORS, 0, 0, root, 1, 6)
    boot[64] = 0x80
    boot[66] = 0x29
    struct.pack_into("<I", boot, 67, 0x4d594f53)
    boot[71:90] = b"MYOS DISK  FAT32   "
    struct.pack_into("<H", boot, 510, 0xaa55)
    info = bytearray(SECTOR)
    struct.pack_into("<I", info, 0, 0x41615252)
    struct.pack_into("<III", info, 484, 0x61417272, CLUSTERS - (next_cluster - 2), next_cluster)
    struct.pack_into("<I", info, 508, 0xaa550000)
    with pathlib.Path(output).open("wb") as image:
        image.truncate(SECTORS * SECTOR)
        for sector, data in [(0, boot), (1, info), (6, boot), (7, info),
                             (RESERVED, fat), (RESERVED + FAT_SECTORS, fat)]:
            image.seek(sector * SECTOR)
            image.write(data)
        for cluster, data in payloads:
            image.seek((DATA + cluster - 2) * SECTOR)
            image.write(data)


if __name__ == "__main__":
    try:
        build(sys.argv[1], sys.argv[2:])
    except (ValueError, OSError, IndexError) as error:
        raise SystemExit(f"fat-image: {error}")
