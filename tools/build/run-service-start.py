#!/usr/bin/env python3
"""Exercise a real late service-admission failure and repeat after rollback."""

from pathlib import Path
import subprocess
import sys

def main():
    qemu, kernel, bundle, disk, cpus = sys.argv[1:6]
    logs = Path(__file__).resolve().parents[2] / '.tmp/project/service-start'
    logs.mkdir(parents=True, exist_ok=True)
    for count in cpus.split(','):
        command = [qemu, '-machine', 'virt,iommu-sys=on', '-smp', count,
                   '-nographic', '-bios', 'default', '-kernel', kernel,
                   '-initrd', bundle,
                   '-drive', f'if=none,id=disk,format=raw,readonly=on,file={disk}',
                   '-device', 'virtio-blk-pci,addr=1,drive=disk,disable-legacy=on,iommu_platform=on']
        try:
            result = subprocess.run(command, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, timeout=30)
            output = result.stdout
        except subprocess.TimeoutExpired as error:
            output = error.stdout or b''
        if (output.count(b'uart: console ready') != 2
                or output.count(b'[service-start] first rollback complete') != 1
                or output.count(b'[service-start] second rollback complete') != 1
                or b'MYOS KERNEL PANIC' in output
                or b'failed=0' not in output):
            log = logs / f'failed-{count}.log'
            log.write_bytes(output)
            raise RuntimeError(f'{count} hart startup rollback failed; diagnostic: {log}')
        print(f'[service-start] OK: {count} harts, late admission rollback and relaunch')

if __name__ == '__main__':
    main()
