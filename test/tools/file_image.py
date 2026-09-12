#!/usr/bin/env python3
"""Create the file-service fixture using the production FAT formatter, then fragment DATA.BIN."""
import pathlib
import runpy
import struct
import sys

formatter, output, source = sys.argv[1:]
fat = runpy.run_path(formatter)
fat['build'](output, [f'DATA.BIN={source}'])
sectors = fat['SECTOR']
last = 3 + (pathlib.Path(source).stat().st_size + sectors - 1) // sectors - 1
swap = {4: last, last: 4}
with pathlib.Path(output).open('r+b') as image:
    image.seek(fat['RESERVED'] * sectors)
    original = image.read(fat['FAT_SECTORS'] * sectors)
    changed = bytearray(original)
    for cluster in range(2, last + 1):
        following = struct.unpack_from('<I', original, cluster * 4)[0]
        struct.pack_into('<I', changed, swap.get(cluster, cluster) * 4, swap.get(following, following))
    for copy in range(2):
        image.seek((fat['RESERVED'] + copy * fat['FAT_SECTORS']) * sectors)
        image.write(changed)
    first_offset = (fat['DATA'] + 4 - 2) * sectors
    last_offset = (fat['DATA'] + last - 2) * sectors
    image.seek(first_offset); first_data = image.read(sectors)
    image.seek(last_offset); last_data = image.read(sectors)
    image.seek(first_offset); image.write(last_data)
    image.seek(last_offset); image.write(first_data)
