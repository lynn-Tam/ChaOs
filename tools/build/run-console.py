#!/usr/bin/env python3
"""Exercise the production console through ordinary serial bytes."""
import os
from pathlib import Path
import re
import selectors
import subprocess
import sys
import time


def exercise(qemu, kernel, bundle, smp, exhaustion, iterations, disk, pressure=False):
    root = Path(__file__).resolve().parents[2]
    logs = root / '.tmp/project/interactive-storage'
    logs.mkdir(parents=True, exist_ok=True)
    output = bytearray()
    command = [qemu, '-machine', 'virt', '-smp', smp, '-nographic',
               '-bios', 'default', '-kernel', kernel, '-initrd', bundle]
    if disk is not None:
        command[2] = 'virt,iommu-sys=on'
        command.extend([
            '-drive', f'if=none,id=disk,format=raw,readonly=on,file={disk}',
            '-device', 'virtio-blk-pci,addr=1,drive=disk,disable-legacy=on,iommu_platform=on'])
    process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT)
    poll = selectors.DefaultSelector()
    poll.register(process.stdout, selectors.EVENT_READ)

    def until(marker, start=0):
        deadline = time.monotonic() + 30
        while marker not in output[start:]:
            if process.poll() is not None or time.monotonic() >= deadline:
                raise RuntimeError(f'missing {marker!r}')
            for key, _ in poll.select(0.1):
                chunk = os.read(key.fd, 65536)
                if not chunk:
                    raise RuntimeError('QEMU closed serial')
                output.extend(chunk)
                if b'MYOS KERNEL PANIC' in output or b'user: contained fault' in output:
                    # Preserve the diagnostic body before stopping QEMU.
                    drain_until = time.monotonic() + 5
                    while process.poll() is None and time.monotonic() < drain_until:
                        for pending, _ in poll.select(0.05):
                            output.extend(os.read(pending.fd, 65536))
                        if b'\n====================================================' in output:
                            break
                    raise RuntimeError('kernel panic or unexpected user fault')

    def run(text, expected):
        start = len(output)
        # Pace input to the UART FIFO while still exercising real IRQ delivery.
        for byte in text.encode() + b'\r':
            process.stdin.write(bytes([byte]))
            process.stdin.flush()
            time.sleep(0.003)
        until(b'myos> ', start)
        transcript = output[start:]
        if expected not in transcript:
            raise RuntimeError(f'{text}: expected {expected!r}, got {transcript!r}')
        return transcript

    try:
        until(b'myos> ')
        if disk is not None and b'io: isolated PCI function ready requester=0x8' not in output:
            raise RuntimeError('missing isolated Device bootstrap')
        if not re.search(rb'\[test\] summary\s+passed=[1-9][0-9]*\s+failed=0\s', output):
            raise RuntimeError('missing successful builtin test summary')
        run('help', b'run hello')
        run('ls', b'HELLO.PKG')
        run('cat README.TXT', b'myos disk file service')
        run('cat absent.txt', b'exit: -5')
        if exhaustion:
            for _ in range(iterations):
                run('run hello', b'exit: -7')
            run('help', b'run hello')
            run('wait', b'exit: -1')
            print(f'[console] OK: {smp} harts, repeated admission failure and rollback')
            return
        for _ in range(iterations):
            run('run hello', b'Hello from userspace.\nexit: 0')
            run('run echo named arguments survive paging', b'named arguments survive paging\nexit: 0')
        run('run demand', b'[demand] initialized data, BSS, private writes and VM reuse ok\nexit: 0')
        if pressure and (not re.search(rb'pressure drained held=[1-9][0-9]* free=0', output)
                         or b'pressure released held=' not in output):
            raise RuntimeError('missing actual PMM drain/release evidence')
        run('run bad', b'exit: -4')
        run('run absent', b'exit: -5')
        run('run uart', b'exit: -6')
        run('wait', b'exit: -1')
        run('run sleep 1', b'exit: 0')
        first = int(re.search(rb'task: ([0-9]+)', run('spawn sleep 10000', b'task: '))[1])
        second = int(re.search(rb'task: ([0-9]+)', run('spawn sleep 10000', b'task: '))[1])
        if first == second:
            raise RuntimeError('two live tasks share a handle')
        run(f'wait {first} 1', b'exit: -21')
        run('run echo unrelated task progresses', b'unrelated task progresses\nexit: 0')
        run(f'stop {second}', b'exit: -18')
        run(f'wait {second}', b'exit: -1')
        run(f'stop {first}', b'exit: -18')
        run(f'wait {first}', b'exit: -1')
        run('run echo ordered pipe bytes | cat', b'ordered pipe bytes\nexit: 0')
        run('run fill 8192 | slow 1', b'bytes: 8192\nexit: 0')
        early = run('run fill 100000 | slow 1 96', b'bytes: 96')
        if b'producer: -22' not in early and b'producer: -13' not in early:
            raise RuntimeError(f'writer did not observe consumer close: {early!r}')
        blocked = re.findall(rb'task: ([0-9]+)', run('spawn fill 100000 | slow 100', b'task: '))
        if len(blocked) != 2:
            raise RuntimeError('pipeline did not return independent handles')
        run('run echo running during backpressure', b'running during backpressure\nexit: 0')
        run(f'stop {int(blocked[1])}', b'exit: -18')
        producer = run(f'wait {int(blocked[0])}', b'exit: ')
        if b'exit: -22' not in producer and b'exit: -13' not in producer:
            raise RuntimeError(f'blocked producer was not released: {producer!r}')
        run('run hello', b'Hello from userspace.\nexit: 0')
        print(f'[console] OK: {smp} harts, applications, concurrent tasks, deadlines, bounded streams, EOF, stop and reuse')
    except Exception:
        sys.stdout.buffer.write(output)
        raise
    finally:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        poll.close()
        profile = 'console-exhaustion' if exhaustion else 'console'
        (logs / f'{profile}-{smp}.raw').write_bytes(output)


if __name__ == '__main__':
    qemu, kernel, bundle, disk, harts = sys.argv[1:6]
    exhaustion = len(sys.argv) >= 7 and sys.argv[6] == 'exhaustion'
    iterations = int(sys.argv[7]) if len(sys.argv) == 8 else 3
    for smp in harts.split(','):
        exercise(qemu, kernel, bundle, smp, exhaustion, iterations, disk,
                 pressure=len(sys.argv) >= 7 and sys.argv[6] == 'pressure')
