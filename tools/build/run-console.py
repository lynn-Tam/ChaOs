#!/usr/bin/env python3
"""Exercise the production console through ordinary serial bytes."""
import json
import os
from pathlib import Path
import re
import selectors
import subprocess
import struct
import sys
import time
import tempfile

def exercise(qemu, kernel, bundle, smp, iterations, recovery=False, exec_check=False, missing_storage=False, disk=None, persisted=False):
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
        while not (marker.search(output[start:]) if isinstance(marker, re.Pattern) else marker in output[start:]):
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
        until(re.compile(rb'(?:^|\n)/[^\r\n]* > '), boundary)
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

    def instance(name):
        transcript = run(f'bus call . Get name={name}', b'/ > ')
        for line in transcript.splitlines():
            if line.startswith(b'{'): return json.loads(line)['instance']
        return None

    def suspend(command):
        start = len(output); send(command.encode() + b'\r'); time.sleep(0.15); send(b'\x1a')
        until(b'/ > ', start)
        state = run('jobs', b'Suspend')
        return int(re.findall(rb'\[(\d+)\]', state)[-1])

    try:
        until(b'/ > ')
        if recovery:
            def managers():
                text = run('bus call system List', b'"items"')
                return {item['name']: item['gen'] for line in text.splitlines() if line.startswith(b'{')
                        for item in json.loads(line)['items']}
            export = struct.pack('<IHH', 12, 5, 0) + b'math' + bytes(4)
            exports = (struct.pack('<IHH', 24, 7, 0) + export).hex()
            run(f'bus call session Configure name=foreign program=/boot/foreign.elf exports=^{exports}', b'"gen":0')
            run('bus call foreign/math Add a=11 b=31', b'"sum":42')
            before = managers()
            # Persist a default package's desired state, then recover its fresh
            # grants when the enclosing manager is replaced below.
            start = len(output); send(b'svc restart shell\r')
            until(b'myos native shell\n', start); until(b'/ > ', output.index(b'myos native shell\n', start))
            if managers() != before: raise RuntimeError('shell replacement changed its parent or siblings')
            run('bus call foreign/math Add a=20 b=22', b'"sum":42')
            run('sleep 10000 &', b'Running')
            start = len(output); send(b'svc restart session\r')
            until(b'myos native shell\n', start); until(b'/ > ', output.index(b'myos native shell\n', start))
            after = managers()
            if after['session'] == before['session'] or after['boot'] != before['boot'] or after['devices'] != before['devices']:
                raise RuntimeError('session replacement escaped its ownership boundary')
            if b'Running' in run('jobs', b'/ > '): raise RuntimeError('old job survived session replacement')
            run('bus call foreign/math Add a=19 b=23', b'"sum":42')
            run('sleep 10000 &', b'Running')
            mark = len(output); run('svc restart devices', b'service change requested')
            until(b'uart: console ready\n', mark)
            run('bus describe devices', b'"List"')
            current = managers()
            if current['devices'] == after['devices'] or current['session'] != after['session'] or current['boot'] != after['boot']:
                raise RuntimeError('device replacement escaped its ownership boundary')
            run('jobs', b'Running')
            run('cat /boot/README.TXT', b'myos disk file service')
            run('bus call foreign/math Add a=21 b=21', b'"sum":42')
            print(f'[supervisor] OK: {smp} harts, branch isolation, dynamic recipe checkpoint and lazy reactivation')
            return
        run('help', b'PROGRAM [ARGS]')
        run('svc', b'shell running')
        run('ls /boot', b'cat.elf')
        run('cat /boot/README.TXT', b'myos disk file service')
        if persisted:
            states({'store': 'running'})
            run('cat note', b'hello-again')
            run('cat notes/item', b'nested')
            run('cat edited', b'editor-retained')
            run('stat empty', b'0 bytes')
            run('stat tree/branch/item', b'0 bytes')
            run('ls tree/branch', b'nested')
            run('cat work-renamed/moved', b'cwd-data')
            print(f'[interactive] OK: {smp} harts, files persisted across QEMU death')
            return
        for _ in range(iterations): run('echo console-ready', b'console-ready\n/ > ')
        run('echo ordered pipe bytes | cat | cat', b'ordered pipe bytes\n/ > ')
        run('echo "&"', b'&\n/ > ')
        run('echo rollback | absent', b'error: -5')
        token = suspend('sleep 10000 | cat')
        run(f'bg {token}', b'/ > ')
        run(f'wait {token} 1', b'error: -21')
        run(f'kill {token}', b'/ > ')
        token = suspend('fill 1000000 | slow 50') if disk or exec_check else 0
        if token:
            run(f'bg {token}', b'/ > ')
            run(f'kill {token}', b'/ > ')
        state = run('cat &', b'Running')
        token = int(re.findall(rb'\[(\d+)\]', state)[-1])
        run('echo foreground-input', b'foreground-input\n/ > ')
        run(f'kill {token}', b'/ > ')
        start = len(output); send(b'cat\r'); time.sleep(0.1); send(b'\x03'); until(b'/ > ', start)
        run('echo stop-reuse', b'stop-reuse\n/ > ')
        if exec_check:
            run('cow', b'ELF data, COW and BSS ok')
            run('cow', b'ELF data, COW and BSS ok')
        if disk:
            states({'store': 'running'})
            run('mkfs', b'/ > ')
            run('create note', b'/ > ')
            run('write note hello', b'/ > ')
            run('create note', b'/ > ')  # Must preserve existing content.
            run('append note -again', b'/ > ')
            run('cat note', b'hello-again')
            run('mkdir notes', b'/ > ')
            if b'error' in run('touch -- note empty -spare', b'/ > '):
                raise RuntimeError('touch failed to create or preserve files')
            run('cat note', b'hello-again')
            run('stat empty', b'0 bytes')
            run('stat -spare', b'0 bytes')
            if b'error' in run('mkdir -p tree/branch tree/other /boot', b'/ > '):
                raise RuntimeError('mkdir -p failed to create or accept directories')
            if b'error' in run('mkdir -p tree/branch', b'/ > '):
                raise RuntimeError('mkdir -p failed on an existing directory')
            run('mkdir tree/branch', b'mkdir: tree/branch: error')
            run('mkdir -p empty/child', b'mkdir: empty/child: error')
            run('touch absent/child kept', b'touch: absent/child: error')
            run('stat kept', b'0 bytes')
            run('/boot/touch.elf tree/branch/item', b'/ > ')
            run('/boot/mkdir.elf -- tree/branch/nested', b'/ > ')
            run('ls tree/branch', b'nested')
            run('stat tree/branch/item', b'0 bytes')
            run('echo nested > notes/item', b'/ > ')
            run('mkdir -p work/sub work/out', b'/ > ')
            run('cd work', b'/work > ')
            run('pwd', b'/work\n')
            run('echo cwd-data > file', b'/work > ')
            run('touch a b sub/item', b'/work > ')
            run('mv a b out', b'/work > ')
            run('mv file moved', b'/work > ')
            run('mv sub out', b'/work > ')
            run('cd out/sub', b'/work/out/sub > ')
            run('pwd', b'/work/out/sub\n')
            run('touch local', b'/work/out/sub > ')
            run('cd ../..', b'/work > ')
            run('cd missing', b'error: ')
            run('pwd', b'/work\n')
            run('cd /', b'/ > ')
            run('mv work work-renamed', b'/ > ')
            run('cat work-renamed/moved', b'cwd-data')
            run('rm work-renamed', b'rm: work-renamed: error')
            if b'error' in run('rm -rf work-renamed/out absent', b'/ > '):
                raise RuntimeError('recursive/forced removal failed')
            listing = run('ls work-renamed', b'moved')
            if b'out\n' in listing: raise RuntimeError('recursive removal left its tree')
            run('touch disposable', b'/ > ')
            run('rm absent disposable', b'rm: absent: error')
            run('stat disposable', b'error: ')
            run('rm -rf /', b'rm: /: error')
            run('cat notes/item', b'nested')
            run('write /boot/denied no', b'error: ')
            start = len(output); send(b'edit edited\r'); until(b'Ctrl+O Save', start)
            send(b'editor-retained'); send(b'\x1a'); until(b'/ > ', start)
            state = run('jobs', b'Suspend'); token = int(re.findall(rb'\[(\d+)\]', state)[-1])
            start = len(output); send(f'fg {token}\r'.encode()); until(b'editor-retained', start)
            send(b'\x0f'); until(b'Written', start); send(b'\x18'); until(b'/ > ', start)
            run('cat edited', b'editor-retained')
            paused = []
            for _ in range(2):
                start = len(output); send(b'edit edited\r'); until(b'Ctrl+O Save', start)
                send(b'X\x1a'); until(b'/ > ', start)
                state = run('jobs', b'Suspend')
                paused.append(int(re.findall(rb'\[(\d+)\]', state)[-1]))
            run('fd', b'error: -5')
            for _ in range(2):
                start = len(output); send(b'fg\r'); until(b'Xeditor-retained', start)
                send(b'\x1a'); until(b'/ > ', start)
            start = len(output); send(b'fg\r'); until(b'Xeditor-retained', start)
            send(b'\x18'); until(b'Save changes?', start); send(b'n'); until(b'/ > ', start)
            run(f'kill {paused[0]}', b'/ > ')
            if b'.edit-' in run('ls', b'edited'): raise RuntimeError('opening/suspending edit left drafts')
            start = len(output); send(b'edit /boot/README.TXT\r'); until(b'Ctrl+O Save', start)
            send(b'X\x0f'); until(b'Save failed:', start)
            send(b'\x18'); until(b'Save changes?', start); send(b'n'); until(b'/ > ', start)
            run('control-test', b'[control] scoped badges, stale instances/cursors and real VM/account stats ok')
        if exec_check or disk:
            program = '/boot/foreign.elf'
            if disk:
                run('cp /boot/foreign.elf package', b'/ > ')
                program = '/package'
            # Public Array(String("math")); the independently linked client
            # consumes the same Value bytes as every peer, with no service SDK.
            export = struct.pack('<IHH', 12, 5, 0) + b'math' + bytes(4)
            exports = (struct.pack('<IHH', 24, 7, 0) + export).hex()
            run(f'bus call session Configure name=foreign program={program} exports=^{exports}', b'"gen":0')
            run('bus wait foreign/math', b'/ > ')
            run('bus describe foreign/math', b'"Add"')
            run('bus call foreign/math Add a=11 b=31', b'"sum":42')
        if disk:
            store_instance = instance('store.admin')
            if store_instance is None: raise RuntimeError('missing store publication')
        start = len(output); send(b'tasks\r'); until(b'myos tasks', start)
        until(b'shell', start); until(b'Running', start)
        send(b'fshell\r3'); until(b'Slot', start); until(b'Grant', start); until(b'Valid', start)
        send(b'l'); time.sleep(0.3); send(b'\x1b[F'); until(b'deployment', start)
        start = len(output); send(b'h'); until(b'Slot', start)
        start = len(output); send(b'\x1b[Hv'); until(b'| grants', start)
        start = len(output); send(b'v'); until(b'| objects', start); until(b'Domain', start)
        send(b'fVSpace\r'); time.sleep(0.3)
        start = len(output); send(b'\r'); until(b'link_id', start); until(b'flags', start)
        send(b'hh')
        send(b'1'); send(b'\x1a'); until(b'/ > ', start)
        state = run('jobs', b'Suspend'); token = int(re.findall(rb'\[(\d+)\]', state)[-1])
        start = len(output); send(f'fg {token}\r'.encode()); until(b'myos tasks', start)
        if disk:
            send(b'2fblock\r'); until(b'block-8', start)
            time.sleep(0.6)
            mark = len(output); send(b'3'); until(b'| objects', mark); until(b'IRQ', mark)
            send(b'2fblock\r'); time.sleep(0.6)
            mark = len(output); send(b'x'); until(b'Idle', mark)
            mark = len(output); send(b's'); until(b'Running', mark)
        send(b'q'); until(b'/ > ', start)
        start = len(output); send(b'view tasks\r'); until(b'myos tasks', start)
        time.sleep(0.5); send(b'q'); until(b'/ > ', start)
        if disk:
            deadline = time.monotonic() + 20
            while (current := instance('store.admin')) is None or current == store_instance:
                if time.monotonic() >= deadline: raise RuntimeError('store did not republish after device replacement')
            states({'store': 'running'})
            run('cat note', b'hello-again')
            run('fs sync', b'/ > ')
        if missing_storage: run('cat note', b'error: ')
        print(f'[interactive] OK: {smp} harts, pipelines, Ctrl+C/Z, focus, dynamic management and file/device interaction')
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
        if mode in ('interactive', 'recovery'):
            temporary = Path(__file__).resolve().parents[2] / '.tmp/project/interactive-storage'
            temporary.mkdir(parents=True, exist_ok=True)
            with tempfile.TemporaryDirectory(dir=temporary) as directory:
                disk = Path(directory) / 'data.img'
                with disk.open('wb') as stream: stream.truncate(16 * 1024 * 1024)
                exercise(qemu, kernel, bundle, smp, iterations, disk=disk, recovery=mode == 'recovery')
                if mode == 'interactive': exercise(qemu, kernel, bundle, smp, 0, disk=disk, persisted=True)
        else:
            exercise(qemu, kernel, bundle, smp, iterations, exec_check=mode == 'exec', missing_storage=mode == 'missing-storage')
