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

def exercise(qemu, kernel, bundle, smp, iterations, recovery=False, exec_check=False, missing_storage=False, disk=None, persisted=False, control=False, threads=False, second_disk=None, vfs=False, swap=False):
    root = Path(__file__).resolve().parents[2]
    logs = root / ('.tmp/project/swap' if swap else '.tmp/project/interactive-storage')
    logs.mkdir(parents=True, exist_ok=True)
    output = bytearray()
    command = [qemu, '-machine', 'virt,iommu-sys=on' if missing_storage else 'virt', '-smp', smp, '-nographic',
               '-bios', 'default', '-kernel', kernel, '-initrd', bundle]
    if disk:
        command[command.index('virt')] = 'virt,iommu-sys=on'
        command += ['-drive', f'if=none,id=data,format=raw,file={disk}',
                    '-device', 'virtio-blk-pci,addr=1,drive=data,disable-legacy=on,iommu_platform=on']
    if second_disk:
        command += ['-drive', f'if=none,id=other,format=raw,file={second_disk}',
                    '-device', 'virtio-blk-pci,addr=2,drive=other,disable-legacy=on,iommu_platform=on']
    if stub := os.environ.get('QEMU_GDB'):
        command += ['-gdb', stub]
    if ram := os.environ.get('QEMU_RAM'):
        command += ['-m', ram]
    process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT)
    poll = selectors.DefaultSelector()
    poll.register(process.stdout, selectors.EVENT_READ)
    trace = (logs / f'console-{smp}.raw').open('wb', buffering=0)

    def until(marker, start=0, timeout=30):
        deadline = time.monotonic() + timeout
        while not (marker.search(output[start:]) if isinstance(marker, re.Pattern) else marker in output[start:]):
            if process.poll() is not None or time.monotonic() >= deadline:
                raise RuntimeError(f'missing {marker!r}')
            for key, _ in poll.select(0.1):
                chunk = os.read(key.fd, 65536)
                if not chunk:
                    raise RuntimeError('QEMU closed serial')
                output.extend(chunk)
                trace.write(chunk)
                if b'KERNEL PANIC' in output or b'user: contained fault' in output:
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

    def capture(name):
        # Preserve the real terminal stream at a stable visual checkpoint.
        end = time.monotonic() + 0.25
        while time.monotonic() < end:
            for key, _ in poll.select(0.02): output.extend(os.read(key.fd, 65536))
        (logs / f'tasks-{name}.raw').write_bytes(output)

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

    def suspend(command, ready=None):
        start = len(output); send(command.encode() + b'\r')
        if ready: until(ready, start)
        else: time.sleep(0.15)
        send(b'\x1a')
        until(b'/ > ', start)
        state = run('jobs', b'Suspend')
        return int(re.findall(rb'\[(\d+)\]', state)[-1])

    try:
        until(b'/ > ')
        if vfs:
            run('control-test vfs', b'[vfs] delayed Get/Bind, independent mount progress, pinned targets and Sync fanout ok')
            print(f'[vfs] OK: {smp} harts, delayed Get/Bind and multi-mount lifecycle')
            return
        if swap:
            if swap == 'swap-sessions':
                run('memcheck contracts', b'memory write/bind, sealed snapshots and destination revoke verified')
                run('swap format pci-10', b'swap format: 0')
                run('swap on pci-10', b'swap on: 0')
                mark = len(output)
                run('memcheck sessions &', b'/ > ')
                until(re.compile(rb'memory independent sessions, content and owner retirement verified|Done -'), mark, 60)
                if b'Done -' in output[mark:]: raise RuntimeError('session content/lifetime check failed')
                until(b'Done 0', mark)
                state = run('swap status', b'pci-10 On')
                if len(re.findall(rb'session=\d+', state)) != 1: raise RuntimeError('retired session retained')
                if not re.search(rb'reads=[1-9]\d*', state): raise RuntimeError('sessions did not refault from disk')
                run('swap off', b'swap off: 0')
                print(f'[swap] OK: {smp} harts, independent sessions, content and owner retirement')
                return
            if swap == 'swap-physical':
                run('swap format pci-10', b'swap format: 0')
                run('swap on pci-10', b'swap on: 0')
                mark = len(output)
                # 8 MiB fits its 16 MiB Quota. Real PMM ballast, allocated by
                # the test kernel after boot, creates the global shortage.
                run('memcheck 8 15000 &', b'/ > ')
                until(b'memory filled bytes=8388608', mark)
                until(b'[pressure] held=', mark)
                state = run('swap status', b'pci-10 On')
                if not re.search(rb'released=[1-9]\d*', state): raise RuntimeError('global pressure did not reclaim pages')
                for cycle in range(1, 4):
                    until(f'memory pass={cycle} bytes=8388608 verified'.encode(), mark, 120)
                    if cycle == 1:
                        if b'[pressure] released free=' in output[mark:]: raise RuntimeError('pressure ended before content verification')
                        state = run('swap status', b'failures=0')
                        if not re.search(rb'reads=[1-9]\d*', state): raise RuntimeError('pressure verification did not refault from disk')
                until(b'[pressure] released free=', mark, 120)
                run('swap off', b'swap off: 0')
                print(f'[swap] OK: {smp} harts, real global PMM pressure, nonbinding client Quota and content progress')
                return
            if swap in ('swap-write-error', 'swap-read-error'):
                run('swap format pci-10', b'swap format: 0')
                run('swap on pci-10', b'swap on: 0')
                run('swap set resident 1048576', b'swap set: 0')
                mark = len(output)
                run('memcheck 8 3000 &', b'/ > ')
                if swap == 'swap-write-error':
                    until(b'memory filled bytes=8388608', mark)
                    until(b'memory pass=3 bytes=8388608 verified', mark)
                else:
                    until(b'Done -', mark)
                    if b'memory pass=3' in output[mark:]: raise RuntimeError('failed reads produced successful content')
                state = run('swap status', b'pci-10 Failed')
                if not re.search(rb'failures=[1-9]\d*', state): raise RuntimeError('injected I/O error was not reported')
                states({'memory': 'running'})
                run('swap off pci-10', b'swap off: 0')
                print(f'[swap] OK: {smp} harts, {swap}, real I/O failure and safe drain')
                return
            if swap in ('swap-failure', 'swap-device-loss'):
                run('swap format pci-10', b'swap format: 0')
                run('swap on pci-10', b'swap on: 0')
                if swap == 'swap-device-loss':
                    run('swap format block', b'swap format: 0')
                    run('swap on block --priority -1', b'swap on: 0')
                run('swap set resident 1048576', b'swap set: 0')
                mark = len(output)
                run('memcheck 8 10000 &', b'/ > ')
                until(b'memory filled bytes=8388608', mark)
                run('sleep 2500', b'/ > ')
                state = run('swap status', b'pci-10 On')
                if not re.search(rb'released=[1-9]\d*', state): raise RuntimeError('failure scenario did not establish backing')
                if swap == 'swap-device-loss':
                    due = time.monotonic() + 10
                    while not re.search(rb'io=\d+/[1-9]\d*', state):
                        if time.monotonic() > due: raise RuntimeError('no in-flight writeback before driver stop')
                        state = run('swap status', b'pci-10 On')
                run('svc stop devices/block-10' if swap == 'swap-device-loss' else 'svc stop memory', b'service change requested')
                until(b'Done -', mark)
                if swap == 'swap-device-loss':
                    states({'memory': 'running', 'devices/block-10': 'idle'})
                    run('swap status', b'pci-10 Failed')
                    run('swap off', b'swap off: 0')
                    run('swap on block', b'swap on: 0')
                    recovered = len(output)
                    run('memcheck 4 1000 &', b'/ > ')
                    until(b'memory pass=3 bytes=4194304 verified', recovered, 90)
                    run('swap off', b'swap off: 0')
                else: states({'memory': 'idle'})
                print(f'[swap] OK: {smp} harts, {swap}, client failure contained and management remains usable')
                return
            run('swap status', b'free=')
            run('swap format pci-10', b'swap format: 0')
            run('swap on pci-10', b'swap on: 0')
            run('swap set swap 262144', b'swap set: 0')
            run('swap set resident 1048576', b'swap set: 0')
            mark = len(output)
            run('memcheck 8 4000 &', b'/ > ')
            until(b'Running', mark)
            run('sleep 2500', b'/ > ')
            state = run('swap status', b'pci-10 On')
            count = re.search(rb'writes=(\d+) released=(\d+)', state)
            if not count or min(map(int, count.groups())) == 0: raise RuntimeError('no real swap writes or released pages')
            usage = re.search(rb'pci-10 On used=(\d+)/', state)
            if not usage or int(usage[1]) > 262144: raise RuntimeError('session swap limit exceeded')
            run('swap set swap 16777216', b'swap set: 0')
            run('swap format block', b'swap format: 0')
            run('swap on block --limit 8388608 --priority -1', b'swap on: 0')
            run('swap on pci-10 --limit 262144', b'swap on: 0')
            run('swap on pci-10 --limit 8388608', b'swap on: 0')
            run('swap off pci-10', b'swap off: 0')
            run('swap pause', b'swap pause: 0')
            run('swap status', b'block Paused')
            run('swap off', b'swap off: 0')
            state = run('swap status', b'pci-10 Off')
            if not re.search(rb'reads=[1-9]\d*', state): raise RuntimeError('no real swap reads')
            until(b'memory pass=3 bytes=8388608 verified')
            run('echo swap-content-ok', b'swap-content-ok\n/ > ')
            run('swap on pci-10 --limit 8388608', b'swap on: 0')
            run('swap on block --limit 8388608 --priority -1', b'swap on: 0')
            run('swap set read-ahead 1', b'swap set: 0')
            run('swap set resident 0', b'swap set: 0')
            mark = len(output)
            run('memcheck 24 5000 &', b'/ > ')
            until(b'Running', mark)
            until(b'memory filled bytes=25165824', mark, 120)
            run('swap off', b'swap off: -8')
            state = run('swap status', b'Paused')
            if not re.search(rb'used=[1-9]\d*/', state): raise RuntimeError('failed off discarded backing')
            run('swap on pci-10 --limit 8388608', b'swap on: 0')
            run('swap on block --limit 8388608 --priority -1', b'swap on: 0')
            for cycle in range(1, 4):
                started = time.monotonic()
                try:
                    until(re.compile(f'memory pass={cycle} bytes=25165824 verified|Done -'.encode()), mark, 120)
                except RuntimeError:
                    run('swap status', b'free=')
                    raise
                if b'Done -' in output[mark:]:
                    run('swap status', b'free=')
                    raise RuntimeError('pressure workload failed')
                print(f'[swap] pressure pass={cycle} verified in {time.monotonic() - started:.2f}s', flush=True)
            run('swap off', b'swap off: 0')
            state = run('swap status', b'block Off')
            if not re.search(rb'failures=0(?: |\n)', state): raise RuntimeError('paging failures under real resident quota pressure')
            mark = len(output)
            run('memcheck 1 1000 &', b'/ > ')
            until(b'memory filled bytes=1048576', mark)
            previous = instance('memory.admin')
            run('bus call memory.admin Release', b'{}')
            run('svc restart memory', b'service change requested')
            until_changed = time.monotonic() + 20
            while (current := instance('memory.admin')) is None or current == previous:
                if time.monotonic() >= until_changed: raise RuntimeError('memory service did not republish')
            run('swap on pci-10 --limit 8388608', b'swap on: 0')
            run('swap set resident 262144', b'swap set: 0')
            until(b'memory pass=3 bytes=1048576 verified', mark)
            run('swap off', b'swap off: 0')
            print(f'[swap] OK: {smp} harts, registered DMA, real reclaim/refault, pause/off, 8MiB drain and 24MiB through a 16MiB quota verified')
            return
        if second_disk:
            run('control-test devices', b'[devices] independent DMA, cross-page 64KiB I/O and driver retirement ok')
            states({'devices/block-8': 'running', 'devices/block-10': 'running'})
            print(f'[devices] OK: {smp} harts, two live drivers, independent data and retirement')
            return
        if threads:
            run('thread-test', b'[threads] pthread, TLS errno, cleanup, shared fd and parallel RPC ok')
            run('thread-test last', b'[threads] initial thread exit preserves argv, fd and workers')
            run('thread-test exit', b'error: 23')
            run('thread-test fault', b'error: -12')
            token = suspend('thread-test hold', b'[threads] ready')
            mark = len(output); send(f'fg {token}\r'.encode()); time.sleep(0.15); send(b'q')
            until(b'/ > ', mark)
            token = suspend('thread-test hold | cat', b'[threads] ready')
            run(f'bg {token}', b'/ > ')
            run(f'kill {token}', b'/ > ')
            run('mkfs', b'/ > '); states({'store': 'running'})
            run('thread-test write', b'[threads] sequential writes and parallel explicit offsets ok')
            run('echo thread-scope-retired', b'thread-scope-retired\n/ > ')
            print(f'[threads] OK: {smp} harts, normal/main/worker exit, faults, Ctrl+Z/fg/bg, pipelines and shared file writes')
            return
        if control:
            run('control-test', b'[control] scoped badges, stale instances/cursors and real VM/quota stats ok')
            run('svc', b'survivor running')
            run('svc remove survivor', b'service change requested')
            print(f'[model] OK: {smp} harts, scoped authority, delegation, deletion and retirement')
            return
        if recovery:
            def managers():
                text = run('bus call @control List', b'"items"')
                return {item['name']: item['run_id'] for line in text.splitlines() if line.startswith(b'{')
                        for item in json.loads(line)['items']}
            export = struct.pack('<IHH', 12, 5, 0) + b'math' + bytes(4)
            exports = (struct.pack('<IHH', 24, 7, 0) + export).hex()
            run(f'bus call @control Configure name=foreign program=/boot/foreign.elf exports=^{exports}', b'"run_id":0')
            run('bus call foreign/math Add a=11 b=31', b'"sum":42')
            before = managers()
            # Replace only the actual owner, without a redundant session wrapper.
            run('sleep 86400000 &', b'Running')
            start = len(output); send(b'svc restart shell\r')
            echo = b'svc restart shell\n'; until(echo, start)
            until(b'/ > ', output.index(echo, start) + len(echo))
            after = managers()
            if after['shell'] == before['shell'] or any(after[n] != before[n] for n in before if n != 'shell'):
                raise RuntimeError('shell replacement escaped its ownership boundary')
            if b'Running' in run('jobs', b'/ > '): raise RuntimeError('old pipeline survived owner replacement')
            run('bus call foreign/math Add a=19 b=23', b'"sum":42')

            run('control-test', b'[control] scoped badges, stale instances/cursors and real VM/quota stats ok')
            run('svc', b'survivor running')
            run('svc remove survivor', b'service change requested')
            run('sleep 86400000 &', b'Running')
            mark = len(output); run('svc restart devices', b'service change requested')
            until(b'uart: console ready\n', mark)
            run('svc', b'devices/uart running')
            current = managers()
            if current['devices'] == after['devices'] or current['shell'] != after['shell'] or current['boot'] != after['boot']:
                raise RuntimeError('device replacement escaped its ownership boundary')
            run('jobs', b'Running')
            run('cat /boot/README.TXT', b'Disk file service')
            run('bus call foreign/math Add a=21 b=21', b'"sum":42')
            print(f'[supervisor] OK: {smp} harts, branch isolation, dynamic recipe checkpoint and lazy reactivation')
            return
        run('help', b'PROGRAM [ARGS]')
        run('svc', b'shell running')
        run('ls /boot', b'cat.elf')
        run('cat /boot/README.TXT', b'Disk file service')
        if persisted:
            run('cat note', b'hello-again')
            states({'store': 'running'})
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
            run('mkfs', b'/ > ')
            states({'store': 'running'})
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
            run('control-test', b'[control] scoped badges, stale instances/cursors and real VM/quota stats ok')
        if exec_check or disk:
            program = '/boot/foreign.elf'
            if disk:
                run('cp /boot/foreign.elf package', b'/ > ')
                program = '/package'
            # Public Array(String("math")); the independently linked client
            # consumes the same Value bytes as every peer, with no service SDK.
            export = struct.pack('<IHH', 12, 5, 0) + b'math' + bytes(4)
            exports = (struct.pack('<IHH', 24, 7, 0) + export).hex()
            run(f'bus call @control Configure name=foreign program={program} exports=^{exports}', b'"run_id":0')
            run('bus wait foreign/math', b'/ > ')
            run('bus describe foreign/math', b'"Add"')
            run('bus call foreign/math Add a=11 b=31', b'"sum":42')
        if disk:
            store_instance = instance('store.admin')
            if store_instance is None: raise RuntimeError('missing store publication')
        start = len(output); send(b'tasks\r'); until(b'TASKS', start)
        until(b'shell', start); until(b'Running', start)
        # The task list uses program names, not shell deployment identifiers.
        time.sleep(1.2)
        capture('programs')
        mark = len(output); send(b'?'); until(b'Keys', mark)
        send(b'\x1b'); time.sleep(0.15)
        mark = len(output); send(b't'); until(b'Tree', mark); send(b'ts')
        send(b'/uart\r'); until(b'uart', start)
        mark = len(output); send(b'\r'); until(b'Program', mark); until(b'/boot/uart.elf', mark)
        until(b'Elapsed', mark)
        until(re.compile(rb'[0-9]+:[0-9]{2}:[0-9]{2}'), mark)
        capture('details')
        send(b'\x1b'); time.sleep(0.15)
        send(b'\x1b'); time.sleep(0.15)  # Clear the filter.
        send(b' '); until(b'Paused', start); send(b' ')
        mark = len(output); send(b'2'); until(b'Serial port', mark); until(b'In use', mark)
        capture('devices')
        send(b'\r'); until(b'Firmware', mark)
        send(b'\x1b'); time.sleep(0.15)
        if disk:
            until(b'Virtio block', mark); until(b'Ready', mark)
            mark = len(output); send(b'/virtio\r\r'); until(b'PCI 0:1.0', mark)
            send(b'\x1b'); time.sleep(0.15); send(b'\x1b'); time.sleep(0.15)
        send(b'1'); time.sleep(0.2)
        send(b'\x1a'); until(b'/ > ', start)
        state = run('jobs', b'Suspend'); token = int(re.findall(rb'\[(\d+)\]', state)[-1])
        start = len(output); send(f'fg {token}\r'.encode()); until(b'TASKS', start)
        if disk:
            send(b'/block\r'); time.sleep(0.3)
            mark = len(output); send(b'a'); until(b'End program', mark)
            send(b'\r'); until(b'End program?', mark); send(b'\r')
            # Stopped deployments are an explicit filter, not running programs.
            send(b'h'); until(b'Stopped', mark)
            # Observing devices neither removes the device nor restarts its driver.
            mark = len(output); send(b'2'); until(b'Virtio block', mark); until(b'Stopped', mark)
            mark = len(output); send(b'\r'); until(b'Location', mark)
            if b'Problem' in output[mark:]: raise RuntimeError('intentional stop reported as device failure')
            send(b'\x1b'); time.sleep(0.15); send(b'1'); send(b'/block\r'); time.sleep(0.3)
            mark = len(output); send(b'a'); until(b'Start', mark)
            # A prior list repaint can contain other running programs. Wait
            # for this action's reply before checking the filtered row again.
            mark = len(output); send(b'\r'); until(b'Requested', mark); until(b'Running', mark)
        send(b'q'); until(b'/ > ', start)
        start = len(output); send(b'view tasks\r'); until(b'TASKS', start)
        until(b'Running', start); time.sleep(0.3)
        mark = len(output); send(b'a'); until(b'Read only', mark)
        send(b'q'); until(b'/ > ', start)
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
        trace.close()
        (logs / f'console-{smp}.raw').write_bytes(output)

if __name__ == '__main__':
    qemu, kernel, bundle, harts = sys.argv[1:5]
    harts = os.environ.get("QEMU_SMP", harts)
    mode = sys.argv[5] if len(sys.argv) >= 6 else ''
    iterations = int(sys.argv[6]) if len(sys.argv) >= 7 else 1
    for smp in harts.split(','):
        if mode in ('interactive', 'recovery', 'control', 'threads', 'devices', 'vfs') or mode.startswith('swap'):
            temporary = Path(__file__).resolve().parents[2] / '.tmp/project/interactive-storage'
            temporary.mkdir(parents=True, exist_ok=True)
            with tempfile.TemporaryDirectory(dir=temporary) as directory:
                disk = Path(directory) / 'data.img'
                with disk.open('wb') as stream: stream.truncate(16 * 1024 * 1024)
                other = Path(directory) / 'other.img' if mode == 'devices' or mode.startswith('swap') else None
                if other:
                    with other.open('wb') as stream: stream.truncate(16 * 1024 * 1024)
                    if mode in ('swap-write-error', 'swap-read-error'):
                        event = 'write_aio' if mode == 'swap-write-error' else 'read_aio'
                        fault = Path(directory) / 'blkdebug.conf'
                        # Keep the format/enable header accessible; inject
                        # below the real driver, never in production code.
                        if mode == 'swap-read-error':
                            fault.write_text('[set-state]\nevent = "flush_to_disk"\nstate = "1"\nnew_state = "2"\n'
                                             '[set-state]\nevent = "write_aio"\nstate = "2"\nnew_state = "3"\n'
                                             '[inject-error]\nevent = "read_aio"\nstate = "3"\nerrno = "5"\n')
                        else:
                            fault.write_text(f'[set-state]\nevent = "{event}"\nstate = "1"\nnew_state = "2"\n'
                                             f'[inject-error]\nevent = "{event}"\nstate = "2"\nerrno = "5"\n')
                        other = f'blkdebug:{fault}:{other}'
                exercise(qemu, kernel, bundle, smp, iterations, swap=mode if mode.startswith('swap') else False, vfs=mode == 'vfs', second_disk=other, disk=disk, recovery=mode == 'recovery', control=mode == 'control', threads=mode == 'threads')
                if mode == 'interactive': exercise(qemu, kernel, bundle, smp, 0, disk=disk, persisted=True)
        else:
            exercise(qemu, kernel, bundle, smp, iterations, exec_check=mode == 'exec', missing_storage=mode == 'missing-storage', control=mode == 'control')
