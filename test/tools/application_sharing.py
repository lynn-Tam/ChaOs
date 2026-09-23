#!/usr/bin/env python3
"""Read-only Sv39 evidence for two live, disk-loaded demand applications."""
import os
from pathlib import Path
import re
import selectors
import subprocess
import sys
import tempfile
import time

qemu, gdb, nm, kernel, bundle, disk, application = sys.argv[1:]
root = Path(__file__).resolve().parents[2]
logs = root / '.tmp/project/application-sharing'
logs.mkdir(parents=True, exist_ok=True)
symbols = subprocess.check_output([nm, '-n', application], text=True)

def symbol(name):
    rows = [line.split() for line in symbols.splitlines()
            if (line.split()[-1] == name if name == '_start' else line.split()[-1].endswith(name))]
    if len(rows) != 1:
        raise RuntimeError(f'ambiguous symbol: {name}')
    return int(rows[0][0], 16)

code = symbol('_start')
initialized = symbol('initializedE')
zeroed = symbol('zeroedE')
# Reading DWARF field layouts avoids duplicating kernel object offsets. The
# only hardware-specific interpretation is the actual Sv39 PTE format.
probe = f'''
import gdb

def manual(value, name):
    assert bool(value['engaged_'])
    return value['storage_'].address.cast(gdb.lookup_type(name).pointer()).dereference()

state = manual(gdb.parse_and_eval("'(anonymous namespace)::kernel_storage'"), 'kernel::KernelState')
objects = manual(state['objects_'], 'kernel::object::ObjectStore')
direct = manual(state['direct_map_'], 'kernel::mm::DirectMap')
delta = int(direct['virtual_base_']['value_']) - int(direct['physical_base_']['value_'])
inferior = gdb.selected_inferior()
def physical(address, size):
    return bytes(inferior.read_memory(address + delta, size))

def translate(root, address):
    table = root
    for level in (2, 1, 0):
        index = (address >> (12 + level * 9)) & 511
        pte = int.from_bytes(physical(table + index * 8, 8), 'little')
        if not pte & 1: return None
        if pte & 14:
            mask = (1 << (12 + level * 9)) - 1
            return (((pte >> 10) << 12) & ~mask) | (address & mask), pte & 1023
        table = (pte >> 10) << 12
    return None

slot_type = gdb.lookup_type('kernel::object::ObjectPool<kernel::mm::VSpace>::Slot')
page_type = gdb.lookup_type('kernel::object::ObjectPool<kernel::mm::VSpace>::PageHeader')
offset = (page_type.sizeof + slot_type.alignof - 1) & ~(slot_type.alignof - 1)
count = (4096 - offset) // slot_type.sizeof
page = objects['vspaces_']['pages_head_']
found = {{}}
while int(page):
    for index in range(count):
        slot = gdb.Value(int(page) + offset + index * slot_type.sizeof).cast(slot_type.pointer()).dereference()
        if int(slot['anchor']['lifecycle_']) != 2: continue
        space = slot['storage'].address.cast(gdb.lookup_type('kernel::mm::VSpace').pointer()).dereference()
        if not bool(space['root_']['engaged_']): continue
        user = manual(space['root_'], 'arch::UserRoot')
        root = int(user['root_page_']['frame_']['value_']) << 12
        data, bss = translate(root, {initialized}), translate(root, {zeroed})
        if data is None or bss is None: continue
        seed = physical(data[0], 1)[0]
        if seed not in (17, 29) or physical(bss[0], 1)[0] != seed + 7: continue
        text = translate(root, {code})
        assert text is not None and (text[1] & 30) == 26, 'code must be user RX'
        assert (data[1] & 30) == 22 and (bss[1] & 30) == 22, 'private data must be user RW'
        assert seed not in found, 'ambiguous live application identity'
        found[seed] = (root, text[0], data[0], bss[0])
    page = page.dereference()['next']
assert set(found) == {{17, 29}}, 'both live demand instances must be present'
a, b = found[17], found[29]
assert a[0] != b[0], 'instances must have independent address spaces'
assert a[1] == b[1], 'readonly code must share its physical page'
assert a[2] != b[2] and a[3] != b[3], 'initialized data and BSS must be physically private'
for seed, row in sorted(found.items()):
    print('[sharing] seed=%d root=%#x code=%#x data=%#x bss=%#x' % (seed, *row))
print('[sharing] physical code sharing and private data/BSS verified')
'''

with tempfile.TemporaryDirectory(dir=logs) as directory:
    socket = str(Path(directory) / 'qemu.sock')
    script = Path(directory) / 'inspect.py'
    script.write_text(probe)
    command = [qemu, '-machine', 'virt,iommu-sys=on', '-nographic', '-bios', 'default',
               '-kernel', kernel, '-initrd', bundle, '-smp', '4',
               '-drive', f'if=none,id=disk,format=raw,readonly=on,file={disk}',
               '-device', 'virtio-blk-pci,addr=1,drive=disk,disable-legacy=on,iommu_platform=on',
               '-gdb', f'unix:{socket},server=on,wait=off']
    process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    poll = selectors.DefaultSelector()
    poll.register(process.stdout, selectors.EVENT_READ)
    serial = bytearray()
    def until(marker, start=0):
        deadline = time.monotonic() + 30
        while marker not in serial[start:]:
            if process.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError(f'missing {marker!r}')
            for key, _ in poll.select(.1):
                serial.extend(os.read(key.fd, 65536))
            if b'MYOS KERNEL PANIC' in serial or b'user: contained fault' in serial:
                raise RuntimeError('unexpected guest failure')
    def command(text):
        start = len(serial)
        for byte in text.encode() + b'\r':
            process.stdin.write(bytes([byte])); process.stdin.flush(); time.sleep(.003)
        until(b'myos> ', start)
        return start
    try:
        until(b'myos> ')
        ids = []
        for seed in (17, 29):
            start = command(f'spawn demand 60000 {seed}')
            until(b'[demand] holding private pages', start)
            ids.append(int(re.search(rb'task: ([0-9]+)', serial[start:])[1]))
        result = subprocess.run([gdb, '-q', '-nx', '-batch', kernel,
            '-ex', 'set pagination off', '-ex', f'target remote {socket}',
            '-ex', f'source {script}', '-ex', 'detach'], text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
        (logs / 'inspection.log').write_text(result.stdout)
        print(result.stdout)
        if result.returncode != 0 or '[sharing] physical code sharing and private data/BSS verified' not in result.stdout:
            raise RuntimeError('physical sharing check failed')
        # Resume the unmodified guest and verify ordinary stop/reuse still works.
        for task in ids:
            start = command(f'stop {task}')
            if b'exit: -18' not in serial[start:]: raise RuntimeError('stop failed')
        start = command('run hello')
        if b'Hello from userspace.\nexit: 0' not in serial[start:]: raise RuntimeError('reuse failed')
    finally:
        (logs / 'serial.log').write_bytes(serial)
        process.terminate()
        try: process.wait(timeout=3)
        except subprocess.TimeoutExpired: process.kill(); process.wait()
        poll.close()
