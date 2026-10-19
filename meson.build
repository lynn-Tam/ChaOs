project(
  'myos',
  ['c', 'cpp'],
  version: '0.1.0',
  default_options: ['c_std=gnu11', 'cpp_std=gnu++23', 'buildtype=plain', 'b_ndebug=true', 'b_pie=false', 'b_staticpic=false'],
)

profile = get_option('profile')

trace_enabled = get_option('trace')

scenario_name = get_option('scenario')
scenario_id = 0
if scenario_name == 'ordinary'
  scenario_id = 1
elif scenario_name == 'initrd'
  scenario_id = 2
elif scenario_name == 'trap'
  scenario_id = 3
elif scenario_name == 'remote'
  scenario_id = 4
elif scenario_name == 'dispatch'
  scenario_id = 7
endif

root = meson.project_source_root()
# Debian's bare-metal GCC installs the freestanding C++ headers but omits
# their target config directory from the compiler's default search path.
cpp_config_dir = '/usr/lib/riscv64-unknown-elf/include/c++/' + meson.get_compiler('cpp').version() + '/riscv64-unknown-elf'
cpp_config_arg = '-I' + cpp_config_dir
kernel_common_args = [
  '-ffreestanding', '-Wall', '-Wextra', '-O2', '-g3',
  '-march=rv64gc', '-mabi=lp64d', '-mcmodel=medany', '-msmall-data-limit=0',
  '-I' + root,
  '-I' + join_paths(root, 'arch/riscv64'),
]
arch_common_args = kernel_common_args
kernel_common_args += ['-I' + join_paths(root, 'kernel')]
kernel_c_args = kernel_common_args + ['-Werror=implicit-function-declaration']
freestanding_cpp_args = [
  cpp_config_arg,
  '-fno-exceptions', '-fno-rtti', '-fno-threadsafe-statics', '-fno-use-cxa-atexit',
  '-fno-omit-frame-pointer', '-fno-optimize-sibling-calls', '-fstack-usage',
]
kernel_cpp_args = kernel_common_args + freestanding_cpp_args
arch_cpp_args = arch_common_args + freestanding_cpp_args
kernel_link_args = [
  '-march=rv64gc', '-mabi=lp64d', '-nostdlib', '-lgcc',
  '-Wl,-T,' + join_paths(root, 'arch/riscv64/kernel.ld'),
]

arch_sources = files(
  'arch/riscv64/boot.S',
  'arch/riscv64/cpu.cpp', 'arch/riscv64/cpu.S', 'arch/riscv64/sbi.cpp',
  'arch/riscv64/trap.S', 'arch/riscv64/trap.cpp',
)
core_kernel_sources = files(
  'kernel/io/space.cpp', 'kernel/mm/table.cpp', 'kernel/boot/fdt.cpp',
  'kernel/boot/root.cpp',
  'kernel/cap/view.cpp',  'kernel/cap/grant.cpp',
  'kernel/cap/cspace.cpp', 'kernel/ipc/port.cpp', 'kernel/ipc/channel.cpp', 'kernel/mm/pager.cpp',
  'kernel/irq/irq.cpp',
  'kernel/object/pool.cpp', 'kernel/resource/account.cpp',
  'kernel/sched/sc.cpp',
  'kernel/sched/dispatcher.cpp',
  'kernel/task/thread.cpp',
  'kernel/wait.cpp',
  'kernel/task/env.cpp',
  'kernel/syscall/call.cpp', 'kernel/syscall/create.cpp',
  'kernel/syscall/ipc.cpp',
  'kernel/mm/mem.cpp', 'kernel/mm/phys.cpp', 'kernel/mm/vspace.cpp',
  'kernel/mm/kspace.cpp', 'kernel/mm/tlb.cpp',
  'kernel/mm/pmm.cpp',
  'libk/mem.c',
)

arch = static_library('kernel-arch', arch_sources, c_args: arch_common_args, cpp_args: arch_cpp_args, pic: false)
core = static_library('kernel-core', core_kernel_sources, c_args: kernel_c_args, cpp_args: kernel_cpp_args, pic: false)
trace = static_library('kernel-trace', files('kernel/trace.cpp'),
  c_args: kernel_c_args, cpp_args: kernel_cpp_args + ['-DTRACE_ENABLED=' + (trace_enabled ? '1' : '0')], pic: false)
sync = static_library('kernel-sync', files('kernel/sync.cpp'),
  c_args: kernel_c_args, cpp_args: kernel_cpp_args, pic: false)
panic = static_library('kernel-panic', files('kernel/panic.cpp'),
  c_args: kernel_c_args, cpp_args: kernel_cpp_args, pic: false)
# Entry selects test calls and the IPI transport at link time. Common
# mechanisms are compiled once; production has no test selectors or providers.
entry_sources = files('kernel/boot/start.cpp', 'kernel/cpu/cpu.cpp', 'kernel/trap.cpp', 'kernel/cpu/ipi.cpp')
entry = static_library('kernel-entry', entry_sources,
  c_args: kernel_c_args, cpp_args: kernel_cpp_args + ['-DTEST_ENABLED=0'], pic: false)
test_entry = static_library('kernel-test-entry', entry_sources,
  c_args: kernel_c_args, cpp_args: kernel_cpp_args + ['-DTEST_ENABLED=1'], pic: false)
test_configs = {}
foreach cfg : [['debug', scenario_id, get_option('panic_probe')], ['wait', 11, 0]]
  test_configs += {cfg[0]: static_library('test-boot-' + cfg[0], files('test/boot.cpp'),
    c_args: kernel_c_args,
    cpp_args: kernel_cpp_args + ['-DTEST_SCENARIO=' + cfg[1].to_string(), '-DTEST_PANIC=' + cfg[2].to_string()],
    pic: false, build_by_default: false)}
endforeach
test_sources = files(
  'test/framework.cpp', 'test/scenario.cpp',
  'test/scenario_basic.cpp', 'test/scenario_wait.cpp', 'test/scenario_remote.cpp',
  'test/libk_test.cpp',
  'test/sync_test.cpp', 'test/allocator_test.cpp', 'test/bootinfo_test.cpp',
  'test/sched_test.cpp',
  'test/cap_test.cpp', 'test/memory_test.cpp', 'test/tlb_test.cpp',
  'test/vspace_test.cpp', 'test/user_test.cpp', 'test/ipc_test.cpp',
)
test_module = static_library(
  'kernel-test', test_sources, c_args: kernel_c_args,
  cpp_args: kernel_cpp_args + ['-fomit-frame-pointer', '-foptimize-sibling-calls'],
  pic: false, build_by_default: false,
)

sh = find_program('sh')
image_suffix = '-riscv64-' + profile + '-trace' + (trace_enabled ? '1' : '0')
build_info_args = kernel_cpp_args
debug_build_info_args = build_info_args
if get_option('build_id') == 'auto'
  build_info_source = vcs_tag(
    command: [sh, files('tools/build/build-id.sh'), root],
    input: 'tools/build/build_info.cpp.in', output: 'build_info_auto.cpp',
    replace_string: '@VCS_TAG@', fallback: 'unknown-clean',
  )
  build_info_args = kernel_cpp_args + ['-DIMAGE_SUFFIX="-riscv64-kernel-concoff"']
  debug_build_info_args = kernel_cpp_args + ['-DIMAGE_SUFFIX="' + image_suffix + '"']
else
  build_info_source = configure_file(input: 'tools/build/build_info.cpp.in',
    output: 'build_info.cpp', configuration: {'VCS_TAG': get_option('build_id')})
endif
kernel = executable(
  'kernel.elf', build_info_source,
  c_args: kernel_c_args, cpp_args: build_info_args,
  link_whole: [arch, core, entry, trace, sync, panic],
  link_args: kernel_link_args + ['-Wl,-Map,' + join_paths(meson.current_build_dir(), 'kernel.map')],
  link_language: 'c',
)
kernel_debug = executable(
  'kernel_debug.elf', build_info_source,
  c_args: kernel_c_args, cpp_args: debug_build_info_args,
  link_whole: [arch, core, test_entry, trace, sync, panic, test_configs['debug'], test_module],
  link_args: kernel_link_args + ['-Wl,-Map,' + join_paths(meson.current_build_dir(), 'kernel_debug.map')],
  link_language: 'c', build_by_default: false,
)
kernel_wait = executable(
  'kernel_wait.elf', build_info_source,
  c_args: kernel_c_args, cpp_args: debug_build_info_args,
  link_whole: [arch, core, test_entry, trace, sync, panic, test_configs['wait'], test_module],
  link_args: kernel_link_args, link_language: 'c', build_by_default: false,
)
user_common_args = [
  '-Wall', '-Wextra', '-Werror', '-O2', '-g3',
  '-march=rv64imac_zicsr_zifencei', '-mabi=lp64', '-mcmodel=medany', '-msmall-data-limit=0', '-I' + root, '-I' + join_paths(root, 'user/include'),
]
user_c_args = user_common_args + ['-ffreestanding', '-idirafter', get_option('newlib_include')]
user_cpp_args = user_common_args + ['-fno-exceptions', '-fno-rtti', '-fno-threadsafe-statics', '-fno-use-cxa-atexit']
# Container headers need the target C declarations; only used runtime symbols link.
user_cpp_args += [cpp_config_arg, '-idirafter', get_option('newlib_include')]
user_link_args = ['-march=rv64imac_zicsr_zifencei', '-mabi=lp64', '-nostdlib', '-lgcc', '-Wl,-T,' + join_paths(root, 'user/riscv64/linker.ld')]
crt = static_library(
  'crt', files('user/crt/start.cpp', 'user/crt/heap.cpp', 'libk/mem.c'),
  c_args: user_c_args, cpp_args: user_cpp_args, pic: false, build_by_default: false,
)
user_entry = static_library(
  'user-entry', files('user/crt/start.S'),
  c_args: user_c_args, pic: false, build_by_default: false,
)
exec = static_library('exec', files('user/lib/exec.cpp'),
  cpp_args: user_cpp_args, pic: false, build_by_default: false)
posix = static_library(
  'posix', files('adapters/posix/io.cpp'),
  cpp_args: user_cpp_args + ['-I' + join_paths(root, 'adapters/posix/include')],
  pic: false, build_by_default: false,
)
subdir('third_party/ftxui')
cxx_runtime = static_library('cxx-runtime', files('third_party/libstdcxx/tree.cc', 'third_party/libstdcxx/shared_ptr.cc'),
  cpp_args: user_cpp_args + ['-ffunction-sections', '-fdata-sections'],
  pic: false, build_by_default: false)
math_runtime = static_library('math-runtime', files(
  'third_party/musl/__math_invalid.c',
  'third_party/musl/__math_invalidf.c',
  'third_party/musl/__math_oflow.c',
  'third_party/musl/__math_oflowf.c',
  'third_party/musl/__math_uflow.c',
  'third_party/musl/__math_uflowf.c',
  'third_party/musl/__math_xflow.c',
  'third_party/musl/__math_xflowf.c',
  'third_party/musl/exp2f_data.c',
  'third_party/musl/exp_data.c',
  'third_party/musl/fabs.c',
  'third_party/musl/pow.c',
  'third_party/musl/pow_data.c',
  'third_party/musl/powf.c',
  'third_party/musl/powf_data.c',
), c_args: user_c_args + ['-ffunction-sections', '-fdata-sections', '-Wno-unused-but-set-variable'],
  pic: false, build_by_default: false)
tui = static_library('tui', files('adapters/ftxui/terminal.cpp'),
  include_directories: ftxui_inc, cpp_args: user_cpp_args,
  pic: false, build_by_default: false)
bins = {}
probes = {}
foreach name, spec : {
  'init': [files('servers/init.cpp'), false],
  'block': [files('servers/block.cpp'), false],
  'devices': [files('servers/devices.cpp'), false],
  'bootfs': [files('servers/bootfs.cpp'), false],
  'vfs': [files('servers/vfs.cpp'), false],
  'uart': [files('servers/uart.cpp'), false],
  'shell': [files('programs/shell.cpp'), false],
  'tasks': [files('programs/tasks.cpp'), false],
  'echo': [files('programs/echo.cpp'), true],
  'slow': [files('test/user/stream/slow.cpp'), true],
  'fill': [files('test/user/stream/fill.cpp'), true],
  'control-test': [files('test/user/control.cpp'), false],
  'cat': [files('programs/cat.cpp'), true],
  'ls': [files('programs/ls.cpp'), true],
  'cp': [files('programs/cp.cpp'), true],
  'fs': [files('programs/fs.cpp'), true],
  'edit': [files('programs/edit.cpp'), true],
  'sleep': [files('programs/sleep.cpp'), true],
}
  binary = executable(name + '.elf', spec[0],
    c_args: user_c_args, include_directories: name == 'tasks' ? ftxui_inc : [],
    cpp_args: user_cpp_args + (spec[1] ? ['-I' + join_paths(root, 'adapters/posix/include')] : []),
    link_whole: [crt, user_entry] + (spec[1] ? [posix] : []),
    link_with: (['init','shell'].contains(name) ? [exec] : []) + (name == 'tasks' ? [tui, ftxui, cxx_runtime, math_runtime] : []),
    link_args: user_link_args + (name == 'tasks' ? ['-Wl,--gc-sections'] : []) + ['-Wl,-Map,' + join_paths(meson.current_build_dir(), name + '.map')],
    link_language: 'c', build_by_default: false)
  if ['fill','slow','control-test'].contains(name)
    probes += {name: binary}
  else
    bins += {name: binary}
  endif
endforeach


store_user = executable(
  'store.elf', files('servers/store.cpp', 'servers/lfs.c',
    'third_party/littlefs/lfs_util.c'),
  c_args: user_c_args + ['-I' + join_paths(root, 'user/include'),
    '-DLFS_NO_MALLOC', '-DLFS_NO_DEBUG', '-DLFS_NO_WARN',
    '-DLFS_NO_ERROR', '-UNDEBUG'],
  cpp_args: user_cpp_args + ['-I' + join_paths(root, 'user/include'),
    '-DLFS_NO_MALLOC', '-DLFS_NO_DEBUG', '-DLFS_NO_WARN',
    '-DLFS_NO_ERROR', '-UNDEBUG'],
  link_whole: [crt, user_entry], link_args: user_link_args,
  link_language: 'c', build_by_default: false,
)





fdcheck_user = executable(
  'fdcheck.elf', files('test/user/fd/main.cpp'), c_args: user_c_args,
  cpp_args: user_cpp_args + ['-I' + join_paths(root, 'adapters/posix/include')],
  link_whole: [crt, user_entry, posix], link_args: user_link_args, link_language: 'c', build_by_default: false,
)
demand_user = executable(
  'demand-test.elf', files('test/user/demand/main.cpp'), c_args: user_c_args, cpp_args: user_cpp_args,
  link_whole: [crt, user_entry], link_args: user_link_args, link_language: 'c', build_by_default: false,
)

initramfs = executable(
  'initramfs', files('tools/initramfs.cpp'), native: true,
  cpp_args: ['-std=c++23', '-I' + root, '-idirafter' + join_paths(root, 'user/include')], build_by_default: false,
)

shared_ring_test = executable(
  'shared-ring-test', files('test/shared_ring_test.cpp'), native: true,
  cpp_args: ['-std=c++23', '-O2', '-Wall', '-Wextra', '-I' + root],
  dependencies: dependency('threads', native: true), build_by_default: false,
)
run_target('check-shared-ring', command: [shared_ring_test])
io_queue_test = executable(
  'io-queue-test', files('test/io_queue_test.cpp'), native: true,
  cpp_args: ['-std=c++23', '-O2', '-Wall', '-Wextra', '-I' + root, '-idirafter' + join_paths(root, 'user/include')], build_by_default: false,
)
run_target('check-io-queue', command: [io_queue_test])

syscall_dependency_test = executable(
  'syscall-dependency-test', files('test/host/syscall_dependency_test.cpp'), native: true,
  cpp_args: ['-std=c++23', '-I' + root, '-idirafter' + join_paths(root, 'user/include')], build_by_default: false,
)

run_target('test-syscall-dependency', command: [syscall_dependency_test])










probe_disk = custom_target('probe-disk', output: 'probe.img',
  command: [find_program('python3'), '-c', 'import pathlib,sys; f=pathlib.Path(sys.argv[1]).open("wb"); f.truncate(1024*1024); f.close()', '@OUTPUT@'], build_by_default: false)

channel_init_user = executable(
  'channel-init.elf', files('test/user/channel/init.cpp'),
  c_args: user_c_args, cpp_args: user_cpp_args,
  link_with: exec, link_whole: [crt, user_entry], link_args: user_link_args, link_language: 'c', build_by_default: false,
)


channel_test_user = executable(
  'channel-test.elf', files('test/user/channel/main.cpp'),
  c_args: user_c_args, cpp_args: user_cpp_args,
  link_with: exec, link_whole: [crt, user_entry], link_args: user_link_args, link_language: 'c', build_by_default: false,
)
channel_worker_user = executable(
  'channel-worker.elf', files('test/user/channel/worker.cpp'),
  c_args: user_c_args, cpp_args: user_cpp_args,
  link_whole: [crt, user_entry], link_args: user_link_args, link_language: 'c', build_by_default: false,
)
channel_provider_user = executable(
  'channel-provider.elf', files('test/user/channel/provider.cpp'),
  c_args: user_c_args, cpp_args: user_cpp_args,
  link_whole: [crt, user_entry], link_args: user_link_args, link_language: 'c', build_by_default: false,
)
channel_holder_user = executable(
  'channel-holder.elf', files('test/user/channel/export_holder.cpp'),
  c_args: user_c_args, cpp_args: user_cpp_args,
  link_whole: [crt, user_entry], link_args: user_link_args, link_language: 'c', build_by_default: false,
)

process_client_user = executable(
  'process-client.elf', files('test/user/process/main.cpp'),
  c_args: user_c_args, cpp_args: user_cpp_args,
  link_with: exec, link_whole: [crt, user_entry], link_args: user_link_args, link_language: 'c', build_by_default: false,
)

exec_test = executable('exec-test.elf', files('test/user/exec.cpp'),
  c_args: user_c_args, cpp_args: user_cpp_args,
  link_whole: [crt, user_entry], link_args: user_link_args, link_language: 'c', build_by_default: false)
fault_shell_user = executable('fault-shell.elf', files('test/user/service_start/fault_shell.cpp'),
  c_args: user_c_args, cpp_args: user_cpp_args,
  link_with: exec, link_whole: [crt, user_entry], link_args: user_link_args, link_language: 'c', build_by_default: false)

# Debug ELF stays beside the objects; deployment consumes ordinary stripped ELF.
objcopy = find_program('riscv64-unknown-elf-objcopy')
images = {}
foreach name, elf : bins + probes + {
  'store': store_user, 'fdcheck': fdcheck_user, 'demand': demand_user,
  'process-client': process_client_user, 'cow': exec_test, 'fault-shell': fault_shell_user,
  'channel-init': channel_init_user, 'channel-test': channel_test_user,
  'channel-worker': channel_worker_user, 'channel-provider': channel_provider_user,
  'channel-holder': channel_holder_user,
}
  images += {name: custom_target(name + '-runtime', input: elf,
    output: name + '.run.elf', command: [objcopy, '--strip-debug', '@INPUT@', '@OUTPUT@'],
    build_by_default: false)}
endforeach

apps = {'boot/README.TXT': files('programs/README.TXT')[0],
        'boot/touch.elf': images['fs'], 'boot/mkdir.elf': images['fs'],
        'boot/rm.elf': images['fs'], 'boot/mv.elf': images['fs']}
test_apps = {'boot/fdcheck.elf': images['fdcheck'], 'boot/demand.elf': images['demand'],
             'boot/fill.elf': images['fill'], 'boot/slow.elf': images['slow'], 'boot/control-test.elf': images['control-test']}
foreach name, app : bins
  if not ['init','block','devices','bootfs','vfs','uart','shell'].contains(name)
    apps += {'boot/' + name + '.elf': images[name]}
  endif
endforeach
base = apps + {'init': images['init'], 'uart': images['uart'], 'block': images['block'],
               'bootfs': images['bootfs'], 'vfs': images['vfs'], 'devices': images['devices']}
bundles = {}
foreach name, layout : {
  'boot': base + {'shell': images['shell']},
  'storage': base + {'shell': images['shell'], 'store': images['store']},
  'storage-test': base + test_apps + {'shell': images['shell'], 'store': images['store']},
  'exec': base + test_apps + {'shell': images['shell'], 'boot/cow.elf': images['cow']},
  'process-test': base + {'shell': images['process-client']},
  'spontaneous-fault': base + {'shell': images['fault-shell']},
  'channel-test': {'init': images['channel-init'], 'channel-test': images['channel-test'],
    'writer': images['channel-worker'], 'provider': images['channel-provider'],
    'export-holder': images['channel-holder']},
}
  inputs = []
  paths = []
  foreach path, source : layout
    paths += path + '=@INPUT' + inputs.length().to_string() + '@'
    inputs += [source]
  endforeach
  bundles += {name: custom_target(name + '-bundle', input: inputs, output: name + '.bundle',
    command: [initramfs, '@OUTPUT@'] + paths, build_by_default: false)}
endforeach
boot_bundle = bundles['boot']
storage_bundle = bundles['storage']
storage_test_bundle = bundles['storage-test']
exec_bundle = bundles['exec']
process_test_bundle = bundles['process-test']
spontaneous_fault_bundle = bundles['spontaneous-fault']
channel_test_bundle = bundles['channel-test']

nm = find_program('riscv64-unknown-elf-nm')
ar = find_program('riscv64-unknown-elf-ar')
readelf = find_program('riscv64-unknown-elf-readelf')
clangxx = find_program('clang++')
audit_symbols = files('tools/build/audit-symbols.sh')
audit_stack = files('tools/build/audit-boot-stack.sh')
audit_user = files('tools/build/audit-user.sh')
audit_clang = files('tools/build/audit-clang.sh')
run_qemu = files('tools/build/run-qemu.sh')
artifact = files('tools/build/artifact.sh')

run_target('audit-kernel-symbols', command: [sh, audit_symbols, nm, ar, kernel, 'none', arch, core, entry, trace, sync, panic])
run_target('audit-symbols', command: [sh, audit_symbols, nm, ar, kernel_debug, 'any', arch, core, test_entry, trace, sync, panic, test_configs['debug'], test_module])
run_target('audit-kernel-stack', command: [sh, audit_stack, meson.current_build_dir(), '1792', kernel])
run_target('audit-boot-stack', command: [sh, audit_stack, meson.current_build_dir(), '2048', kernel_debug])
run_target('audit-user', command: [sh, audit_user, nm, readelf, bins['init'], bins['shell'], bins['echo'], bins['cat'], bins['uart'], bins['devices'], bins['block'], bins['bootfs'], bins['vfs'], bins['ls'], bins['cp'], bins['fs'], bins['edit'], bins['tasks'], bins['sleep'], store_user])
run_target('audit-clang', command: [sh, audit_clang, clangxx, root, (trace_enabled ? '1' : '0')])
objdump = find_program('riscv64-unknown-elf-objdump')
run_target('disasm', command: [sh, artifact, objdump, 'disasm', kernel, join_paths(meson.current_build_dir(), 'kernel.disasm')])
run_target('symbols', command: [sh, artifact, nm, 'symbols', kernel, join_paths(meson.current_build_dir(), 'kernel.sym')])
run_target('debug-disasm', command: [sh, artifact, objdump, 'disasm', kernel_debug, join_paths(meson.current_build_dir(), 'kernel_debug.disasm')])
run_target('debug-symbols', command: [sh, artifact, nm, 'symbols', kernel_debug, join_paths(meson.current_build_dir(), 'kernel_debug.sym')])

qemu = find_program('qemu-system-riscv64')

run_target('run-process-matrix', command: [find_program('python3'), files('tools/build/run-io-platform.py'),
  qemu, kernel_debug, '1,2,4', '--bundle', process_test_bundle, '--timeout', '45',
  '[process] direct spawn, retention, deadlines and reuse ok'])



timeout = get_option('qemu_timeout')
smp = get_option('qemu_smp').to_string()
run_target('run', command: [qemu, '-machine', 'virt', '-nographic', '-bios', 'default', '-kernel', kernel])
run_target('run-timeout', command: [sh, run_qemu, qemu, '3s', '1', kernel, 'timeout'])
run_target('run-console', command: [find_program('python3'),
  files('tools/build/run-console-storage.py'), qemu, kernel, storage_bundle,
  join_paths(meson.current_build_dir(), 'data.img'), smp],
  depends: [kernel, storage_bundle])
run_target('run-exec', command: [find_program('python3'), files('tools/build/run-console.py'),
  qemu, kernel_debug, exec_bundle, '4', 'exec', '1'])
run_target('run-interactive', command: [find_program('python3'), files('tools/build/run-console.py'),
  qemu, kernel, storage_test_bundle, smp, 'interactive', '1'])
run_target('run-storage-persistence', command: [find_program('python3'),
  files('tools/build/run-storage-persistence.py'), qemu, kernel_debug,
  storage_test_bundle, '1,2,4'], depends: [kernel_debug, storage_test_bundle])
run_target('run-debug', command: [qemu, '-machine', 'virt', '-nographic', '-bios', 'default', '-kernel', kernel_debug])
run_target('debug', command: [qemu, '-machine', 'virt', '-nographic', '-bios', 'default', '-kernel', kernel_debug, '-S', '-gdb', 'tcp:' + get_option('gdb_host') + ':' + get_option('gdb_port').to_string()])
run_target('run-wait-test-smp', command: [sh, run_qemu, qemu, timeout, smp, kernel_wait, 'marker',
  '--exact-failed-zero', '--forbid', 'MYOS KERNEL PANIC', '--stop-when', '[scenario] wait publication ok',
  '[scenario] wait publication ok'])
run_target('run-wait-test-dual', command: [sh, run_qemu, qemu, timeout, '2', kernel_wait, 'marker',
  '--exact-failed-zero', '--forbid', 'MYOS KERNEL PANIC', '--stop-when', '[scenario] wait publication ok',
  '[scenario] pending VM wait completed after remote shootdown', '[scenario] wait publication ok'])

run_target('run-channel-matrix', command: [find_program('python3'), files('tools/build/run-io-platform.py'),
  qemu, kernel_debug, '1,2,4', '--bundle', channel_test_bundle, '--timeout', '30', '--report', '[channel]',
  '[channel] service generations, stale cap denied, blocked senders, stop and reuse ok'],
  depends: [kernel_debug, channel_test_bundle])


run_target('run-io-dual-console-matrix', command: [find_program('python3'),
  files('tools/build/run-io-platform.py'), qemu, kernel, '1,2,4',
  '--bundle', boot_bundle, '--second-disk', '--timeout', '18', '/ > '],
  depends: [kernel, boot_bundle])
run_target('run-test-smp', command: [sh, run_qemu, qemu, timeout, smp, kernel_debug, 'timeout', '--exact-failed-zero', 'cpu: online=' + smp])
if get_option('panic_probe') == 1
  run_target('run-panic-smp', command: [sh, run_qemu, qemu, timeout, smp, kernel_debug, 'shutdown', '--forbid', 'no acknowledgement', '--count-regex', 'cpu [0-9]+: stopped pc=', (get_option('qemu_smp') - 1).to_string(), 'MYOS KERNEL PANIC', 'context: call-site', 'expression: false'])
elif get_option('panic_probe') == 2
  run_target('run-panic-degraded-smp', command: [sh, run_qemu, qemu, timeout, smp, kernel_debug, 'shutdown', 'MYOS KERNEL PANIC', 'no acknowledgement'])
endif
scenario_marker = ''
if scenario_name == 'ordinary'
  scenario_marker = '[scenario] ordinary ok'
elif scenario_name == 'initrd'
  scenario_marker = '[scenario] initrd ok'
elif scenario_name == 'trap'
  scenario_marker = '[scenario] trap ok'
elif scenario_name == 'remote'
  scenario_marker = '[scenario] remote-delivery ok'
elif scenario_name == 'dispatch'
  scenario_marker = '[scenario] dispatch ok'
endif
scenario_args_qemu = [sh, run_qemu, qemu, timeout, smp, kernel_debug, 'timeout']
if scenario_marker != ''
  scenario_args_qemu += [scenario_marker]
endif
if scenario_name == 'initrd'
  scenario_args_qemu += ['--initrd', boot_bundle, '--disk', probe_disk]
endif
if scenario_name != 'off'
  run_target('run-scenario', command: scenario_args_qemu)
endif

alias_target('kernel', kernel)
alias_target('kernel-debug', kernel_debug)
alias_target('bundle', [boot_bundle, bins['uart']])
