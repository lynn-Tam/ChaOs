#!/usr/bin/env python3
"""Exercise the production shell and a real writable virtio disk across QEMU death."""

import json
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

    def command(text, interactions=()):
        start = len(output)
        for char in text.encode() + b"\r":
            process.stdin.write(bytes([char])); process.stdin.flush(); time.sleep(0.003)
        cursor = start
        for marker, keys in interactions:
            until(marker, cursor); cursor = len(output)
            process.stdin.write(keys); process.stdin.flush()
        until(b"/ > ", start)
        if text == "svc restart devices": until(b"uart: console ready\n", start)
        return output[start:]

    def publication():
        for line in command('bus call . Get name=store.admin').splitlines():
            if line.startswith(b'{'): return json.loads(line)['instance']
        return None

    def await_services(expected):
        deadline = time.monotonic() + 40
        while True:
            state = command('svc')
            if all(f"{name} {value}".encode() in state for name, value in expected.items()): return
            if time.monotonic() >= deadline: raise RuntimeError(f"services did not reach {expected!r}; {state!r}")

    try:
        until(b"/ > ", 0)
        # A responsive shell no longer implies optional storage is ready.
        command("bus wait store.admin")
        command("bus wait files")
        await_services({"store": "running", "vfs": "running"})
        for item in commands:
            text, expected = item[:2]
            states = item[2] if len(item) > 2 else None
            if states and not isinstance(states, dict): states = dict.fromkeys(states, "running")
            replacing = text == 'svc restart store'
            previous = publication() if replacing else None
            began = time.monotonic()
            result = command(text, item[3] if len(item) > 3 else ())
            if text == "fdcheck cache":
                print(f"[storage] cache/sync: {cpus} hart(s), {time.monotonic()-began:.3f}s", flush=True)
            if expected not in result:
                raise RuntimeError(f"{text}: missing {expected!r}; got {result!r}")
            if replacing:
                deadline = time.monotonic() + 40
                while (current := publication()) is None or current == previous:
                    if time.monotonic() >= deadline: raise RuntimeError('store did not publish its replacement')
            if states: await_services(states)

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
                ("mkfs", b"/ > "),
                ("fdcheck", b"fd offsets ok"),
                ("fdcheck cache", b"fd cache ok"),
                ("fdcheck close", b"close accepted"),
                ("cat fd-async", b"CR"),
                ("fs sync", b"/ > "),
                ("fs write note hello-persistent", b"/ > "),
                ("fs append note -again", b"/ > "),
                ("cat note", b"hello-persistent-again\n/ > "),

                ("cat note", b"hello-persistent-again"),
                ("svc restart devices", b"service change requested", ["devices", "block-8", "store", "vfs"]),
                ("cat note", b"hello-persistent-again"),
                ("cat note", b"hello-persistent-again\n/ > "),
                ("ls", b"note\n"),
                ("ls /boot", b"cat.elf"),
                ("cat /boot/README.TXT", b"myos disk file service"),
                ("write /boot/denied no", b"error: "),
                ("cp /boot/README.TXT readme", b"/ > "),
                ("cat readme", b"myos disk file service"),
                ("cp /boot/cat.elf cat.elf", b"/ > "),
                ("fs stat cat.elf", package_size),
                ("/cat.elf /boot/README.TXT", b"myos disk file service"),
                ("echo stream-persisted > piped", b"/ > "),
                ("cat piped", b"stream-persisted"),
                ("cat piped > copied", b"/ > "),
                ("cat copied", b"stream-persisted"),
                ("fs mkdir notes", b"/ > "),
                ("fs write notes/item nested-persistent", b"/ > "),
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
                ("svc stop store", b"service change requested", {"store": "idle", "vfs": "running"}),
                ("cat /boot/README.TXT", b"myos disk file service"),
                ("echo storage-offline", b"storage-offline"),
                ("cat note", b"hello-persistent-again"),
                ("svc start store", b"service change requested", ["store", "vfs"]),
                ("cat note", b"hello-persistent-again"),
                ("svc restart store", b"service change requested", ["store", "vfs"]),
                ("cat readme", b"myos disk file service"),
                ("fs mv note renamed", b"/ > "),
                ("fs rm renamed", b"/ > "),
                ("ls", b"readme\n"),
            ]
            if count == "1":
                resumed.extend([
                    ("mkfs", b"/ > "),
                    ("fs write fresh reinitialized", b"/ > "),
                    ("cat fresh", b"reinitialized"),
                ])
            boot(qemu, kernel, bundle, data_disk, count, resumed)
            if count == "1":
                boot(qemu, kernel, bundle, data_disk, count, [
                    ("cat fresh", b"reinitialized"),
                    ("cp /boot/README.TXT readme", b"/ > "),
                ])
            print(f"[storage] OK: {count} hart(s), shell writes survive forced QEMU death")
        config = Path(directory) / "write-error.conf"
        config.write_text('[inject-error]\nevent = "write_aio"\nerrno = "5"\nonce = "on"\n')
        source = f"blkdebug:{config}:{data_disk}"
        boot(qemu, kernel, bundle, data_disk, "1", [
            ("fs write readme uncertain", b"error: ", ["store", "vfs"]),
            ("cat readme", b"myos disk file service"),
            ("fs write readme recovered", b"/ > "),
        ], source)
        boot(qemu, kernel, bundle, data_disk, "1", [
            ("cat readme", b"recovered"),
        ])
        print("[storage] OK: injected write EIO isolated, old file survived, later sync persisted")

if __name__ == "__main__":
    main()
