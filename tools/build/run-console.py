#!/usr/bin/env python3
"""Exercise the production console through ordinary serial bytes."""
import os
from pathlib import Path
import re
import selectors
import subprocess
import sys
import time

def exercise(qemu, kernel, bundle, smp, iterations, service_fault=False, exec_check=False):
    root = Path(__file__).resolve().parents[2]
    logs = root / '.tmp/project/interactive-storage'
    logs.mkdir(parents=True, exist_ok=True)
    output = bytearray()
    command = [qemu, '-machine', 'virt', '-smp', smp, '-nographic',
               '-bios', 'default', '-kernel', kernel, '-initrd', bundle]
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
        run('help', b'run/spawn PROGRAM')
        run('svc', b'shell running')
        run('ls', b'cat.elf')
        run('cat README.TXT', b'myos disk file service')
        run('cat absent.txt', b'error: 2')
        if exec_check:
            # Reuse the same immutable file backing after the first process writes
            # every initialized page; a second process must see the original bytes.
            run('run cow', b'ELF data, COW and BSS ok\nmyos> ')
            run('run cow', b'ELF data, COW and BSS ok\nmyos> ')
        if service_fault:
            run('svc restart absent', b'error: -5')
            baseline = output.count(b'uart: console ready')
            for target in ('files',):
                start = len(output)
                for byte in f'svc restart {target}\r'.encode():
                    process.stdin.write(bytes([byte]))
                    process.stdin.flush()
                    time.sleep(0.003)
                until(b'myos native shell', start)
                restarted = output.index(b'myos native shell', start)
                until(b'myos> ', restarted)
                if output.count(b'uart: console ready') != baseline:
                    raise RuntimeError('unrelated UART service restarted')
                run('run echo console-ready', b'console-ready\nmyos> ')
                run('cat README.TXT', b'myos disk file service')
                run('svc restart absent', b'error: -5')
            print(f'[console] OK: {smp} harts, local service restart and surviving UART')
            return
        for _ in range(iterations):
            run('run echo console-ready', b'console-ready\nmyos> ')
            run('run echo named arguments survive paging', b'named arguments survive paging\nmyos> ')
        run('echo console-ready', b'console-ready\nmyos> ')
        run('run demand', b'[demand] initialized data, BSS, private writes and VM reuse ok\nmyos> ')
        first = int(re.search(rb'task: ([0-9]+)', run('spawn demand 1000 17', b'task: '))[1])
        second = int(re.search(rb'task: ([0-9]+)', run('spawn demand 1000 71', b'task: '))[1])
        if first == second:
            raise RuntimeError('concurrent ELF processes share a task handle')
        for token in (first, second):
            if b'error: ' in run(f'wait {token}', b'myos> '):
                raise RuntimeError('concurrent ELF mapping check failed')
        run('run absent', b'error: -5')
        run('wait', b'error: -1')
        run('run sleep 1', b'myos> ')
        first = int(re.search(rb'task: ([0-9]+)', run('spawn sleep 10000', b'task: '))[1])
        second = int(re.search(rb'task: ([0-9]+)', run('spawn sleep 10000', b'task: '))[1])
        if first == second:
            raise RuntimeError('two live tasks share a handle')
        run(f'wait {first} 1', b'error: -21')
        run('run echo unrelated task progresses', b'unrelated task progresses\nmyos> ')
        run(f'stop {second}', b'myos> ')
        run(f'wait {second}', b'error: -1')
        run(f'stop {first}', b'myos> ')
        run(f'wait {first}', b'error: -1')
        run('run echo ordered pipe bytes | cat', b'ordered pipe bytes\nmyos> ')
        run('run echo three-stage | cat | cat', b'three-stage\nmyos> ')
        # Waiting for a producer must also observe its dead, uncollected consumer.
        short = re.findall(rb'task: ([0-9]+)', run('spawn fill 100000 | slow 1 96', b'task: '))
        if len(short) != 2:
            raise RuntimeError('background pipeline did not return two handles')
        producer = run(f'wait {int(short[0])}', b'error: ')
        if b'error: 32' not in producer:
            raise RuntimeError(f'dead consumer retained its input: {producer!r}')
        run(f'wait {int(short[1])}', b'myos> ')

        run('run echo rollback | absent', b'error: -5')
        run('run echo abcdef | fdcheck stdio', b'stdio aliases ok\nmyos> ')
        run('run fill 8192 | slow 1', b'bytes: 8192\nmyos> ')
        early = run('run fill 100000 | slow 1 96', b'bytes: 96')
        if b'error: 32' not in early:
            raise RuntimeError(f'writer did not observe consumer close: {early!r}')
        blocked = re.findall(rb'task: ([0-9]+)', run('spawn fill 100000 | slow 100', b'task: '))
        if len(blocked) != 2:
            raise RuntimeError('pipeline did not return independent handles')
        run('run echo running during backpressure', b'running during backpressure\nmyos> ')
        run(f'stop {int(blocked[1])}', b'myos> ')
        producer = run(f'wait {int(blocked[0])}', b'error: ')
        if b'error: 32' not in producer:
            raise RuntimeError(f'blocked producer was not released: {producer!r}')
        blocked = re.findall(rb'task: ([0-9]+)', run('spawn fill 100000 | slow 100', b'task: '))
        if len(blocked) != 2:
            raise RuntimeError('pipeline admission failed after reuse')
        run(f'stop {int(blocked[0])}', b'myos> ')
        run(f'stop {int(blocked[1])}', b'myos> ')
        run('run echo console-ready', b'console-ready\nmyos> ')
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
        (logs / f'console-{smp}.raw').write_bytes(output)

if __name__ == '__main__':
    qemu, kernel, bundle, harts = sys.argv[1:5]
    iterations = int(sys.argv[6]) if len(sys.argv) == 7 else 3
    for smp in harts.split(','):
        exercise(qemu, kernel, bundle, smp, iterations,
                 service_fault=len(sys.argv) >= 6 and sys.argv[5] == 'service-fault',
                 exec_check=len(sys.argv) >= 6 and sys.argv[5] == 'exec')
