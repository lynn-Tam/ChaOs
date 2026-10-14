#!/usr/bin/env python3
"""Exercise the production shell and a real writable virtio disk across QEMU death."""

import os
from pathlib import Path
import selectors
import subprocess
import sys
import tempfile
import time

def boot(qemu, kernel, bundle, data_disk, cpus, commands,
         data_source=None):
    data_source = data_source or data_disk
    args = [qemu, "-machine", "virt,iommu-sys=on", "-smp", cpus,
            "-nographic", "-bios", "default", "-kernel", kernel,
            "-initrd", bundle,
            "-drive", f"if=none,id=data,format=raw,file={data_source}",
            "-device", "virtio-blk-pci,addr=1,drive=data,disable-legacy=on,iommu_platform=on"]
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

    def await_services(expected):
        deadline = time.monotonic() + 40
        while True:
            start = len(output)
            process.stdin.write(b"svc\r")
            process.stdin.flush()
            until(b"myos> ", start)
            state = output[start:]
            if all(f"{name} {value}".encode() in state for name, value in expected.items()):
                return
            if time.monotonic() >= deadline:
                raise RuntimeError(f"services did not reach {expected!r}; {state!r}")

    try:
        until(b"myos> ", 0)
        # A responsive shell no longer implies optional storage is ready.
        await_services({"store": "running", "vfs": "running"})
        for item in commands:
            command, expected = item[:2]
            begin = len(output)
            began = time.monotonic()
            for char in command.encode() + b"\r":
                process.stdin.write(bytes([char]))
                process.stdin.flush()
                time.sleep(0.003)
            if len(item) > 3:
                interaction = begin
                for marker, keys in item[3]:
                    until(marker, interaction)
                    interaction = len(output)
                    process.stdin.write(keys)
                    process.stdin.flush()
            until(b"myos> ", begin)
            result = output[begin:]
            if command == "run fdcheck cache":
                print(f"[storage] cache/sync: {cpus} hart(s), {time.monotonic()-began:.3f}s", flush=True)
            if expected not in result:
                raise RuntimeError(f"{command}: missing {expected!r}; got {result!r}")
            if len(item) > 2 and item[2]:
                # Management accepts intent asynchronously; observe real readiness.
                expected = item[2] if isinstance(item[2], dict) else dict.fromkeys(item[2], "running")
                await_services(expected)

    except Exception:
        # A fault header can arrive before its diagnostic body.
        drain = time.monotonic() + 1
        while process.poll() is None and time.monotonic() < drain:
            for event, _ in poll.select(0.05):
                chunk = os.read(event.fd, 65536)
                if chunk:
                    output.extend(chunk)
        diagnostic = Path(__file__).resolve().parents[2] / ".tmp/project/interactive-storage/failed-persistence.log"
        diagnostic.parent.mkdir(parents=True, exist_ok=True)
        diagnostic.write_bytes(output)
        raise
    finally:
        process.kill()  # Simulate power loss; no guest shutdown or unmount.
        process.wait()
        poll.close()

def main():
    qemu, kernel, bundle, cpus = sys.argv[1:5]
    package_size = f"{(Path(bundle).parent / 'cat.run.elf').stat().st_size} bytes".encode()
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
            boot(qemu, kernel, bundle, data_disk, count, [
                ("mkfs", b"myos> "),
                ("run fdcheck", b"fd offsets ok"),
                ("run fdcheck cache", b"fd cache ok"),
                ("run fdcheck close", b"close accepted"),
                ("cat fd-async", b"CR"),
                ("fs sync", b"myos> "),
                ("fs write note hello-persistent", b"myos> "),
                ("fs append note -again", b"myos> "),
                ("cat note", b"hello-persistent-again\nmyos> "),
                ("svc restart block-8", b"service change requested", ["block-8", "store", "vfs"]),
                ("cat note", b"hello-persistent-again"),
                ("svc restart devices", b"service change requested", ["devices", "block-8", "store", "vfs"]),
                ("cat note", b"hello-persistent-again"),
                ("cat note", b"hello-persistent-again\nmyos> "),
                ("ls", b"note\n"),
                ("ls /boot", b"cat.elf"),
                ("cat /boot/README.TXT", b"myos disk file service"),
                ("write /boot/denied no", b"error: "),
                ("cp /boot/README.TXT readme", b"myos> "),
                ("cat readme", b"myos disk file service"),
                ("cp /boot/cat.elf cat.elf", b"myos> "),
                ("fs stat cat.elf", package_size),
                ("run /cat.elf /boot/README.TXT", b"myos disk file service"),
                ("run echo stream-persisted > piped", b"myos> "),
                ("cat piped", b"stream-persisted"),
                ("run cat piped > copied", b"myos> "),
                ("cat copied", b"stream-persisted"),
                ("fs mkdir notes", b"myos> "),
                ("fs write notes/item nested-persistent", b"myos> "),
                ("ls notes", b"item\n"),
                ("edit edited", b"Written", False, [
                    (b"Ctrl+O Save", b"editor-persistent\x0f"),
                    (b"Written", b"\x18"),
                ]),
                ("cat edited", b"editor-persistent"),
            ])
            resumed = [
                ("cat note", b"hello-persistent-again"),
                ("cat note", b"hello-persistent-again"),
                ("cat readme", b"myos disk file service"),
                ("fs stat cat.elf", package_size),
                ("cat piped", b"stream-persisted"),
                ("cat copied", b"stream-persisted"),
                ("cat notes/item", b"nested-persistent"),
                ("ls notes", b"item\n"),
                ("cat edited", b"editor-persistent"),
                ("cat fd-async", b"CR"),
                ("svc stop store", b"service change requested", {"store": "stopped", "vfs": "running"}),
                ("cat /boot/README.TXT", b"myos disk file service"),
                ("run echo storage-offline", b"storage-offline"),
                ("cat note", b"error: 11"),
                ("svc start store", b"service change requested", ["store", "vfs"]),
                ("cat note", b"hello-persistent-again"),
                ("svc restart store", b"service change requested", ["store", "vfs"]),
                ("cat readme", b"myos disk file service"),
                ("fs mv note renamed", b"myos> "),
                ("fs rm renamed", b"myos> "),
                ("ls", b"readme\n"),
            ]
            if count == "1":
                resumed.extend([
                    ("mkfs", b"myos> "),
                    ("fs write fresh reinitialized", b"myos> "),
                    ("cat fresh", b"reinitialized"),
                ])
            boot(qemu, kernel, bundle, data_disk, count, resumed)
            if count == "1":
                boot(qemu, kernel, bundle, data_disk, count, [
                    ("cat fresh", b"reinitialized"),
                    ("cp /boot/README.TXT readme", b"myos> "),
                ])
            print(f"[storage] OK: {count} hart(s), shell writes survive forced QEMU death")
        config = Path(directory) / "write-error.conf"
        config.write_text('[inject-error]\nevent = "write_aio"\nerrno = "5"\nonce = "on"\n')
        source = f"blkdebug:{config}:{data_disk}"
        boot(qemu, kernel, bundle, data_disk, "1", [
            ("fs write readme uncertain", b"error: ", ["store", "vfs"]),
            ("cat readme", b"myos disk file service"),
            ("fs write readme recovered", b"myos> "),
        ], source)
        boot(qemu, kernel, bundle, data_disk, "1", [
            ("cat readme", b"recovered"),
        ])
        print("[storage] OK: injected write EIO isolated, old file survived, later sync persisted")

if __name__ == "__main__":
    main()
