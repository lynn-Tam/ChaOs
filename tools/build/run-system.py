#!/usr/bin/env python3
"""Boot the complete system; verification uses its own scratch disk and QMP."""
import argparse
import json
import os
import re
import queue
import threading
from pathlib import Path
import selectors
import socket
import subprocess
import sys
import tempfile
import time


def run(args, cpus, directory, resume=False):
    root = Path(directory)
    disk = Path(args.disk) if args.disk else (root / 'disk.img' if args.check else Path(args.bundle).parent / 'data.img')
    if not disk.exists():
        with disk.open('xb') as f:
            f.truncate((32 if args.check else 256) << 20)
        if not args.check: print(f'Created {disk}; run mkfs once in the terminal to initialize files.', flush=True)
    native = args.check or args.native_display
    qmp_path = root / 'qmp.sock'
    command = [args.qemu, '-machine', 'virt,iommu-sys=on', '-m', '512M', '-smp', cpus,
               '-bios', 'default', '-kernel', args.kernel, '-initrd', args.bundle,
               '-monitor', 'none', '-qmp', f'unix:{qmp_path},server=on,wait=off',
               '-display', 'none' if args.check or not native else os.environ.get('QEMU_DISPLAY', 'gtk'),
               '-serial', 'stdio' if args.check else 'mon:stdio',
               '-drive', f'if=none,id=data,format=raw,file={disk}',
               '-device', 'virtio-blk-pci,addr=1,drive=data,disable-legacy=on,iommu_platform=on']
    viewer = None
    viewer_log = None
    if native:
        command += ['-device', 'virtio-gpu-pci,addr=2,iommu_platform=on,xres=800,yres=600',
                    '-device', 'virtio-keyboard-pci,addr=3,iommu_platform=on',
                    '-device', 'virtio-tablet-pci,addr=4,iommu_platform=on']
    else:
        program = Path(args.viewer) if args.viewer else Path(args.kernel).resolve().parent / 'host-viewer'
        if not program.is_file():
            raise RuntimeError('host-viewer is missing; install SDL2 development files and build host-viewer')
        pixels = root / 'pixels.bin'
        pixel_bytes = 64 << 20
        with pixels.open('xb') as file: file.truncate(pixel_bytes)
        endpoint = root / 'host.sock'
        viewer_log = (root / 'host.log').open('wb', buffering=0)
        viewer = subprocess.Popen([str(program), str(endpoint), str(pixels)], stdin=subprocess.DEVNULL,
                                  stdout=viewer_log, stderr=subprocess.STDOUT, start_new_session=True)
        end = time.monotonic() + 10
        while not endpoint.exists():
            if viewer.poll() is not None or time.monotonic() >= end:
                if viewer.poll() is None: viewer.terminate(); viewer.wait()
                viewer_log.close()
                raise RuntimeError('host-viewer: ' + (root / 'host.log').read_text(errors='replace').strip())
            time.sleep(0.01)
        command += ['-object', f'memory-backend-file,id=hostmem,mem-path={pixels},size={pixel_bytes},share=on',
                    '-device', 'ivshmem-plain,addr=3,memdev=hostmem',
                    '-chardev', f'socket,id=host,path={endpoint},reconnect-ms=250',
                    '-device', 'virtio-serial-pci,id=hostlink,addr=2,disable-legacy=on,iommu_platform=on,max_ports=2',
                    '-device', 'virtserialport,bus=hostlink.0,chardev=host,name=org.myos.gui']
    if os.environ.get('QEMU_GDB'):
        command += ['-gdb', os.environ['QEMU_GDB']]
    if not args.check:
        process = subprocess.Popen(command)
        try:
            if not viewer: return process.wait()
            exits = queue.Queue()
            def watch(child):
                threading.Thread(target=lambda: exits.put((child, child.wait())), daemon=True).start()
            watch(process); watch(viewer)
            while True:
                child, result = exits.get()
                if child is process: return result
                # The exited process owns this private listener. QEMU reconnects;
                # Hello fences its old window readers before pixels are reused.
                time.sleep(0.25)
                endpoint.unlink(missing_ok=True)
                viewer = subprocess.Popen([str(program), str(endpoint), str(pixels)], stdin=subprocess.DEVNULL,
                                          stdout=viewer_log, stderr=subprocess.STDOUT, start_new_session=True)
                watch(viewer)
        finally:
            if process.poll() is None: process.terminate(); process.wait()
            if viewer:
                if viewer.poll() is None: viewer.terminate(); viewer.wait()
                viewer_log.close()
                details = (root / 'host.log').read_text(errors='replace').strip()
                if details: print(details, file=sys.stderr)
    log = (root / f'serial-{cpus}{"-reboot" if resume else ""}.log').open('wb', buffering=0)
    process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    poll = selectors.DefaultSelector()
    poll.register(process.stdout, selectors.EVENT_READ)
    output = bytearray()
    qmp = socket.socket(socket.AF_UNIX)
    wire = None

    def pump(duration=0.05):
        for key, _ in poll.select(duration):
            data = os.read(key.fd, 65536)
            if not data:
                raise RuntimeError('QEMU serial closed')
            output.extend(data)
            log.write(data)
        if b'KERNEL PANIC' in output or b'user: contained fault' in output:
            raise RuntimeError(f'guest fault: {output[-2000:]!r}')

    def until(marker, start=0, timeout=40):
        end = time.monotonic() + timeout
        while marker not in output[start:]:
            if process.poll() is not None or time.monotonic() >= end:
                raise RuntimeError(f'missing {marker!r}: {output[-4000:]!r}')
            pump()

    def serial(text):
        start = len(output)
        for c in text.encode() + b'\r':
            process.stdin.write(bytes([c])); process.stdin.flush(); pump(0.003)
        until(text.encode() + b'\n', start)
        offset = output.index(text.encode() + b'\n', start) + len(text) + 1
        until(b'/ > ', offset)
        return bytes(output[offset:])

    def execute(name, data=None):
        request = {'execute': name}
        if data is not None:
            request['arguments'] = data
        wire.write((json.dumps(request) + '\n').encode())
        wire.flush()
        while True:
            result = json.loads(wire.readline())
            if 'error' in result:
                raise RuntimeError(result['error'])
            if 'return' in result:
                return result['return']

    def pause(seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            pump(min(0.05, end - time.monotonic()))

    def key(code, modifiers=()):
        keys = [*modifiers, code]
        events = [{'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': k}}}
                  for down, names in [(True, keys), (False, reversed(keys))] for k in names]
        execute('input-send-event', {'events': events})
        pump(0.02)

    def type_text(text):
        names = {' ': 'spc', '\n': 'ret', '-': 'minus', '/': 'slash', '.': 'dot'}
        for char in text:
            if char == '>': key('dot', ['shift'])
            else: key(names.get(char, char))

    def pointer(x, y, down=None):
        events = [{'type': 'abs', 'data': {'axis': axis, 'value': int(value * 32767 / limit)}}
                  for axis, value, limit in [('x', x, 799), ('y', y, 599)]]
        if down is not None:
            events.append({'type': 'btn', 'data': {'button': 'left', 'down': down}})
        execute('input-send-event', {'events': events})
        pause(0.1)

    def json_call(command):
        data = serial(command)
        for line in data.splitlines():
            if line.startswith(b'{'):
                return json.loads(line)
        raise RuntimeError(f'no GUI state: {data!r}')

    def stat():
        return json_call('bus call gui Stat')

    def connected(s):
        return s['display']['ready'] and len(s['inputs']) == 2 and all(i['ready'] for i in s['inputs'])

    def terminal_run():
        return next(c['run_id'] for c in json_call('bus call @control List')['items'] if c['name'] == 'term')

    def wait_state(predicate, timeout=40):
        end = time.monotonic() + timeout
        while True:
            state = stat()
            if predicate(state):
                return state
            if time.monotonic() >= end:
                raise RuntimeError(f'GUI condition timed out: {state}')
            pause(0.15)

    def file_contains(path, marker, timeout=30):
        end = time.monotonic() + timeout
        while True:
            data = serial('cat ' + path)
            if marker.encode() in data:
                return
            if time.monotonic() >= end:
                raise RuntimeError(f'graphical file operation failed: {path}: {data!r}')
            pause(0.2)

    def capture(name):
        execute('screendump', {'filename': str(root / f'{name}-{cpus}.ppm')})

    def wait_game_exit():
        end = time.monotonic() + 30
        while True:
            state = serial('svc')
            if not any(name in state for name in [b'term/shell/doom.elf', b'term/shell/fs.elf']): break
            if time.monotonic() >= end:
                raise RuntimeError('Doom process did not retire')
            pause(0.2)
        wait_state(lambda s: len(s['windows']) == 1)

    try:
        until(b'/ > ')
        if b'error:' in serial('svc start term'): raise RuntimeError('could not start the explicit graphical terminal fixture')
        end = time.monotonic() + 5
        while True:
            try:
                qmp.connect(str(qmp_path)); break
            except FileNotFoundError:
                if time.monotonic() >= end: raise
                pump()
        wire = qmp.makefile('rwb')
        json.loads(wire.readline())
        execute('qmp_capabilities')
        state = wait_state(lambda s: connected(s) and s['sessions'] == 1 and
                           len(s['windows']) == 1 and s['windows'][0]['buffer'])
        term_run = terminal_run()
        if resume:
            if b'doomsav0.dsg' not in serial('ls doom/savegame/doom.wad'):
                raise RuntimeError('save disappeared after power cycle')
            type_text('doom -loadgame 0\n')
            wait_state(lambda s: len(s['windows']) == 2 and all(w['buffer'] for w in s['windows']), timeout=120)
            pause(2); capture('doom-loaded')
            key('tab', ['alt']); key('z', ['ctrl']); pause(0.3)
            type_text('kill\n'); wait_game_exit()
            type_text('fs write gui-note after-kill\n'); file_contains('gui-note', 'after-kill')
            if terminal_run() != term_run: raise RuntimeError('killing Doom restarted the terminal execution')
            print(f'[gui] OK: {cpus} harts, power-cycle Doom load, suspend/kill and usable terminal', flush=True)
            return
        capture('terminal')
        original_window = state['windows'][0]['id']
        pause(0.5); idle = stat()['frames']; pointer(760, 550); pause(0.3)
        if stat()['frames'] != idle: raise RuntimeError('idle cursor movement repainted the scene')
        if b'error:' in serial('mkfs'):
            raise RuntimeError('formatting the independent test disk failed')
        type_text('fs write gui-note gui-input\n')
        file_contains('gui-note', 'gui-input')
        type_text('edit gui-edit\n'); pause(1)
        type_text('editor-retained'); key('o', ['ctrl']); pause(0.5)
        file_contains('gui-edit', 'editor-retained')
        key('x', ['ctrl']); pause(0.3)
        type_text('tasks\n'); pause(1); capture('tasks')
        serial('svc restart gui'); pause(0.5)
        state = wait_state(lambda s: connected(s) and len(s['windows']) == 1 and s['windows'][0]['buffer'])
        if terminal_run() != term_run: raise RuntimeError('GUI restart lost the terminal execution')
        original_window = state['windows'][0]['id']
        capture('tasks-recovered'); key('q'); pause(0.3)
        key('f11')
        wait_state(lambda s: s['windows'][0]['flags'] & 1)
        capture('fullscreen'); key('f11')
        state = wait_state(lambda s: len(s['windows']) == 1 and not s['windows'][0]['flags'] & 1)
        w = state['windows'][0]
        pointer(w['x'] + 40, w['y'] - 10, True)
        pointer(w['x'] + 90, w['y'] + 20)
        pointer(w['x'] + 90, w['y'] + 20, False)
        state = wait_state(lambda s: s['windows'][0]['x'] != w['x'])
        w = state['windows'][0]
        pointer(w['x'] + w['width'] - 4, w['y'] + w['height'] - 4, True)
        pointer(w['x'] + w['width'] + 28, w['y'] + w['height'] + 28)
        pointer(w['x'] + w['width'] + 28, w['y'] + w['height'] + 28, False)
        wait_state(lambda s: s['windows'][0]['width'] != w['width'])
        if stat()['windows'][0]['id'] != original_window: raise RuntimeError('geometry change recreated the terminal session')
        capture('resized')
        for service, marker in [('devices/input-18', 'after-input'), ('devices/display-10', 'after-display'), ('gui', 'after-gui')]:
            if b'error:' in serial('svc restart ' + service):
                raise RuntimeError('restart failed: ' + service)
            pause(0.5)
            wait_state(lambda s: connected(s) and len(s['windows']) == 1 and s['windows'][0]['buffer'])
            type_text('fs write gui-note ' + marker + '\n')
            file_contains('gui-note', marker)
        capture('recovered')
        if args.doom:
            baseline = stat()
            type_text('doom -warp 1 1 -skill 3 > doom-run.log\n')
            wait_state(lambda s: len(s['windows']) == 2 and all(w['buffer'] for w in s['windows']), timeout=120)
            pause(3); capture('doom')
            before = {w['id'] for w in baseline['windows']}
            game = next(w for w in stat()['windows'] if w['id'] not in before)
            fullscreen_frame = stat()['frames']
            key('f11')
            wait_state(lambda s: s['frames'] >= fullscreen_frame + 2 and any(w['id'] == game['id'] and w['flags'] & 1 and w.get('buffer_width') == game['buffer_width'] and w.get('buffer_height') == game['buffer_height'] for w in s['windows']))
            capture('doom-fullscreen'); key('f11')
            wait_state(lambda s: all(not (w['flags'] & 1) for w in s['windows']))
            serial('svc restart devices/display-10'); pause(0.5)
            wait_state(lambda s: connected(s) and len(s['windows']) == 2 and all(w['buffer'] for w in s['windows']))
            key('f2'); pause(0.3); key('ret'); type_text('gui-save'); key('ret'); pause(1)
            end = time.monotonic() + 40
            while b'doomsav0.dsg' not in serial('ls doom/savegame/doom.wad'):
                if time.monotonic() >= end: raise RuntimeError('Doom did not commit its save')
                pause(0.2)
            key('f10'); pause(0.2); key('y')
            wait_game_exit()
            config = serial('cat doom/default.cfg').decode(errors='replace')
            for name, code in [('key_fire', 36), ('key_use', 57), ('key_strafeleft', 51), ('key_straferight', 52)]:
                if not re.search(r'(?m)^' + name + r'[ \t]+' + str(code) + r'[ \t]*$', config):
                    raise RuntimeError('Doom key did not survive configuration serialization: ' + name)
            (root / f'performance-{cpus}.json').write_text(json.dumps({
                'cpus': int(cpus), 'before': baseline, 'after': stat(),
                'gpu': json_call('bus call display Stat')}, indent=2) + '\n')
        if terminal_run() != term_run: raise RuntimeError('recovery restarted the terminal execution')
        result = stat()
        (root / f'state-{cpus}.json').write_text(json.dumps(result, indent=2) + '\n')
        print(f'[gui] OK: {cpus} harts, files/edit/tasks, geometry, input/display/gui recovery' +
              (', Doom play/save/config/exit' if args.doom else ''), flush=True)
    except Exception:
        if wire and process.poll() is None:
            try:
                capture('failed')
                if args.doom: key('tab', ['alt']); pause(0.3); capture('failed-terminal')
                print(serial('svc').decode(errors='replace'), flush=True)
            except Exception as diagnostic:
                print(f'Additional diagnostic failed: {diagnostic}', flush=True)
        raise
    finally:
        process.kill(); process.wait(); poll.close()
        if wire: wire.close()
        qmp.close(); log.close()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('qemu'); p.add_argument('kernel'); p.add_argument('bundle'); p.add_argument('cpus')
    p.add_argument('--native-display', action='store_true', help='use guest GPU/Input scanout instead of independent host windows')
    p.add_argument('--viewer', help='host window viewer executable')
    p.add_argument('--check', action='store_true'); p.add_argument('--disk', help='normal boot disk (default: data.img beside the bundle)')
    p.add_argument('--doom', action='store_true', help='also exercise a configured local IWAD')
    args = p.parse_args()
    if args.check and args.disk:
        p.error('--check always uses a scratch disk; --disk is for normal boot')
    if args.doom and not args.check:
        p.error('--doom selects a check workload; normal boot includes the configured game data')
    base = Path(__file__).resolve().parents[2] / '.tmp/project/run'
    base.mkdir(parents=True, exist_ok=True)
    if not args.check:
        with tempfile.TemporaryDirectory(dir=base) as directory:
            raise SystemExit(run(args, args.cpus, directory))
    for cpus in args.cpus.split(','):
        if os.environ.get('GUI_KEEP'):
            directory = base / ('check-' + cpus); directory.mkdir(exist_ok=True)
            run(args, cpus, directory)
            if args.doom: run(args, cpus, directory, resume=True)
        else:
            with tempfile.TemporaryDirectory(dir=base) as directory:
                run(args, cpus, directory)
                if args.doom: run(args, cpus, directory, resume=True)


if __name__ == '__main__':
    main()
