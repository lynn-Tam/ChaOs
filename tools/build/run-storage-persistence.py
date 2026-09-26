#!/usr/bin/env python3
"""Exercise the production shell and a real writable virtio disk across QEMU death."""

import os
from pathlib import Path
import selectors
import subprocess
import sys
import tempfile
import time


def boot(qemu, kernel, bundle, boot_disk, data_disk, cpus, commands,
         data_source=None):
    data_source = data_source or data_disk
    args = [qemu, "-machine", "virt,iommu-sys=on", "-smp", cpus,
            "-nographic", "-bios", "default", "-kernel", kernel,
            "-initrd", bundle,
            "-drive", f"if=none,id=boot,format=raw,readonly=on,file={boot_disk}",
            "-device", "virtio-blk-pci,addr=1,drive=boot,disable-legacy=on,iommu_platform=on",
            "-drive", f"if=none,id=data,format=raw,file={data_source}",
            "-device", "virtio-blk-pci,addr=2,drive=data,disable-legacy=on,iommu_platform=on"]
    process = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT)
    poll = selectors.DefaultSelector()
    poll.register(process.stdout, selectors.EVENT_READ)
    output = bytearray()

    def until(marker, start):
        deadline = time.monotonic() + 40
        while marker not in output[start:]:
            if process.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError(f"missing {marker!r}; tail={output[-2000:]!r}")
            for event, _ in poll.select(0.1):
                chunk = os.read(event.fd, 65536)
                if not chunk:
                    raise RuntimeError("QEMU serial closed")
                output.extend(chunk)
                fault = output.find(b"user: contained fault")
                if b"MYOS KERNEL PANIC" in output or (fault >= 0 and b"\n" in output[fault:]):
                    raise RuntimeError(f"guest fault; tail={output[-2000:]!r}")

    try:
        until(b"myos> ", 0)
        for item in commands:
            command, expected = item[:2]
            restart = len(item) > 2 and item[2]
            begin = len(output)
            for char in command.encode() + b"\r":
                process.stdin.write(bytes([char]))
                process.stdin.flush()
                time.sleep(0.003)
            if restart or command == "restart store":
                until(b"myos native shell", begin)
                reopened = output.index(b"myos native shell", begin)
                until(b"myos> ", reopened)
            else:
                until(b"myos> ", begin)
            result = output[begin:]
            if expected not in result:
                raise RuntimeError(f"{command}: missing {expected!r}; got {result!r}")
    except Exception:
        diagnostic = Path(__file__).resolve().parents[2] / ".tmp/project/interactive-storage/failed-persistence.log"
        diagnostic.parent.mkdir(parents=True, exist_ok=True)
        diagnostic.write_bytes(output)
        raise
    finally:
        process.kill()  # Simulate power loss; no guest shutdown or unmount.
        process.wait()
        poll.close()


def main():
    qemu, kernel, bundle, boot_disk, cpus = sys.argv[1:6]
    package_size = f"{(Path(boot_disk).parent / 'hello.pkg').stat().st_size} bytes".encode()
    root = Path(__file__).resolve().parents[2] / ".tmp/project/interactive-storage"
    root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=root) as directory:
        data_disk = Path(directory) / "data.img"
        with data_disk.open("wb") as stream:
            stream.truncate(16 * 1024 * 1024)
        for count in cpus.split(","):
            # Every CPU configuration starts from an independent unformatted disk.
            with data_disk.open("r+b") as stream:
                stream.truncate(0)
                stream.truncate(16 * 1024 * 1024)
            boot(qemu, kernel, bundle, boot_disk, data_disk, count, [
                ("mkfs", b"ok\n"),
                ("write note hello-persistent", b"ok\n"),
                ("append note -again", b"ok\n"),
                ("wcat note", b"hello-persistent-again"),
                ("wls", b"note\n"),
                ("save README.TXT readme", b"ok\n"),
                ("wcat readme", b"myos disk file service"),
                ("save HELLO.PKG hello.pkg", b"ok\n"),
                ("wstat hello.pkg", package_size),
                ("run echo stream-persisted | put piped", b"exit: 0"),
                ("wcat piped", b"stream-persisted"),
                ("run get piped | put copied", b"exit: 0"),
                ("wcat copied", b"stream-persisted"),
                ("mkdir notes", b"ok\n"),
                ("write notes/item nested-persistent", b"ok\n"),
                ("wls notes", b"item\n"),
            ])
            resumed = [
                ("wcat note", b"hello-persistent-again"),
                ("wcat readme", b"myos disk file service"),
                ("wstat hello.pkg", package_size),
                ("wcat piped", b"stream-persisted"),
                ("wcat copied", b"stream-persisted"),
                ("wcat notes/item", b"nested-persistent"),
                ("wls notes", b"item\n"),
                ("restart store", b"myos native shell"),
                ("wcat readme", b"myos disk file service"),
                ("mv note renamed", b"ok\n"),
                ("rm renamed", b"ok\n"),
                ("wls", b"readme\n"),
            ]
            if count == "1":
                resumed.extend([
                    ("mkfs", b"ok\n"),
                    ("write fresh reinitialized", b"ok\n"),
                    ("wcat fresh", b"reinitialized"),
                ])
            boot(qemu, kernel, bundle, boot_disk, data_disk, count, resumed)
            if count == "1":
                boot(qemu, kernel, bundle, boot_disk, data_disk, count, [
                    ("wcat fresh", b"reinitialized"),
                    ("save README.TXT readme", b"ok\n"),
                ])
            print(f"[storage] OK: {count} hart(s), shell writes survive forced QEMU death")
        config = Path(directory) / "write-error.conf"
        config.write_text('[inject-error]\nevent = "write_aio"\nerrno = "5"\nonce = "on"\n')
        source = f"blkdebug:{config}:{data_disk}"
        boot(qemu, kernel, bundle, boot_disk, data_disk, "1", [
            ("write readme uncertain", b"myos native shell", True),
            ("wcat readme", b"myos disk file service"),
            ("write readme recovered", b"ok\n"),
        ], source)
        boot(qemu, kernel, bundle, boot_disk, data_disk, "1", [
            ("wcat readme", b"recovered"),
        ])
        print("[storage] OK: injected write EIO isolated, old file survived, later sync persisted")


if __name__ == "__main__":
    main()
