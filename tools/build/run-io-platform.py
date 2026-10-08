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
    fail_sector = None
    second_disk = False
    while extra_markers and extra_markers[0] in ("--bundle", "--disk", "--timeout", "--fail-sector", "--second-disk"):
        option = extra_markers.pop(0)
        if option == "--second-disk":
            second_disk = True
            continue
        value = extra_markers.pop(0)
        if option == "--bundle":
            bundle = value
        elif option == "--disk":
            disk_image = value
        elif option == "--fail-sector":
            fail_sector = int(value)
        else:
            timeout = float(value)
    root = pathlib.Path(__file__).resolve().parents[2]
    temporary = root / ".tmp/project/interactive-storage/platform"
    temporary.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=temporary) as directory:
        disk = pathlib.Path(disk_image) if disk_image else pathlib.Path(directory) / "probe.img"
        if disk_image is None:
            with disk.open("wb") as stream:
                stream.truncate(1024 * 1024)
        disk_source = str(disk)
        if fail_sector is not None:
            # QEMU blkdebug injects a real backend error, without guest hooks.
            config = pathlib.Path(directory) / "blkdebug.conf"
            config.write_text(f'[inject-error]\nevent = "read_aio"\nerrno = "5"\nsector = "{fail_sector}"\n')
            disk_source = f"blkdebug:{config}:{disk}"
        if second_disk:
            other = pathlib.Path(directory) / "probe2.img"
            with other.open("wb") as stream:
                stream.truncate(1024 * 1024)
        for count in cpus.split(","):
            command = [qemu, "-machine", "virt,iommu-sys=on", "-nographic",
                       "-bios", "default", "-kernel", kernel, "-smp", count,
                       "-drive", f"if=none,id=disk,format=raw,readonly=on,file={disk_source}",
                       "-device", "virtio-blk-pci,addr=1,drive=disk,disable-legacy=on,iommu_platform=on"]
            if second_disk:
                command += ["-drive", f"if=none,id=disk2,format=raw,readonly=on,file={other}",
                            "-device", "virtio-blk-pci,addr=2,drive=disk2,disable-legacy=on,iommu_platform=on"]
            if bundle is not None:
                command += ["-initrd", bundle]
            try:
                result = subprocess.run(command, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, timeout=timeout)
                output = result.stdout
            except subprocess.TimeoutExpired as error:
                output = error.stdout or b""
            text = output.decode(errors="replace")
            expected = [f"cpu: online={count}",
                        "runtime: entered", *extra_markers]
            if any(marker not in text for marker in expected) or "MYOS KERNEL PANIC" in text:
                log = temporary / f"failed-{count}.log"
                log.write_text(text)
                print(text)
                raise SystemExit(f"[io-platform] failed; diagnostic: {log}")
            print(f"[io-platform] OK: {count} harts, runtime and requested device paths")

if __name__ == "__main__":
    main()
