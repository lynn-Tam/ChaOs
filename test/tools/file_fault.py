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

def manual(value, name=None):
    assert bool(value['engaged_']), 'uninitialized ' + str(value.type)
    typ = gdb.lookup_type(name) if name else value.type.strip_typedefs().template_argument(0)
    return value['storage_'].address.cast(typ.pointer()).dereference()
state = manual(gdb.parse_and_eval("'(anonymous namespace)::kernel_storage'"), 'KernelState')
memory = manual(state['objects_'])
def pool(name):
    def find(value):
        if value.type.strip_typedefs() == gdb.lookup_type('libk::ManualLifetime<object::pool<%s> >' % name):
            return manual(value, 'object::pool<%s>' % name)
        for f in value.type.fields():
            child = value.cast(f.type) if f.is_base_class else value[f.name]
            if f.is_base_class or f.name == '_M_head_impl':
                result = find(child)
                if result is not None: return result
        return None
    result = find(memory['pools_'])
    assert result is not None, name
    return result
def live_objects(pool, name):
    slot_type = gdb.lookup_type('object::pool<%s>::Slot' % name)
    header = gdb.lookup_type('object::pool<%s>::PageHeader' % name)
    offset = (header.sizeof + slot_type.alignof - 1) & ~(slot_type.alignof - 1)
    page = pool['storage_']['head_']
    seen = set()
    while int(page):
        assert int(page) not in seen, 'cyclic pool page list'
        seen.add(int(page))
        for index in range((4096 - offset) // slot_type.sizeof):
            slot = gdb.Value(int(page) + offset + index * slot_type.sizeof).cast(slot_type.pointer()).dereference()
            if int(slot['anchor']['phase_']) == 2:
                yield slot['storage'].address.cast(gdb.lookup_type(name).pointer()).dereference()
        page = page['next']
# Requests now live on virtual kernel stacks. Resolve those addresses through
# the real kernel page table instead of assuming every payload is a RAM alias.
layout = manual(state['pmm_'], 'mm::Pmm')['window_']
delta = int(layout['va']['value_']) - int(layout['pa']['value_'])
kroot = manual(state['kernel_vspace_'], 'mm::KSpace')['root_']['root_']
inferior = gdb.selected_inferior()
def alias(address, typ):
    table = int(kroot['value_']) << 12
    for level in (2, 1, 0):
        entry = table + ((address >> (12 + 9 * level)) & 511) * 8
        pte = int.from_bytes(inferior.read_memory(entry + delta, 8), 'little')
        assert pte & 1, 'unmapped kernel address'
        if pte & 14:
            mask = (1 << (12 + 9 * level)) - 1
            phys = (((pte >> 10) << 12) & ~mask) | (address & mask)
            return gdb.Value(phys + delta).cast(typ.pointer()).dereference()
        table = (pte >> 10) << 12
    raise AssertionError('missing kernel leaf')

pagers = list(live_objects(pool('Pager'), 'Pager'))
assert len(pagers) == 1, 'fixture exports one canonical file backing'
pager = pagers[0]
index = pager['claimed_']
assert int(index['size_']) == 1 and int(pager['ready_']['size_']) == 0
ptr = int(index['sentinel_']['next_']) - int(index['hook_offset_'])
claim = gdb.Value(ptr).cast(gdb.lookup_type('Pager::Request').pointer()).dereference()
assert int(claim['info']['id']) != 0 and int(claim['info']['page_index']) == 0
assert int(claim['info']['count']) == 1
def field_offset(typ, name):
    for f in typ.fields():
        if f.name == name: return f.bitpos // 8
        if f.is_base_class:
            offset = field_offset(f.type, name)
            if offset is not None: return f.bitpos // 8 + offset
    return None
node_type = gdb.lookup_type('mm::Cache<mm::PagerData>::Node')
offset = field_offset(node_type, 'request')
assert offset is not None
node = gdb.Value(int(claim.address) - offset).cast(node_type.pointer()).dereference()
queue = node['waiters']
assert int(queue['waiters']['size_']) == 2, 'both actual faults must remain queued'
relations = queue['waiters']
sentinel = int(relations['sentinel_'].address)
hook = int(relations['sentinel_']['next_'])
relation_type = gdb.lookup_type('mm::WaitRelation')
offset = next(f.bitpos // 8 for f in relation_type.fields() if f.name == 'hook_')
threads = set()
while hook != sentinel:
    relation = alias(hook - offset, relation_type)
    assert int(relation['request']) == int(queue.address)
    req = alias(int(relation['owner']), gdb.lookup_type('mm::PageReq'))
    edge = req['done_']['wait_']
    assert int(edge) and int(edge['phase_']) == 1, 'completion must own an attached wait edge'
    assert int(edge['completion_']) == int(relation['owner']) + next(f.bitpos // 8 for f in req.type.fields() if f.name == 'done_')
    threads.add(int(req['thread_'].address))
    hook = int(relation['hook_']['next_'])
assert len(threads) == 2, 'two distinct threads must join the same content request'
print('[file-fault] request=%#x transport claim=%d waiters=2 page=0' % (int(queue.address), int(claim['info']['id'])))
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
