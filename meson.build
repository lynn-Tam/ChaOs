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
platform_sources = files(
  'platform/dma.cpp',
  'platform/boot.cpp',
)

core_kernel_sources = files(
  'kernel/io/space.cpp', 'kernel/mm/table.cpp',
  'kernel/boot/root.cpp',
  'kernel/cap/view.cpp',  'kernel/cap/grant.cpp',
  'kernel/cap/cspace.cpp', 'kernel/ipc/notification.cpp', 'kernel/ipc/buffer.cpp', 'kernel/ipc/channel.cpp', 'kernel/mm/pager.cpp',
  'kernel/irq/irq.cpp', 'kernel/ipc/endpoint.cpp',
  'kernel/object/pool.cpp',
  'kernel/sched/sc.cpp',
  'kernel/sched/domain.cpp',
  'kernel/sched/dispatcher.cpp',
  'kernel/task/thread.cpp',
  'kernel/wait.cpp',
  'kernel/task/env.cpp',
  'kernel/syscall/call.cpp', 'kernel/syscall/create.cpp',
  'kernel/syscall/ipc.cpp',
  'kernel/task/exit.cpp',
  'kernel/mm/mem.cpp', 'kernel/mm/vspace.cpp',
  'kernel/mm/kspace.cpp', 'kernel/mm/tlb.cpp',
  'kernel/mm/pmm.cpp',
  'libk/mem.c',
)

arch = static_library('kernel-arch', arch_sources, c_args: arch_common_args, cpp_args: arch_cpp_args, pic: false)
core = static_library('kernel-core', platform_sources + core_kernel_sources, c_args: kernel_c_args, cpp_args: kernel_cpp_args, pic: false)
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
  '-march=rv64imac_zicsr_zifencei', '-mabi=lp64', '-mcmodel=medany', '-msmall-data-limit=0', '-I' + root, '-I' + join_paths(root, 'user/libsys'), '-I' + join_paths(root, 'user/include'),
]
user_c_args = user_common_args + ['-ffreestanding']
user_cpp_args = user_common_args + ['-fno-exceptions', '-fno-rtti', '-fno-threadsafe-statics', '-fno-use-cxa-atexit']
# Container headers need the target C declarations; only used runtime symbols link.
user_cpp_args += [cpp_config_arg, '-idirafter', get_option('newlib_include')]
user_link_args = ['-march=rv64imac_zicsr_zifencei', '-mabi=lp64', '-nostdlib', '-lgcc', '-Wl,-T,' + join_paths(root, 'user/riscv64/linker.ld')]
crt = static_library(
  'crt', files('user/crt/init.cpp', 'user/crt/start.cpp', 'user/crt/heap.cpp', 'libk/mem.c'),
  c_args: user_c_args, cpp_args: user_cpp_args, pic: false, build_by_default: false,
)
user_entry = static_library(
  'user-entry', files('user/crt/start.S'),
  c_args: user_c_args, pic: false, build_by_default: false,
)
libsys = static_library(
  'sys', files('user/libsys/runtime.cpp'),
  cpp_args: user_cpp_args + ['-I' + join_paths(root, 'user/libsys')],
  pic: false, build_by_default: false,
)
bins = {}
foreach name, spec : {
  'init': [files('servers/init.cpp'), false],
  'block': [files('servers/block.cpp'), false],
  'files': [files('servers/files.cpp'), false],
  'vfs': [files('servers/vfs.cpp'), false],
  'uart': [files('servers/uart.cpp'), false],
  'shell': [files('programs/shell.cpp'), false],
  'echo': [files('programs/echo.cpp'), true],
  'slow': [files('programs/slow.cpp'), true],
  'fill': [files('programs/fill.cpp'), true],
  'cat': [files('programs/cat.cpp'), true],
  'ls': [files('programs/ls.cpp'), true],
  'put': [files('programs/put.cpp'), true],
  'cp': [files('programs/cp.cpp'), true],
  'fs': [files('programs/fs.cpp'), true],
  'edit': [files('programs/edit.cpp'), true],
  'sleep': [files('programs/sleep.cpp'), true],
}
  bins += {name: executable(name + '.elf', spec[0],
    c_args: user_c_args, cpp_args: user_cpp_args,
    link_whole: [crt, user_entry] + (spec[1] ? [libsys] : []),
    link_args: user_link_args + ['-Wl,-Map,' + join_paths(meson.current_build_dir(), name + '.map')],
    link_language: 'c', build_by_default: false)}
endforeach
io_init_user = executable(
  'io-init-test.elf', files('test/user/io/init.cpp'), c_args: user_c_args, cpp_args: user_cpp_args,
  link_whole: [crt, user_entry], link_args: user_link_args, link_language: 'c', build_by_default: false,
)
io_worker_user = executable(
  'io-worker-test.elf', files('test/user/io/worker.cpp'), c_args: user_c_args, cpp_args: user_cpp_args,
  link_whole: [crt, user_entry], link_args: user_link_args, link_language: 'c', build_by_default: false,
)
store_user = executable(
  'store.elf', files('servers/store.cpp', 'servers/lfs.c',
    'third_party/littlefs/lfs_util.c'),
  c_args: user_c_args + ['-I' + join_paths(root, 'user/libsys'), '-I' + join_paths(root, 'user/include'),
    '-DLFS_NO_MALLOC', '-DLFS_NO_DEBUG', '-DLFS_NO_WARN',
    '-DLFS_NO_ERROR', '-UNDEBUG'],
  cpp_args: user_cpp_args + ['-I' + join_paths(root, 'user/libsys'), '-I' + join_paths(root, 'user/include'),
    '-DLFS_NO_MALLOC', '-DLFS_NO_DEBUG', '-DLFS_NO_WARN',
    '-DLFS_NO_ERROR', '-UNDEBUG'],
  link_whole: [crt, user_entry], link_args: user_link_args,
  link_language: 'c', build_by_default: false,
)
io_session_init_user = executable(
  'io-session-init-test.elf', files('test/user/io/session_init.cpp'), c_args: user_c_args, cpp_args: user_cpp_args,
  link_whole: [crt, user_entry], link_args: user_link_args, link_language: 'c', build_by_default: false,
)
io_client_user = executable(
  'io-client-test.elf', files('test/user/io/client.cpp'), c_args: user_c_args, cpp_args: user_cpp_args,
  link_whole: [crt, user_entry], link_args: user_link_args, link_language: 'c', build_by_default: false,
)
block_write_user = executable(
  'block-write-test.elf', files('test/user/io/write_client.cpp'), c_args: user_c_args, cpp_args: user_cpp_args,
  link_whole: [crt, user_entry], link_args: user_link_args, link_language: 'c', build_by_default: false,
)
block_verify_user = executable(
  'block-verify-test.elf', files('test/user/io/write_client.cpp'), c_args: user_c_args,
  cpp_args: user_cpp_args + ['-DVERIFY_ONLY'],
  link_whole: [crt, user_entry], link_args: user_link_args, link_language: 'c', build_by_default: false,
)
file_init_user = executable(
  'file-init-test.elf', files('test/user/io/file_init.cpp'), c_args: user_c_args, cpp_args: user_cpp_args,
  link_whole: [crt, user_entry], link_args: user_link_args, link_language: 'c', build_by_default: false,
)
file_client_user = executable(
  'file-client-test.elf', files('test/user/io/file_client.cpp'), c_args: user_c_args, cpp_args: user_cpp_args,
  link_whole: [crt, user_entry], link_args: user_link_args, link_language: 'c', build_by_default: false,
)
file_failure_init_user = executable(
  'file-failure-init-test.elf', files('test/user/io/file_init.cpp'), c_args: user_c_args,
  cpp_args: user_cpp_args + ['-DTEST_FILE_FAILURE'],
  link_whole: [crt, user_entry], link_args: user_link_args, link_language: 'c', build_by_default: false,
)
file_failure_user = executable(
  'file-failure-test.elf', files('test/user/io/file_failure.cpp'), c_args: user_c_args, cpp_args: user_cpp_args,
  link_whole: [crt, user_entry], link_args: user_link_args, link_language: 'c', build_by_default: false,
)

fdcheck_user = executable(
  'fdcheck.elf', files('test/user/fd/main.cpp'), c_args: user_c_args,
  cpp_args: user_cpp_args + ['-I' + join_paths(root, 'user/libsys')],
  link_whole: [crt, user_entry, libsys], link_args: user_link_args, link_language: 'c', build_by_default: false,
)
demand_user = executable(
  'demand-test.elf', files('test/user/demand/main.cpp'), c_args: user_c_args, cpp_args: user_cpp_args,
  link_whole: [crt, user_entry], link_args: user_link_args, link_language: 'c', build_by_default: false,
)

bootpack = executable(
  'bootpack', files('tools/bootpack/main.cpp'), native: true,
  cpp_args: ['-std=c++23', '-I' + root, '-idirafter' + join_paths(root, 'user/libsys'), '-idirafter' + join_paths(root, 'user/include')], build_by_default: false,
)

shared_ring_test = executable(
  'shared-ring-test', files('test/shared_ring_test.cpp'), native: true,
  cpp_args: ['-std=c++23', '-O2', '-Wall', '-Wextra', '-I' + root],
  dependencies: dependency('threads', native: true), build_by_default: false,
)
run_target('check-shared-ring', command: [shared_ring_test])
io_queue_test = executable(
  'io-queue-test', files('test/io_queue_test.cpp'), native: true,
  cpp_args: ['-std=c++23', '-O2', '-Wall', '-Wextra', '-I' + root, '-idirafter' + join_paths(root, 'user/libsys'), '-idirafter' + join_paths(root, 'user/include')], build_by_default: false,
)
run_target('check-io-queue', command: [io_queue_test])
fat32_test = executable(
  'fat32-test', files('test/fat32_test.cpp'), native: true,
  cpp_args: ['-std=c++23', '-O2', '-Wall', '-Wextra', '-I' + root, '-idirafter' + join_paths(root, 'user/libsys'), '-idirafter' + join_paths(root, 'user/include')], build_by_default: false,
)
run_target('check-fat32', command: [fat32_test])

boot_bundle_test = executable(
  'boot-bundle-test', files('test/host/boot_bundle_test.cpp'), native: true,
  cpp_args: ['-std=c++23', '-I' + root, '-idirafter' + join_paths(root, 'user/libsys'), '-idirafter' + join_paths(root, 'user/include')], build_by_default: false,
)
syscall_dependency_test = executable(
  'syscall-dependency-test', files('test/host/syscall_dependency_test.cpp'), native: true,
  cpp_args: ['-std=c++23', '-I' + root, '-idirafter' + join_paths(root, 'user/libsys'), '-idirafter' + join_paths(root, 'user/include')], build_by_default: false,
)

run_target('test-boot-bundle', command: [boot_bundle_test])

run_target('test-syscall-dependency', command: [syscall_dependency_test])

io_user_bundle = custom_target(
  'io-user-bundle', input: [io_init_user, io_worker_user], output: 'io-user.bundle',
  command: [bootpack, '@OUTPUT@', 'init', '--boot', 'init=@INPUT0@', 'io-test=@INPUT1@'],
  build_by_default: false,
)
io_dual_init_user = executable(
  'io-dual-init.elf', files('test/user/io/dual_init.cpp'),
  c_args: user_c_args, cpp_args: user_cpp_args,
  link_whole: [crt, user_entry], link_args: user_link_args, link_language: 'c', build_by_default: false,
)
io_dual_bundle = custom_target(
  'io-dual-bundle', input: [io_dual_init_user, io_worker_user],
  output: 'io-dual.bundle',
  command: [bootpack, '@OUTPUT@', 'init', '--boot', 'init=@INPUT0@', 'io-test=@INPUT1@'],
  build_by_default: false,
)

io_session_bundle = custom_target(
  'io-session-bundle', input: [io_session_init_user, bins['block'], io_client_user],
  output: 'io-session.bundle',
  command: [bootpack, '@OUTPUT@', 'init', '--boot', 'init=@INPUT0@', 'block=@INPUT1@', 'io-client=@INPUT2@'],
  build_by_default: false,
)

block_write_bundle = custom_target(
  'block-write-bundle', input: [io_session_init_user, bins['block'], block_write_user],
  output: 'block-write.bundle',
  command: [bootpack, '@OUTPUT@', 'init', '--boot', 'init=@INPUT0@', 'block=@INPUT1@', 'io-client=@INPUT2@'],
  build_by_default: false,
)
block_verify_bundle = custom_target(
  'block-verify-bundle', input: [io_session_init_user, bins['block'], block_verify_user],
  output: 'block-verify.bundle',
  command: [bootpack, '@OUTPUT@', 'init', '--boot', 'init=@INPUT0@', 'block=@INPUT1@', 'io-client=@INPUT2@'],
  build_by_default: false,
)

echo_package = custom_target(
  'echo-package', input: [bins['echo']], output: 'echo.pkg',
  command: [bootpack, '@OUTPUT@', 'echo', 'echo=@INPUT0@'], build_by_default: false,
)
run_target('test-bootpack-image', command: [find_program('python3'),
  files('test/tools/bootpack_image.py'), bins['echo'], echo_package])

slow_package = custom_target(
  'slow-package', input: [bins['slow']], output: 'slow.pkg',
  command: [bootpack, '@OUTPUT@', 'slow', 'slow=@INPUT0@'], build_by_default: false,
)

fill_package = custom_target(
  'fill-package', input: [bins['fill']], output: 'fill.pkg',
  command: [bootpack, '@OUTPUT@', 'fill', 'fill=@INPUT0@'], build_by_default: false,
)

cat_package = custom_target(
  'cat-package', input: [bins['cat']], output: 'cat.pkg',
  command: [bootpack, '@OUTPUT@', 'cat', 'cat=@INPUT0@'], build_by_default: false,
)

ls_package = custom_target(
  'ls-package', input: [bins['ls']], output: 'ls.pkg',
  command: [bootpack, '@OUTPUT@', 'ls', 'ls=@INPUT0@'], build_by_default: false,
)

put_package = custom_target(
  'put-package', input: [bins['put']], output: 'put.pkg',
  command: [bootpack, '@OUTPUT@', 'put', 'put=@INPUT0@'], build_by_default: false,
)

get_package = custom_target(
  'get-package', input: [bins['cat']], output: 'get.pkg',
  command: [bootpack, '@OUTPUT@', 'get', 'get=@INPUT0@'], build_by_default: false,
)

cp_package = custom_target(
  'cp-package', input: [bins['cp']], output: 'cp.pkg',
  command: [bootpack, '@OUTPUT@', 'cp', 'cp=@INPUT0@'], build_by_default: false,
)

fdcheck_package = custom_target(
  'fdcheck-package', input: [fdcheck_user], output: 'fdcheck.pkg',
  command: [bootpack, '@OUTPUT@', 'fdcheck', 'fdcheck=@INPUT0@'], build_by_default: false,
)

fs_package = custom_target(
  'fs-package', input: [bins['fs']], output: 'fs.pkg',
  command: [bootpack, '@OUTPUT@', 'fs', 'fs=@INPUT0@'], build_by_default: false,
)

edit_package = custom_target(
  'edit-package', input: [bins['edit']], output: 'edit.pkg',
  command: [bootpack, '@OUTPUT@', 'edit', 'edit=@INPUT0@'], build_by_default: false,
)



sleep_package = custom_target(
  'sleep-package', input: [bins['sleep']], output: 'sleep.pkg',
  command: [bootpack, '@OUTPUT@', 'sleep', 'sleep=@INPUT0@'], build_by_default: false,
)

demand_package = custom_target(
  'demand-package', input: [demand_user], output: 'demand.pkg',
  command: [bootpack, '@OUTPUT@', 'demand', 'demand=@INPUT0@'], build_by_default: false,
)

disk_entries = {
  'README.TXT': files('user/disk/README.TXT')[0],
  'ECHO.PKG': echo_package, 'SLEEP.PKG': sleep_package,
  'CAT.PKG': cat_package, 'LS.PKG': ls_package,
  'SLOW.PKG': slow_package, 'FILL.PKG': fill_package,
  'PUT.PKG': put_package, 'GET.PKG': get_package, 'CP.PKG': cp_package,
  'FS.PKG': fs_package, 'EDIT.PKG': edit_package,
}
disk_inputs = []
disk_args = [find_program('python3'), files('tools/build/fat-image.py'), '@OUTPUT@']
foreach name, source : disk_entries
  disk_args += name + '=@INPUT' + disk_inputs.length().to_string() + '@'
  disk_inputs += [source]
endforeach
disk_image = custom_target(
  'disk-image', input: disk_inputs, output: 'disk.img',
  command: disk_args, build_by_default: false,
)
foreach name, source : {'DEMAND.PKG': demand_package,
                        'FDCHECK.PKG': fdcheck_package}
  disk_args += name + '=@INPUT' + disk_inputs.length().to_string() + '@'
  disk_inputs += [source]
endforeach
test_disk = custom_target(
  'test-disk', input: disk_inputs, output: 'test-disk.img',
  command: disk_args + ['BAD.PKG=@INPUT0@'], build_by_default: false,
)
file_test_data = custom_target(
  'file-test-data', output: 'file-test.data',
  command: [find_program('python3'), '-c', 'import pathlib,sys; pathlib.Path(sys.argv[1]).write_bytes(bytes(i % 251 for i in range(16397)))', '@OUTPUT@'],
  build_by_default: false,
)
file_test_disk = custom_target(
  'file-test-disk', input: file_test_data, output: 'file-test.img',
  command: [find_program('python3'), files('test/tools/file_image.py'), files('tools/build/fat-image.py'), '@OUTPUT@', '@INPUT@'], build_by_default: false,
)

file_session_bundle = custom_target(
  'file-session-bundle', input: [file_init_user, bins['block'], bins['files'], file_client_user], output: 'file-session.bundle',
  command: [bootpack, '@OUTPUT@', 'init', '--boot', 'init=@INPUT0@', 'block=@INPUT1@', 'files=@INPUT2@', 'file-client=@INPUT3@'],
  build_by_default: false,
)
file_fault_user = executable(
  'file-fault.elf', files('test/user/io/file_fault.cpp'),
  c_args: user_c_args, cpp_args: user_cpp_args,
  link_whole: [crt, user_entry], link_args: user_link_args, link_language: 'c', build_by_default: false,
)
file_fault_init_user = executable(
  'file-fault-init.elf', files('test/user/io/file_fault_init.cpp'),
  c_args: user_c_args, cpp_args: user_cpp_args,
  link_whole: [crt, user_entry], link_args: user_link_args, link_language: 'c', build_by_default: false,
)

file_fault_bundle = custom_target(
  'file-fault-bundle', input: [file_fault_init_user, bins['block'], bins['files'], file_fault_user],
  output: 'file-fault.bundle',
  command: [bootpack, '@OUTPUT@', 'init', '--boot', 'init=@INPUT0@', 'block=@INPUT1@', 'files=@INPUT2@', 'file-client=@INPUT3@'], build_by_default: false,
)

file_failure_bundle = custom_target(
  'file-failure-bundle', input: [file_failure_init_user, bins['block'], bins['files'], file_failure_user],
  output: 'file-failure.bundle',
  command: [bootpack, '@OUTPUT@', 'init', '--boot', 'init=@INPUT0@', 'block=@INPUT1@', 'files=@INPUT2@', 'file-client=@INPUT3@'],
  build_by_default: false,
)

channel_init_user = executable(
  'channel-init.elf', files('test/user/channel/init.cpp'),
  c_args: user_c_args, cpp_args: user_cpp_args,
  link_whole: [crt, user_entry], link_args: user_link_args, link_language: 'c', build_by_default: false,
)
device_inventory_user = executable(
  'device-inventory.elf', files('test/user/io/device_inventory_init.cpp'),
  c_args: user_c_args, cpp_args: user_cpp_args,
  link_whole: [crt, user_entry], link_args: user_link_args, link_language: 'c', build_by_default: false,
)
device_inventory_bundle = custom_target(
  'device-inventory-bundle', input: device_inventory_user,
  output: 'device-inventory.bundle',
  command: [bootpack, '@OUTPUT@', 'init', '--boot', 'init=@INPUT0@'], build_by_default: false,
)
channel_test_user = executable(
  'channel-test.elf', files('test/user/channel/main.cpp'),
  c_args: user_c_args, cpp_args: user_cpp_args,
  link_whole: [crt, user_entry], link_args: user_link_args, link_language: 'c', build_by_default: false,
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

channel_test_bundle = custom_target(
  'channel-test-bundle', input: [channel_init_user, channel_test_user, channel_worker_user, channel_provider_user, channel_holder_user],
  output: 'channel-test.bundle',
  command: [bootpack, '@OUTPUT@', 'init', '--boot', 'init=@INPUT0@', 'channel-test=@INPUT1@', 'writer=@INPUT2@', 'provider=@INPUT3@', 'export-holder=@INPUT4@'], build_by_default: false,
)
process_client_user = executable(
  'process-client.elf', files('test/user/process/main.cpp'),
  c_args: user_c_args, cpp_args: user_cpp_args,
  link_whole: [crt, user_entry], link_args: user_link_args, link_language: 'c', build_by_default: false,
)

process_test_bundle = custom_target(
  'process-test-bundle', input: [bins['init'], bins['uart'], process_client_user, bins['block'], bins['files'], bins['vfs']],
  output: 'process-test.bundle',
  command: [bootpack, '@OUTPUT@', 'init', '--boot', 'init=@INPUT0@', 'uart=@INPUT1@', 'shell=@INPUT2@', 'block=@INPUT3@', 'files=@INPUT4@', 'vfs=@INPUT5@'], build_by_default: false,
)
boot_bundle = custom_target(
  'boot-bundle', input: [bins['init'], bins['uart'], bins['shell'], bins['block'], bins['files'], bins['vfs']], output: 'boot.bundle',
  command: [bootpack, '@OUTPUT@', 'init', '--boot', 'init=@INPUT0@', 'uart=@INPUT1@', 'shell=@INPUT2@', 'block=@INPUT3@', 'files=@INPUT4@', 'vfs=@INPUT5@'], build_by_default: false,
)
run_target('test-boot-load', command: [find_program('python3'),
  files('test/tools/bootpack_image.py'), bins['init'], boot_bundle])
storage_bundle = custom_target(
  'storage-bundle', input: [bins['init'], bins['uart'],
    bins['shell'], bins['block'], bins['files'], store_user, bins['vfs']],
  output: 'storage.bundle',
  command: [bootpack, '@OUTPUT@', 'init', '--boot', 'init=@INPUT0@', 'uart=@INPUT1@',
    'shell=@INPUT2@', 'block=@INPUT3@',
    'files=@INPUT4@', 'store=@INPUT5@', 'vfs=@INPUT6@'], build_by_default: false,
)
fault_shell_user = executable(
  'fault-shell.elf', files('test/user/service_start/fault_shell.cpp'),
  c_args: user_c_args, cpp_args: user_cpp_args,
  link_whole: [crt, user_entry], link_args: user_link_args, link_language: 'c', build_by_default: false,
)

spontaneous_fault_bundle = custom_target(
  'spontaneous-fault-bundle',
  input: [bins['init'], bins['uart'], fault_shell_user, bins['block'], bins['files'], bins['vfs']],
  output: 'spontaneous-fault.bundle',
  command: [bootpack, '@OUTPUT@', 'init', '--boot', 'init=@INPUT0@', 'uart=@INPUT1@',
    'shell=@INPUT2@', 'block=@INPUT3@', 'files=@INPUT4@', 'vfs=@INPUT5@'], build_by_default: false,
)

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
run_target('audit-user', command: [sh, audit_user, nm, readelf, bins['init'], bins['shell'], bins['echo'], bins['cat'], bins['uart'], bins['block'], bins['files']])
run_target('audit-clang', command: [sh, audit_clang, clangxx, root, (trace_enabled ? '1' : '0')])
objdump = find_program('riscv64-unknown-elf-objdump')
run_target('disasm', command: [sh, artifact, objdump, 'disasm', kernel, join_paths(meson.current_build_dir(), 'kernel.disasm')])
run_target('symbols', command: [sh, artifact, nm, 'symbols', kernel, join_paths(meson.current_build_dir(), 'kernel.sym')])
run_target('debug-disasm', command: [sh, artifact, objdump, 'disasm', kernel_debug, join_paths(meson.current_build_dir(), 'kernel_debug.disasm')])
run_target('debug-symbols', command: [sh, artifact, nm, 'symbols', kernel_debug, join_paths(meson.current_build_dir(), 'kernel_debug.sym')])

qemu = find_program('qemu-system-riscv64')
run_target('run-io-user-matrix', command: [find_program('python3'), files('tools/build/run-io-platform.py'),
  qemu, kernel_debug, '1,2,4', '--bundle', io_user_bundle,
  '[io-user] ok: 32 outstanding, batch completion, task teardown, device reuse'])
run_target('run-process-matrix', command: [find_program('python3'), files('tools/build/run-io-platform.py'),
  qemu, kernel_debug, '1,2,4', '--bundle', process_test_bundle, '--disk', disk_image, '--timeout', '45',
  '[process] direct spawn, retention, deadlines and reuse ok'])
run_target('run-file-session-matrix', command: [find_program('python3'), files('tools/build/run-io-platform.py'),
  qemu, kernel_debug, '1,2,4', '--bundle', file_session_bundle, '--disk', file_test_disk, '--timeout', '60',
  '[file-session] ok: two sessions, byte reads, 64 outstanding, EOF, handles, close'])
run_target('run-file-failure-matrix', command: [find_program('python3'), files('tools/build/run-io-platform.py'),
  qemu, kernel_debug, '1,2,4', '--bundle', file_failure_bundle, '--disk', file_test_disk,
  '--timeout', '20', '--fail-sector', '2081',
  '[file-failure] ok: read error, two faulted mappings, closed sessions, services live'])

run_target('run-io-session-matrix', command: [find_program('python3'), files('tools/build/run-io-platform.py'),
  qemu, kernel_debug, '1,2,4', '--bundle', io_session_bundle,
  '[io-session] ok: isolated pages, 32 outstanding, batched completion, cancel, close'])
run_target('run-block-write-matrix', command: [find_program('python3'), files('tools/build/run-block-write.py'),
  qemu, kernel_debug, block_write_bundle, block_verify_bundle, '1,2,4'],
  depends: [kernel_debug, block_write_bundle, block_verify_bundle])
timeout = get_option('qemu_timeout')
smp = get_option('qemu_smp').to_string()
run_target('run', command: [qemu, '-machine', 'virt', '-nographic', '-bios', 'default', '-kernel', kernel])
run_target('run-timeout', command: [sh, run_qemu, qemu, '3s', '1', kernel, 'timeout'])
run_target('run-console', command: [find_program('python3'),
  files('tools/build/run-console-storage.py'), qemu, kernel, storage_bundle,
  disk_image, join_paths(meson.current_build_dir(), 'data.img'), smp],
  depends: [kernel, storage_bundle, disk_image])
run_target('run-storage-persistence', command: [find_program('python3'),
  files('tools/build/run-storage-persistence.py'), qemu, kernel_debug,
  storage_bundle, test_disk, '1,2,4'], depends: [kernel_debug, storage_bundle, test_disk])
run_target('run-debug', command: [qemu, '-machine', 'virt', '-nographic', '-bios', 'default', '-kernel', kernel_debug])
run_target('debug', command: [qemu, '-machine', 'virt', '-nographic', '-bios', 'default', '-kernel', kernel_debug, '-S', '-gdb', 'tcp:' + get_option('gdb_host') + ':' + get_option('gdb_port').to_string()])
run_target('run-wait-test-smp', command: [sh, run_qemu, qemu, timeout, smp, kernel_wait, 'marker',
  '--exact-failed-zero', '--forbid', 'MYOS KERNEL PANIC', '--stop-when', '[scenario] wait publication ok',
  '[scenario] wait publication ok'])
run_target('run-wait-test-dual', command: [sh, run_qemu, qemu, timeout, '2', kernel_wait, 'marker',
  '--exact-failed-zero', '--forbid', 'MYOS KERNEL PANIC', '--stop-when', '[scenario] wait publication ok',
  '[scenario] pending VM wait completed after remote shootdown', '[scenario] wait publication ok'])
sharing_gdb = find_program('gdb-multiarch', required: false)
if sharing_gdb.found()
  run_target('run-file-fault-matrix', command: [find_program('python3'), files('test/tools/file_fault.py'),
    qemu, sharing_gdb, kernel_debug, file_fault_bundle, file_test_disk, '1,2,4'])
  run_target('run-application-sharing', command: [find_program('python3'), files('test/tools/application_sharing.py'),
  qemu, sharing_gdb, find_program('riscv64-unknown-elf-nm'),
  kernel_debug, boot_bundle, test_disk, demand_user])
endif
run_target('run-channel-matrix', command: [find_program('python3'), files('tools/build/run-io-platform.py'),
  qemu, kernel_debug, '1,2,4', '--bundle', channel_test_bundle, '--timeout', '30',
  '[channel] service generations, stale cap denied, blocked senders, stop and reuse ok'],
  depends: [kernel_debug, channel_test_bundle])
run_target('run-device-inventory-matrix', command: [find_program('python3'),
  files('tools/build/run-io-platform.py'), qemu, kernel_debug, '1,2,4',
  '--bundle', device_inventory_bundle, '--second-disk', '--timeout', '15',
  '[device-inventory] two requester grants, dynamic binding and cfg revocation'])
run_target('run-io-dual-matrix', command: [find_program('python3'),
  files('tools/build/run-io-platform.py'), qemu, kernel_debug, '1,2,4',
  '--bundle', io_dual_bundle, '--second-disk', '--timeout', '60',
  '[io-dual] two devices completed independent DMA and teardown'],
  depends: [kernel_debug, io_dual_bundle])
run_target('run-io-dual-console-matrix', command: [find_program('python3'),
  files('tools/build/run-io-platform.py'), qemu, kernel, '1,2,4',
  '--bundle', boot_bundle, '--disk', disk_image, '--second-disk', '--timeout', '18', 'myos> '],
  depends: [kernel, boot_bundle, disk_image])
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
  scenario_args_qemu += ['--initrd', boot_bundle, '--disk', disk_image]
endif
if scenario_name != 'off'
  run_target('run-scenario', command: scenario_args_qemu)
endif

alias_target('kernel', kernel)
alias_target('kernel-debug', kernel_debug)
alias_target('bundle', [boot_bundle, bins['uart']])
