"""Compare a packed load image with its ELF input, including all zero tails."""
import pathlib
import struct
import sys


def verify(elf_path, package_path):
    elf = pathlib.Path(elf_path).read_bytes()
    package = pathlib.Path(package_path).read_bytes()
    phoff = struct.unpack_from('<Q', elf, 32)[0]
    stride, count = struct.unpack_from('<HH', elf, 54)
    loads = []
    for i in range(count):
        kind, flags, offset, address, _, file_size, memory_size, _ = struct.unpack_from(
            '<IIQQQQQQ', elf, phoff + i * stride)
        if kind == 1 and memory_size:
            loads.append((address, flags, offset, file_size, memory_size))
    loads.sort()
    modules = struct.unpack_from('<Q', package, 40)[0]
    root = struct.unpack_from('<I', package, 52)[0]
    module = modules + root * 64
    image, image_size = struct.unpack_from('<QQ', package, module + 16)
    first, count = struct.unpack_from('<II', package, module + 40)
    segments = struct.unpack_from('<Q', package, 56)[0]
    assert count == len(loads) and image % 4096 == 0
    for i, (address, flags, offset, file_size, memory_size) in enumerate(loads):
        va, source, stored, mapped, alignment, access, reserved = struct.unpack_from(
            '<QQQQQII', package, segments + (first + i) * 48)
        writable = flags & 2
        rounded = (memory_size + 4095) & ~4095
        assert va == address and source % 4096 == 0 and alignment == 4096 and reserved == 0
        assert stored == (file_size if writable else rounded)
        assert mapped == (memory_size if writable else rounded)
        assert access == (bool(flags & 4) | (bool(flags & 2) << 1) | (bool(flags & 1) << 2))
        assert image <= source <= source + stored <= image + image_size
        assert package[source:source + file_size] == elf[offset:offset + file_size]
        assert not any(package[source + file_size:source + stored])
    print('[bootpack] load bytes, permissions, alignment and zero tails match ELF')


if __name__ == '__main__':
    verify(*sys.argv[1:])
