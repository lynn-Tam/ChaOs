#!/usr/bin/env python3
"""Observe a real non-OK service exit through the production service graph."""

import os
from pathlib import Path
import selectors
import subprocess
import sys
import time


def main():
    qemu, kernel, bundle, disk, harts = sys.argv[1:6]
    marker = b'[fault-shell] file read before failure'
    for count in harts.split(','):
        command = [qemu, '-machine', 'virt,iommu-sys=on', '-smp', count,
                   '-nographic', '-bios', 'default', '-kernel', kernel,
                   '-initrd', bundle,
                   '-drive', f'if=none,id=disk,format=raw,readonly=on,file={disk}',
                   '-device', 'virtio-blk-pci,addr=1,drive=disk,disable-legacy=on,iommu_platform=on']
        process = subprocess.Popen(command, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT)
        output = bytearray()
        selector = selectors.DefaultSelector()
        selector.register(process.stdout, selectors.EVENT_READ)
        try:
            deadline = time.monotonic() + 35
            while (output.count(marker) < 3
                   or output.count(b'user: contained fault') < 3
                   or output.count(b'address=0xe100') < 3):
                if process.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError('three service generations did not read the disk')
                for key, _ in selector.select(0.1):
                    data = os.read(key.fd, 65536)
                    if not data:
                        raise RuntimeError('QEMU serial closed')
                    output.extend(data)
                    if b'MYOS KERNEL PANIC' in output:
                        raise RuntimeError('kernel panic')
            if output.count(b'uart: console ready') != 1:
                raise RuntimeError('unrelated UART service restarted')
            if b'io: isolated PCI function ready requester=0x8' not in output:
                raise RuntimeError('missing isolated block device')
            print(f'[service-fault] OK: {count} harts, three contained shell faults and file reads, UART survives')
        except Exception:
            directory = Path(__file__).resolve().parents[2] / '.tmp/project/service-fault'
            directory.mkdir(parents=True, exist_ok=True)
            path = directory / f'failed-{count}.log'
            path.write_bytes(output)
            raise RuntimeError(f'service-fault diagnostic: {path}')
        finally:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            selector.close()


if __name__ == '__main__':
    main()
