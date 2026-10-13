#!/usr/bin/env python3
"""Boot the production shell with a persistent, explicitly formatted data disk."""

import os
from pathlib import Path
import sys

def main():
    qemu, kernel, bundle, data, cpus = sys.argv[1:6]
    data = Path(data)
    if not data.exists():
        with data.open("xb") as stream:
            stream.truncate(16 * 1024 * 1024)
    command = [qemu, "-machine", "virt,iommu-sys=on", "-smp", cpus,
               "-nographic", "-bios", "default", "-kernel", kernel,
               "-initrd", bundle,
               "-drive", f"if=none,id=data,format=raw,file={data}",
               "-device", "virtio-blk-pci,addr=1,drive=data,disable-legacy=on,iommu_platform=on"]
    os.execvp(qemu, command)

if __name__ == "__main__":
    main()
