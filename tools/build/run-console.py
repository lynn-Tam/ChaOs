#!/usr/bin/env python3
"""Exercise the production console through ordinary serial bytes."""
import os
from pathlib import Path
import re
import selectors
import subprocess
import sys
import time
import tempfile

def exercise(qemu, kernel, bundle, smp, iterations, service_fault=False, exec_check=False, missing_storage=False, disk=None, persisted=False):
    root = Path(__file__).resolve().parents[2]
    logs = root / '.tmp/project/interactive-storage'
    logs.mkdir(parents=True, exist_ok=True)
    output = bytearray()
    command = [qemu, '-machine', 'virt,iommu-sys=on' if missing_storage else 'virt', '-smp', smp, '-nographic',
               '-bios', 'default', '-kernel', kernel, '-initrd', bundle]
    if disk:
        command[command.index('virt')] = 'virt,iommu-sys=on'
        command += ['-drive', f'if=none,id=data,format=raw,file={disk}',
                    '-device', 'virtio-blk-pci,addr=1,drive=data,disable-legacy=on,iommu_platform=on']
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
        # A background completion may repaint the prompt while this command is
        # still being typed. Only a prompt after its complete echo ends it.
        echo = text.encode() + b'\n'
        until(echo, start)
        boundary = output.index(echo, start) + len(echo)
        until(b'myos> ', boundary)
        transcript = output[start:]
        if expected not in transcript:
            raise RuntimeError(f'{text}: expected {expected!r}, got {transcript!r}')
        return transcript

    def send(data):
        for byte in data:
            process.stdin.write(bytes([byte])); process.stdin.flush(); time.sleep(0.003)

    def states(expected):
        end = time.monotonic() + 20
        while True:
            state = run('svc', b'shell running')
            if all(f'{name} {value}'.encode() in state for name, value in expected.items()): return
            if time.monotonic() > end: raise RuntimeError(f'state did not reach {expected}: {state!r}')

    def suspend(command):
        start = len(output); send(command.encode() + b'\r'); time.sleep(0.15); send(b'\x1a')
        until(b'myos> ', start)
        state = run('jobs', b'Suspend')
        return int(re.findall(rb'\[(\d+)\]', state)[-1])

    try:
        until(b'myos> ')
        run('help', b'PROGRAM [ARGS]')
        run('svc', b'shell running')
        run('ls /boot', b'cat.elf')
        run('cat /boot/README.TXT', b'myos disk file service')
        if persisted:
            states({'store': 'running'})
            run('cat note', b'hello-again')
            run('cat notes/item', b'nested')
            run('cat edited', b'editor-retained')
            print(f'[interactive] OK: {smp} harts, files persisted across QEMU death')
            return
        for _ in range(iterations): run('echo console-ready', b'console-ready\nmyos> ')
        run('echo ordered pipe bytes | cat | cat', b'ordered pipe bytes\nmyos> ')
        run('echo "&"', b'&\nmyos> ')
        run('echo rollback | absent', b'error: -5')
        token = suspend('sleep 10000 | cat')
        run(f'bg {token}', b'myos> ')
        run(f'wait {token} 1', b'error: -21')
        run(f'kill {token}', b'myos> ')
        token = suspend('fill 1000000 | slow 50') if disk or exec_check else 0
        if token:
            run(f'bg {token}', b'myos> ')
            run(f'kill {token}', b'myos> ')
        state = run('cat &', b'Running')
        token = int(re.findall(rb'\[(\d+)\]', state)[-1])
        run('echo foreground-input', b'foreground-input\nmyos> ')
        run(f'kill {token}', b'myos> ')
        start = len(output); send(b'cat\r'); time.sleep(0.1); send(b'\x03'); until(b'myos> ', start)
        run('echo stop-reuse', b'stop-reuse\nmyos> ')
        if exec_check:
            run('cow', b'ELF data, COW and BSS ok')
            run('cow', b'ELF data, COW and BSS ok')
        if service_fault:
            for name in ('files', 'vfs'):
                run(f'svc stop {name}', b'service change requested')
                states({name: 'stopped'})
                run(f'svc start {name}', b'service change requested')
                states({'vfs': 'running'})
            run('cat /boot/README.TXT', b'myos disk file service')
        if disk:
            states({'store': 'running'})
            run('mkfs', b'myos> ')
            run('create note', b'myos> ')
            run('write note hello', b'myos> ')
            run('create note', b'myos> ')  # Must preserve existing content.
            run('append note -again', b'myos> ')
            run('cat note', b'hello-again')
            run('mkdir notes', b'myos> ')
            run('echo nested > notes/item', b'myos> ')
            run('cat notes/item', b'nested')
            run('write /boot/denied no', b'error: ')
            start = len(output); send(b'edit edited\r'); until(b'Ctrl+O Save', start)
            send(b'editor-retained'); send(b'\x1a'); until(b'myos> ', start)
            state = run('jobs', b'Suspend'); token = int(re.findall(rb'\[(\d+)\]', state)[-1])
            start = len(output); send(f'fg {token}\r'.encode()); until(b'editor-retained', start)
            send(b'\x0f'); until(b'Written', start); send(b'\x18'); until(b'myos> ', start)
            run('cat edited', b'editor-retained')
            paused = []
            for _ in range(2):
                start = len(output); send(b'edit edited\r'); until(b'Ctrl+O Save', start)
                send(b'X\x1a'); until(b'myos> ', start)
                state = run('jobs', b'Suspend')
                paused.append(int(re.findall(rb'\[(\d+)\]', state)[-1]))
            run('fd', b'error: -5')
            for _ in range(2):
                start = len(output); send(b'fg\r'); until(b'Xeditor-retained', start)
                send(b'\x1a'); until(b'myos> ', start)
            start = len(output); send(b'fg\r'); until(b'Xeditor-retained', start)
            send(b'\x18'); until(b'Save changes?', start); send(b'n'); until(b'myos> ', start)
            run(f'kill {paused[0]}', b'myos> ')
            if b'.edit-' in run('ls', b'edited'): raise RuntimeError('opening/suspending edit left drafts')
            start = len(output); send(b'edit /boot/README.TXT\r'); until(b'Ctrl+O Save', start)
            send(b'X\x0f'); until(b'Save failed:', start)
            send(b'\x18'); until(b'Save changes?', start); send(b'n'); until(b'myos> ', start)
            run('control-test', b'[control] scoped badges, stale instances/cursors and real VM/account stats ok')
        start = len(output); send(b'tasks\r'); until(b'myos tasks', start)
        until(b'shell Running', start)
        send(b'jjjjj3'); until(b'actual selector', start); until(b'valid', start)
        send(b'd' * 16); until(b'dependency ', start)
        send(b'1'); send(b'\x1a'); until(b'myos> ', start)
        state = run('jobs', b'Suspend'); token = int(re.findall(rb'\[(\d+)\]', state)[-1])
        start = len(output); send(f'fg {token}\r'.encode()); until(b'myos tasks', start)
        if disk:
            send(b'2'); until(b'block-8', start)
            time.sleep(0.6)
            mark = len(output); send(b'x'); until(b'Stopped', mark)
            mark = len(output); send(b's'); until(b'Running', mark)
        send(b'q'); until(b'myos> ', start)
        start = len(output); send(b'view tasks\r'); until(b'myos tasks', start)
        time.sleep(0.5); send(b'q'); until(b'myos> ', start)
        if disk:
            states({'store': 'running'})
            run('cat note', b'hello-again')
            run('fs sync', b'myos> ')
        if missing_storage: run('cat note', b'error: ')
        print(f'[interactive] OK: {smp} harts, pipelines, Ctrl+C/Z, focus, typed management and file/device interaction')
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
    mode = sys.argv[5] if len(sys.argv) >= 6 else ''
    iterations = int(sys.argv[6]) if len(sys.argv) >= 7 else 1
    for smp in harts.split(','):
        if mode == 'interactive':
            temporary = Path(__file__).resolve().parents[2] / '.tmp/project/interactive-storage'
            temporary.mkdir(parents=True, exist_ok=True)
            with tempfile.TemporaryDirectory(dir=temporary) as directory:
                disk = Path(directory) / 'data.img'
                with disk.open('wb') as stream: stream.truncate(16 * 1024 * 1024)
                exercise(qemu, kernel, bundle, smp, iterations, disk=disk)
                exercise(qemu, kernel, bundle, smp, 0, disk=disk, persisted=True)
        else:
            exercise(qemu, kernel, bundle, smp, iterations, service_fault=mode == 'service-fault',
                     exec_check=mode == 'exec', missing_storage=mode == 'missing-storage')
