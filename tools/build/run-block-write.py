#!/usr/bin/env python3
"""Check real virtio writes and flush across an ungraceful QEMU restart."""

import os
from pathlib import Path
import selectors
import subprocess
import sys
import tempfile
import time

def boot(qemu, kernel, bundle, disk, cpus, log):
    command = [qemu, '-machine', 'virt,iommu-sys=on', '-smp', cpus,
               '-nographic', '-bios', 'default', '-kernel', kernel,
               '-initrd', bundle,
               '-drive', f'if=none,id=disk,format=raw,cache=writeback,file={disk}',
               '-device', 'virtio-blk-pci,addr=1,drive=disk,disable-legacy=on,iommu_platform=on']
    process = subprocess.Popen(command, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT)
    poll = selectors.DefaultSelector()
    poll.register(process.stdout, selectors.EVENT_READ)
    output = bytearray()
    marker = b'[io-session] ok: isolated pages, 32 outstanding, batched completion, cancel, close'
    try:
        end = time.monotonic() + 25
        while marker not in output:
            if time.monotonic() >= end or process.poll() is not None:
                raise RuntimeError('guest did not complete block request')
            for key, _ in poll.select(0.1):
                output.extend(os.read(key.fd, 65536))
                if b'MYOS KERNEL PANIC' in output or b'[io-session] client failed' in output:
                    raise RuntimeError('guest block operation failed')
        if b'failed=0' not in output or b'io: isolated PCI function ready requester=0x8' not in output:
            raise RuntimeError('kernel or isolated device did not initialize')
    except Exception:
        log.write_bytes(output)
        raise
    finally:
        process.kill()
        process.communicate()
        poll.close()

def main():
    qemu, kernel, writer, verifier, cpus = sys.argv[1:6]
    logs = Path(__file__).resolve().parents[2] / '.tmp/project/block-write'
    logs.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=logs) as temporary:
        disk = Path(temporary) / 'volume.img'
        with disk.open('wb') as stream:
            stream.truncate(1024 * 1024)
        for count in cpus.split(','):
            boot(qemu, kernel, writer, disk, count, logs / f'write-{count}.log')
            boot(qemu, kernel, verifier, disk, count, logs / f'verify-{count}.log')
            print(f'[block-write] OK: {count} harts, write/flush, QEMU kill, reread')

if __name__ == '__main__':
    main()
