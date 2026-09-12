#!/usr/bin/env python3
"""Verify optional PCI/IOMMU bootstrap with the canonical kernel artifact."""

import pathlib
import subprocess
import sys
import tempfile


def main():
    qemu, kernel, cpus = sys.argv[1:4]
    extra_markers = sys.argv[4:]
    bundle = None
    disk_image = None
    timeout = 5
    while extra_markers[:1] in (["--bundle"], ["--disk"], ["--timeout"]):
        if extra_markers[0] == "--bundle":
            bundle = extra_markers[1]
        elif extra_markers[0] == "--disk":
            disk_image = extra_markers[1]
        else:
            timeout = float(extra_markers[1])
        extra_markers = extra_markers[2:]
    root = pathlib.Path(__file__).resolve().parents[2]
    temporary = root / ".tmp/project/interactive-storage/platform"
    temporary.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=temporary) as directory:
        disk = pathlib.Path(disk_image) if disk_image else pathlib.Path(directory) / "probe.img"
        if disk_image is None:
            with disk.open("wb") as stream:
                stream.truncate(1024 * 1024)
        for count in cpus.split(","):
            command = [qemu, "-machine", "virt,iommu-sys=on", "-nographic",
                       "-bios", "default", "-kernel", kernel, "-smp", count,
                       "-drive", f"if=none,id=disk,format=raw,readonly=on,file={disk}",
                       "-device", "virtio-blk-pci,addr=1,drive=disk,disable-legacy=on,iommu_platform=on"]
            if bundle is not None:
                command += ["-initrd", bundle]
            try:
                result = subprocess.run(command, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, timeout=timeout)
                output = result.stdout
            except subprocess.TimeoutExpired as error:
                output = error.stdout or b""
            text = output.decode(errors="replace")
            expected = ["io: isolated PCI function ready requester=0x8",
                        f"cpu: discovered={count} prepared=0 starting=0 online={count} failed=0",
                        "failed=0", "runtime: entered", *extra_markers]
            if any(marker not in text for marker in expected) or "MYOS KERNEL PANIC" in text:
                log = temporary / f"failed-{count}.log"
                log.write_text(text)
                print(text)
                raise SystemExit(f"[io-platform] failed; diagnostic: {log}")
            print(f"[io-platform] OK: {count} harts, PCI enumeration, default-deny IOMMU")


if __name__ == "__main__":
    main()
