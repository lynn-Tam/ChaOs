#!/usr/bin/env python3
"""Measure real shell commands on identical copies of a formatted data disk."""

import argparse
import os
from pathlib import Path
import selectors
import shutil
import statistics
import subprocess
import time


def run(args, disk, trace=None):
    command = [args.qemu, "-machine", "virt,iommu-sys=on", "-smp", str(args.smp),
               "-nographic", "-bios", "default", "-kernel", args.kernel,
               "-initrd", args.bundle,
               "-drive", f"if=none,id=boot,format=raw,readonly=on,file={args.boot}",
               "-device", "virtio-blk-pci,addr=1,drive=boot,disable-legacy=on,iommu_platform=on",
               "-drive", f"if=none,id=data,format=raw,file={disk}",
               "-device", "virtio-blk-pci,addr=2,drive=data,disable-legacy=on,iommu_platform=on"]
    if trace is not None:
        command += ["-trace", f"enable=virtio_blk_*,file={trace}"]
    process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT)
    poll = selectors.DefaultSelector()
    poll.register(process.stdout, selectors.EVENT_READ)
    output = bytearray()

    def until(marker, start):
        deadline = time.monotonic() + args.timeout
        while marker not in output[start:]:
            if process.poll() is not None or time.monotonic() >= deadline:
                raise RuntimeError(f"missing {marker!r}; serial tail: {output[-1000:]!r}")
            for event, _ in poll.select(0.1):
                chunk = os.read(event.fd, 65536)
                if not chunk:
                    raise RuntimeError("QEMU serial closed")
                output.extend(chunk)
                if b"MYOS KERNEL PANIC" in output or b"user: contained fault" in output:
                    raise RuntimeError(f"guest fault; serial tail: {output[-1000:]!r}")

    times = {}
    try:
        until(b"myos> ", 0)
        for label, command, expected in (
            ("hello_cold", "hello", b"Hello from userspace."),
            ("hello_hot", "hello", b"Hello from userspace."),
            ("cat_cold", "cat /boot/README.TXT", b"myos disk file service"),
            ("cat_hot", "cat /boot/README.TXT", b"myos disk file service"),
            ("ls_cold", "ls", b"boot"),
            ("ls_hot", "ls", b"boot"),
            ("write", "write phase0 hello", b"myos> "),
            ("cat_written", "cat phase0", b"hello"),
        ):
            start = len(output)
            for byte in command.encode() + b"\r":
                process.stdin.write(bytes([byte]))
                process.stdin.flush()
                time.sleep(0.003)
            began = time.monotonic()
            until(b"myos> ", start)
            duration = time.monotonic() - began
            transcript = output[start:]
            if expected not in transcript or b"error: " in transcript:
                raise RuntimeError(f"{command}: unexpected output {transcript!r}; "
                                   f"serial tail {output[-4000:]!r}")
            times[label] = duration
        return times
    finally:
        process.kill()
        process.wait()
        poll.close()


def percentile(values, percent):
    ordered = sorted(values)
    return ordered[max(0, (len(ordered) * percent + 99) // 100 - 1)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kernel")
    parser.add_argument("bundle")
    parser.add_argument("boot")
    parser.add_argument("data", help="formatted data disk; never modified")
    parser.add_argument("--qemu", default="qemu-system-riscv64")
    parser.add_argument("--smp", type=int, default=4)
    parser.add_argument("--samples", type=int, default=5)
    parser.add_argument("--timeout", type=float, default=45)
    parser.add_argument("--device-trace", action="store_true",
                        help="retain QEMU virtio-blk events under .tmp/project/console-measure")
    args = parser.parse_args()
    if args.samples < 1 or args.smp < 1:
        parser.error("samples and smp must be positive")
    root = Path(__file__).resolve().parents[2]
    directory = root / ".tmp/project/console-measure"
    directory.mkdir(parents=True, exist_ok=True)
    disk = directory / "data.img"
    values = {}
    try:
        for index in range(args.samples):
            shutil.copyfile(args.data, disk)
            trace = directory / f"virtio-{index}.trace" if args.device_trace else None
            sample = run(args, disk, trace)
            for label, seconds in sample.items():
                values.setdefault(label, []).append(seconds)
    finally:
        disk.unlink(missing_ok=True)
    for label, samples in values.items():
        print(f"{label:12} p50={statistics.median(samples):.3f}s "
              f"p95={percentile(samples, 95):.3f}s "
              f"samples={','.join(f'{value:.3f}' for value in samples)}")


if __name__ == "__main__":
    main()
