#!/usr/bin/env python3
"""Hold a real blkdebug read, inspect joined faults, then supply or kill Files."""
import json
import os
from pathlib import Path
import selectors
import socket
import subprocess
import struct
import sys
import tempfile
import time

qemu, gdb, kernel, bundle, disk, cpus = sys.argv[1:7]
logs = Path(__file__).resolve().parents[2] / '.tmp/project/file-fault'
logs.mkdir(parents=True, exist_ok=True)
probe = '''
import gdb

def manual(value, name):
    assert bool(value['engaged_']), 'uninitialized ' + name
    return value['storage_'].address.cast(gdb.lookup_type(name).pointer()).dereference()
state = manual(gdb.parse_and_eval("'(anonymous namespace)::kernel_storage'"), 'kernel::KernelState')
objects = manual(state['objects_'], 'kernel::object::ObjectStore')
def live_objects(pool, name):
    slot_type = gdb.lookup_type('kernel::object::ObjectPool<%s>::Slot' % name)
    header = gdb.lookup_type('kernel::object::ObjectPool<%s>::PageHeader' % name)
    offset = (header.sizeof + slot_type.alignof - 1) & ~(slot_type.alignof - 1)
    page = pool['pages_head_']
    seen = set()
    while int(page):
        assert int(page) not in seen, 'cyclic pool page list'
        seen.add(int(page))
        for index in range((4096 - offset) // slot_type.sizeof):
            slot = gdb.Value(int(page) + offset + index * slot_type.sizeof).cast(slot_type.pointer()).dereference()
            if int(slot['anchor']['lifecycle_']) == 2:
                yield slot['storage'].address.cast(gdb.lookup_type(name).pointer()).dereference()
        page = page['next']
requests = []
for thread in live_objects(objects['threads_'], 'kernel::Thread'):
    wait = thread['wait_']
    if int(wait['local_kind_']) != 1 or int(wait['phase_']) != 1: continue
    fault = wait['local_']['page']
    if int(fault['address_']['value_']) != 0x75000000: continue
    assert int(fault['phase_']['value_']) == 3, 'fault must be Armed'
    request = fault['relation_']['request']
    assert int(request), 'fault must belong to a PageRequest'
    requests.append(request)
assert len(requests) == 2 and int(requests[0]) == int(requests[1]), 'two faults must join one canonical request'
request = requests[0].dereference()
# MemoryObject remains Published until supply/fail begins. The independent
# Pager transport owns the service's claim; do not infer it from PageSlot.
assert int(request['state']) == 3 and int(request['claim_generation']) == 0
assert int(request['waiters']['size_']) == 2, 'request must retain both real waiters'
assert int(request['first']) == 0 and int(request['count']) == 1
pagers = list(live_objects(objects['pagers_'], 'kernel::pager::Pager'))
assert len(pagers) == 1, 'fixture exports one canonical file backing'
pager = pagers[0]
assert int(pager['claimed_']) == 1 and int(pager['ready_']['size_']) == 0
slots = pager['slots_']
first, last = slots.type.range()
claims = [slots[i] for i in range(first, last + 1) if int(slots[i]['state']) == 3]
assert len(claims) == 1
claim = claims[0]
assert int(claim['claim_generation']) != 0 and int(claim['claim_index']) != 0
assert int(claim['page_index']) == int(request['key']['index'])
assert int(claim['page_generation']) == int(request['key']['generation'])
assert int(claim['payload']['page_in']['count']) == 1
print('[file-fault] request=%#x transport claim=%d waiters=2 page=0' % (int(requests[0]), int(claim['claim_generation'])))
print('[file-fault] joined pending faults verified')
'''

for count in cpus.split(','):
    for mode in sys.argv[7:] or ('r', 'k'):
        name = f'{count}-{mode}'
        with tempfile.TemporaryDirectory(dir=logs) as directory:
            directory = Path(directory)
            qmp_socket = directory / 'qmp.sock'
            script = directory / 'inspect.py'; script.write_text(probe)
            process = subprocess.Popen([qemu, '-machine', 'virt,iommu-sys=on', '-nographic',
                '-bios', 'default', '-kernel', kernel, '-initrd', bundle, '-smp', count, '-m', '128M',
                '-drive', f'if=none,id=disk,format=raw,readonly=on,file=blkdebug::{disk}',
                '-object', 'iothread,id=disk-io',
                '-device', 'virtio-blk-pci,addr=1,drive=disk,iothread=disk-io,disable-legacy=on,iommu_platform=on',
                '-qmp', f'unix:{qmp_socket},server=on,wait=off'],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            poll = selectors.DefaultSelector(); poll.register(process.stdout, selectors.EVENT_READ)
            serial = bytearray()
            qmp = None
            def until(marker, occurrences=1):
                deadline = time.monotonic() + 30
                while serial.count(marker) < occurrences:
                    if process.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError(f'{name}: missing {marker!r}')
                    for key, _ in poll.select(.1): serial.extend(os.read(key.fd, 65536))
                    if b'MYOS KERNEL PANIC' in serial:
                        drain_until = time.monotonic() + 2
                        while time.monotonic() < drain_until:
                            for key, _ in poll.select(.1): serial.extend(os.read(key.fd, 65536))
                        raise RuntimeError('kernel panic')
            def send(text):
                process.stdin.write(text.encode()); process.stdin.flush()
            def execute(command, arguments=None):
                stream.write((json.dumps({'execute': command, 'arguments': arguments or {}}) + '\n').encode())
                stream.flush()
                while True:
                    reply = json.loads(stream.readline())
                    if 'event' in reply: continue
                    if 'error' in reply: raise RuntimeError(reply)
                    return reply['return']
            def monitor(command):
                result = execute('human-monitor-command', {'command-line': f'qemu-io disk "{command}"'})
                if result: raise RuntimeError(result)
            try:
                until(b'[file-fault] mapped, awaiting host')
                qmp = socket.socket(socket.AF_UNIX); qmp.settimeout(15); qmp.connect(str(qmp_socket))
                stream = qmp.makefile('rwb'); json.loads(stream.readline()); execute('qmp_capabilities')
                monitor('break read_aio page-in')
                send('g')
                # Delay is only a scheduling opportunity; the read-only check
                # below, not elapsed time, proves two actual pending faults.
                time.sleep(.5)
                # GDB's live stop drains block I/O, which cannot finish while
                # blkdebug holds this read. Snapshot physical RAM without
                # stopping CPUs, then inspect its kernel aliases offline.
                ram = directory / 'ram.bin'
                execute('pmemsave', {'val': 0x80000000, 'size': 128 * 1024 * 1024,
                                    'filename': str(ram)})
                core = directory / 'memory.core'
                ident = b'\x7fELF' + bytes([2, 1, 1, 0]) + bytes(8)
                # Use the actual ELF load addresses: bootstrap and linked
                # higher-half code do not have the same physical base.
                image = Path(kernel).read_bytes()
                elf = struct.unpack_from('<16sHHIQQQIHHHHHH', image)
                mappings = [(4096, 0xffffffc080000000, 0x80000000, ram.stat().st_size)]
                for index in range(elf[10]):
                    kind, flags, offset, virtual, physical, filesz, memsz, align = struct.unpack_from(
                        '<IIQQQQQQ', image, elf[5] + index * elf[9])
                    if kind == 1 and virtual >= 0xffffffff80000000:
                        mappings.append((4096 + physical - 0x80000000, virtual, physical, memsz))
                header = struct.pack('<16sHHIQQQIHHHHHH', ident, 4, 243, 1,
                    0, 64, 0, 0, 64, 56, len(mappings), 0, 0, 0)
                segments = b''.join(struct.pack('<IIQQQQQQ', 1, 6, offset, address,
                    physical, length, length, 4096) for offset, address, physical, length in mappings)
                with core.open('wb') as output, ram.open('rb') as source:
                    output.write(header + segments)
                    output.seek(4096)
                    while data := source.read(1024 * 1024): output.write(data)
                inspection = subprocess.run([gdb, '-q', '-nx', '-batch', kernel,
                    '-ex', 'set pagination off', '-ex', 'set python print-stack full', '-ex', f'core-file {core}',
                    '-ex', f'source {script}'], text=True,
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
                (logs / f'{name}-inspection.log').write_text(inspection.stdout)
                if inspection.returncode or '[file-fault] joined pending faults verified' not in inspection.stdout:
                    core.replace(logs / 'failed.core')
                    (logs / 'failed-probe.py').write_text(probe)
                    raise RuntimeError(inspection.stdout)
                print(inspection.stdout, flush=True)
                send(mode)
                if mode == 'k':
                    until(b'[file-fault] stopping Files with page-in pending')
                # Stop may wait for Block's borrowed DMA buffer; let that
                # exact held read finish before checking terminal outcomes.
                monitor('remove_break page-in')
                marker = (b'[file-fault] Files death released both faults, Block survived' if mode == 'k'
                          else b'[file-fault] shared request supplied both mappings')
                until(marker)
                if mode == 'r' and b'user: contained fault' in serial:
                    raise RuntimeError('unexpected successful-path fault')
                print(f'[file-fault] OK: {count} harts, ' + ('service death' if mode == 'k' else 'shared page-in'), flush=True)
            except subprocess.TimeoutExpired as error:
                (logs / f'{name}-inspection.log').write_bytes(error.stdout or b'')
                raise
            finally:
                (logs / f'{name}-serial.log').write_bytes(serial)
                if qmp is not None: qmp.close()
                process.terminate()
                try: process.wait(timeout=3)
                except subprocess.TimeoutExpired: process.kill(); process.wait()
                poll.close()
