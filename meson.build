project(
  'os',
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
  '-DTRACE_ENABLED=' + (trace_enabled ? '1' : '0'),
  '-march=rv64imac_zicsr_zifencei', '-mabi=lp64', '-mcmodel=medany', '-msmall-data-limit=0',
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
  '-march=rv64imac_zicsr_zifencei', '-mabi=lp64', '-nostdlib', '-lgcc',
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
  'kernel/object/scope.cpp', 'kernel/resource/quota.cpp',
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
  c_args: kernel_c_args, cpp_args: kernel_cpp_args, pic: false)
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
# Export only mechanism headers. Application/private trees never enter a
# userspace include root; Meson's implicit repository root is disabled below.
foreach layer, directories : {'base': ['uapi', 'libk'], 'ubus': ['ubus'], 'mgr': ['mgr']}
  exported = join_paths(meson.current_build_dir(), 'include', layer)
  run_command('mkdir', '-p', exported, check: true)
  foreach directory : directories
    run_command('ln', '-sfn', join_paths(root, directory), join_paths(exported, directory), check: true)
  endforeach
endforeach
user_exports = join_paths(meson.current_build_dir(), 'include')
user_common_args = [
  '-Wall', '-Wextra', '-Werror', '-O2', '-g3', '-ffunction-sections', '-fdata-sections',
  '-march=rv64imac_zicsr_zifencei', '-mabi=lp64', '-mcmodel=medany', '-msmall-data-limit=0', '-I' + join_paths(user_exports, 'base'), '-I' + join_paths(root, 'lib/rt'),
]
user_c_args = user_common_args + ['-ffreestanding', '-idirafter', get_option('newlib_include')]
user_cxx_config = join_paths(meson.current_build_dir(), 'include/cxx')
run_command(find_program('python3'), files('tools/build/cxx-config.py'), cpp_config_dir,
  user_cxx_config, files('third_party/libstdcxx/gthr.h'), check: true)
user_cpp_args = user_common_args + ['-fno-exceptions', '-fno-rtti']
# Container headers need the target C declarations; only used runtime symbols link.
user_cpp_args += ['-I' + user_cxx_config, cpp_config_arg, '-idirafter', get_option('newlib_include')]
user_link_args = ['-Wl,--gc-sections', '-march=rv64imac_zicsr_zifencei', '-mabi=lp64', '-nostdlib', '-lgcc', '-Wl,-T,' + join_paths(root, 'lib/rt/linker.ld')]
crt = static_library(
  'crt', files('lib/rt/start.cpp', 'lib/rt/heap.cpp', 'lib/rt/sync.cpp', 'lib/rt/thread.cpp', 'libk/mem.c'),
  c_args: user_c_args, cpp_args: user_cpp_args, pic: false, implicit_include_directories: false, build_by_default: false,
)
user_entry = static_library(
  'user-entry', files('lib/rt/start.S'),
  c_args: user_c_args, pic: false, implicit_include_directories: false, build_by_default: false,
)
proc = static_library('proc', files('lib/proc/proc.cpp'),
  cpp_args: user_cpp_args + ['-I' + join_paths(root, 'lib/proc')], pic: false, implicit_include_directories: false, build_by_default: false)
proc_dep = declare_dependency(link_with: proc, include_directories: include_directories('lib/proc'))
ubus = static_library('ubus', files('ubus/value.cpp', 'ubus/ubus.cpp'),
  cpp_args: user_cpp_args, pic: false, implicit_include_directories: false, build_by_default: false)
ubus_dep = declare_dependency(link_with: ubus, compile_args: ['-I' + join_paths(user_exports, 'ubus')])
mgr = static_library('mgr', files('mgr/mgr.cpp'), dependencies: [proc_dep, ubus_dep],
  cpp_args: user_cpp_args, pic: false, implicit_include_directories: false, build_by_default: false)
mgr_dep = declare_dependency(link_with: mgr, dependencies: [proc_dep, ubus_dep],
  compile_args: ['-I' + join_paths(user_exports, 'mgr')])
posix = static_library(
  'posix', files('apps/posix/io.cpp', 'apps/posix/pthread.cpp'), dependencies: [ubus_dep],
  cpp_args: user_cpp_args + ['-I' + join_paths(root, 'apps/posix/include')],
  pic: false, implicit_include_directories: false, build_by_default: false,
)
subdir('third_party/ftxui')
cxx_runtime = static_library('cxx-runtime', files('third_party/libstdcxx/tree.cc', 'third_party/libstdcxx/shared_ptr.cc',
  'third_party/libstdcxx/thread.cc', 'third_party/libstdcxx/mutex.cc', 'third_party/libstdcxx/condition_variable.cc'),
  cpp_args: user_cpp_args + ['-ffunction-sections', '-fdata-sections'],
  pic: false, implicit_include_directories: false, build_by_default: false)
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
  pic: false, implicit_include_directories: false, build_by_default: false)
tui = static_library('tui', files('apps/tasks/terminal.cpp'), dependencies: [ubus_dep],
  include_directories: ftxui_inc, cpp_args: user_cpp_args,
  pic: false, implicit_include_directories: false, build_by_default: false)
bins = {}
probes = {}
foreach name, spec : {
  'init': [files('apps/init.cpp'), false],
  'mgr': [files('apps/mgr/main.cpp'), false],
  'bus': [files('apps/bus.cpp'), false],
  'svc': [files('apps/svc.cpp'), false],
  'foreign': [files('test/user/foreign.cpp'), false],
  'supervisor': [files('test/user/supervisor.cpp'), false],
  'block': [files('apps/block/main.cpp'), false],
  'devices': [files('apps/devices/main.cpp'), false],
  'bootfs': [files('apps/bootfs.cpp'), false],
  'vfs': [files('apps/vfs.cpp'), false],
  'uart': [files('apps/uart/main.cpp'), false],
  'shell': [files('apps/shell/main.cpp'), false],
  'tasks': [files('apps/tasks/main.cpp'), false],
  'echo': [files('apps/echo.cpp'), true],
  'slow': [files('test/user/stream/slow.cpp'), true],
  'fill': [files('test/user/stream/fill.cpp'), true],
  'control-test': [files('test/user/control.cpp'), false],
  'thread-test': [files('test/user/thread.cpp'), true],
  'cat': [files('apps/cat.cpp'), true],
  'ls': [files('apps/ls.cpp'), true],
  'cp': [files('apps/cp.cpp'), true],
  'fs': [files('apps/fs.cpp'), true],
  'edit': [files('apps/edit/main.cpp'), false],
  'sleep': [files('apps/sleep.cpp'), false],
}
  binary = executable(name + '.elf', spec[0],
    dependencies: [ubus_dep] + (name == 'thread-test' ? [proc_dep] : []) + (['init','mgr','devices','shell','supervisor'].contains(name) ? [mgr_dep] : []),
    c_args: user_c_args, include_directories: name == 'tasks' ? ftxui_inc : [],
    cpp_args: user_cpp_args + (spec[1] ? ['-I' + join_paths(root, 'apps/posix/include')] : []),
    link_whole: [crt, user_entry] + (spec[1] ? [posix] : []),
    link_with: [ubus, cxx_runtime] + (name == 'tasks' ? [tui, ftxui, cxx_runtime, math_runtime] : []),
    link_args: user_link_args + ['-Wl,--gc-sections'] + ['-Wl,-Map,' + join_paths(meson.current_build_dir(), name + '.map')],
    link_language: 'c', implicit_include_directories: false, build_by_default: false)
  if ['fill','slow','control-test','thread-test','foreign','supervisor'].contains(name)
    probes += {name: binary}
  else
    bins += {name: binary}
  endif
endforeach

store_user = executable(
  'store.elf', files('apps/store/main.cpp', 'apps/store/lfs.c',
    'third_party/littlefs/lfs_util.c'), dependencies: [ubus_dep],
  include_directories: include_directories('third_party/littlefs'),
  c_args: user_c_args + ['-DLFS_NO_MALLOC', '-DLFS_NO_DEBUG', '-DLFS_NO_WARN',
    '-DLFS_NO_ERROR', '-UNDEBUG'],
  cpp_args: user_cpp_args + ['-DLFS_NO_MALLOC', '-DLFS_NO_DEBUG', '-DLFS_NO_WARN',
    '-DLFS_NO_ERROR', '-UNDEBUG'],
  link_with: [ubus, cxx_runtime], link_whole: [crt, user_entry], link_args: user_link_args + ['-Wl,--gc-sections'],
  link_language: 'c', implicit_include_directories: false, build_by_default: false,
)

fdcheck_user = executable(
  'fdcheck.elf', files('test/user/fd/main.cpp'), dependencies: [ubus_dep], c_args: user_c_args,
  cpp_args: user_cpp_args + ['-I' + join_paths(root, 'apps/posix/include')],
  link_with: [ubus, cxx_runtime], link_whole: [crt, user_entry, posix], link_args: user_link_args, link_language: 'c', implicit_include_directories: false, build_by_default: false,
)
demand_user = executable(
  'demand-test.elf', files('test/user/demand/main.cpp'), dependencies: [ubus_dep], c_args: user_c_args, cpp_args: user_cpp_args + ['-I' + join_paths(root, 'apps/posix/include')],
  link_with: [ubus, cxx_runtime], link_whole: [crt, user_entry, posix], link_args: user_link_args + ['-Wl,--gc-sections'], link_language: 'c', implicit_include_directories: false, build_by_default: false,
)

initramfs = executable(
  'initramfs', files('tools/initramfs.cpp'), native: true,
  cpp_args: ['-std=c++23', '-I' + root, '-idirafter' + join_paths(root, 'lib/rt')], build_by_default: false,
)

config_builder = executable('config', files('tools/config.cpp', 'ubus/value.cpp'), native: true,
  dependencies: dependency('json-c', native: true), cpp_args: ['-std=c++23', '-I' + root], build_by_default: false)
mgr_config = custom_target('mgr-config', input: files('apps/mgr/boot.json'), output: 'mgr.config',
  command: [config_builder, '@INPUT@', '@OUTPUT@'], build_by_default: false)

syscall_dependency_test = executable(
  'syscall-dependency-test', files('test/host/syscall_dependency_test.cpp'), native: true,
  cpp_args: ['-std=c++23', '-I' + root, '-idirafter' + join_paths(root, 'lib/rt')], build_by_default: false,
)

run_target('test-syscall-dependency', command: [syscall_dependency_test])

value_test = executable('value-test', files('test/host/value_test.cpp', 'ubus/value.cpp'), native: true,
  cpp_args: ['-std=c++23', '-I' + root, '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer'],
  link_args: ['-fsanitize=address,undefined'], build_by_default: false)
run_target('test-value', command: [value_test])

probe_disk = custom_target('probe-disk', output: 'probe.img',
  command: [find_program('python3'), '-c', 'import pathlib,sys; f=pathlib.Path(sys.argv[1]).open("wb"); f.truncate(1024*1024); f.close()', '@OUTPUT@'], build_by_default: false)

channel_test_user = executable(
  'channel-test.elf', files('test/user/channel.cpp'),
  dependencies: [proc_dep, ubus_dep], c_args: user_c_args, cpp_args: user_cpp_args,
  link_with: [proc, ubus, cxx_runtime], link_whole: [crt, user_entry], link_args: user_link_args,
  link_language: 'c', implicit_include_directories: false, build_by_default: false)
process_client_user = executable(
  'process-client.elf', files('test/user/process/main.cpp'),
  dependencies: [proc_dep, ubus_dep], c_args: user_c_args, cpp_args: user_cpp_args + ['-I' + join_paths(root, 'apps/posix/include')],
  link_with: [proc, ubus, cxx_runtime], link_whole: [crt, user_entry, posix], link_args: user_link_args + ['-Wl,--gc-sections'], link_language: 'c', implicit_include_directories: false, build_by_default: false,
)

mgr_test = executable('mgr-test.elf', files('test/user/mgr.cpp'),
  dependencies: [mgr_dep], cpp_args: user_cpp_args, link_with: [mgr, ubus, cxx_runtime], link_whole: [crt, user_entry],
  link_args: user_link_args, link_language: 'c', implicit_include_directories: false, build_by_default: false)

ubus_test = executable('ubus-test.elf', files('test/user/ubus.cpp'),
  dependencies: [proc_dep, ubus_dep], cpp_args: user_cpp_args, link_with: [proc, ubus, cxx_runtime], link_whole: [crt, user_entry],
  link_args: user_link_args + ['-Wl,--gc-sections'], link_language: 'c', implicit_include_directories: false, build_by_default: false)

exec_test = executable('exec-test.elf', files('test/user/exec.cpp'),
  dependencies: [ubus_dep], c_args: user_c_args, cpp_args: user_cpp_args + ['-I' + join_paths(root, 'apps/posix/include')],
  link_with: [ubus, cxx_runtime], link_whole: [crt, user_entry, posix], link_args: user_link_args + ['-Wl,--gc-sections'], link_language: 'c', implicit_include_directories: false, build_by_default: false)

# Debug ELF stays beside the objects; deployment consumes ordinary stripped ELF.
objcopy = find_program('riscv64-unknown-elf-objcopy')
images = {}
foreach name, elf : bins + probes + {
  'ubus-test': ubus_test, 'mgr-test': mgr_test,
  'store': store_user, 'fdcheck': fdcheck_user, 'demand': demand_user,
  'process-client': process_client_user, 'cow': exec_test,
  'channel-test': channel_test_user,
}
  images += {name: custom_target(name + '-runtime', input: elf,
    output: name + '.run.elf', command: [objcopy, '--strip-debug', '@INPUT@', '@OUTPUT@'],
    build_by_default: false)}
endforeach

apps = {'boot/README.TXT': files('apps/README.TXT')[0],
        'boot/touch.elf': images['fs'], 'boot/mkdir.elf': images['fs'],
        'boot/rm.elf': images['fs'], 'boot/mv.elf': images['fs']}
test_apps = {'boot/foreign.elf': images['foreign'], 'boot/supervisor.elf': images['supervisor'], 'boot/fdcheck.elf': images['fdcheck'], 'boot/demand.elf': images['demand'],
             'boot/fill.elf': images['fill'], 'boot/slow.elf': images['slow'], 'boot/control-test.elf': images['control-test'], 'boot/thread-test.elf': images['thread-test']}
foreach name, app : bins
  if name != 'init'
    apps += {'boot/' + name + '.elf': images[name]}
  endif
endforeach
base = apps + {'init': images['init'], 'mgr.config': mgr_config}
bundles = {}
foreach name, layout : {
  'boot': base,
  'storage': base + {'boot/store.elf': images['store']},
  'storage-test': base + test_apps + {'boot/store.elf': images['store']},
  'exec': base + test_apps + {'boot/cow.elf': images['cow']},
  'process-test': base + {'boot/shell.elf': images['process-client']},
  'thread-test': base + test_apps + {'boot/shell.elf': images['thread-test']},
  'ubus-test': {'init': images['ubus-test'], 'provider': images['ubus-test'], 'client': images['bus']},
  'channel-test': {'init': images['channel-test'], 'worker': images['channel-test']},
  'mgr-test': {'init': images['mgr-test'], 'worker': images['mgr-test']},
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

run_target('audit-kernel-symbols', command: [sh, audit_symbols, nm, ar, kernel, 'none', (trace_enabled ? '1' : '0'), arch, core, entry, trace, sync, panic])
run_target('audit-symbols', command: [sh, audit_symbols, nm, ar, kernel_debug, 'any', (trace_enabled ? '1' : '0'), arch, core, test_entry, trace, sync, panic, test_configs['debug'], test_module])
run_target('audit-kernel-stack', command: [sh, audit_stack, meson.current_build_dir(), '1792', kernel])
run_target('audit-boot-stack', command: [sh, audit_stack, meson.current_build_dir(), '2048', kernel_debug])
user_images = [store_user]
foreach name, image : bins
  user_images += [image]
endforeach
run_target('audit-user', command: [sh, audit_user, nm, readelf] + user_images)
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
run_target('run-thread-matrix', command: [find_program('python3'), files('tools/build/run-io-platform.py'),
  qemu, kernel, '1,2,4', '--bundle', bundles['thread-test'], '--timeout', '45',
  '[threads] execution pause, resume, paused retirement and admission rollback ok'])
run_target('run-thread-console', command: [find_program('python3'), files('tools/build/run-console.py'),
  qemu, kernel, storage_test_bundle, '4', 'threads'])
run_target('run-model-matrix', command: [find_program('python3'), files('tools/build/run-console.py'),
  qemu, kernel, storage_test_bundle, '1,2,4', 'control'])
run_target('run-supervisor-matrix', command: [find_program('python3'), files('tools/build/run-console.py'),
  qemu, kernel, storage_test_bundle, '1,2,4', 'recovery', '1'])

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
  '--exact-failed-zero', '--forbid', 'KERNEL PANIC', '--stop-when', '[scenario] wait publication ok',
  '[scenario] wait publication ok'])
run_target('run-wait-test-dual', command: [sh, run_qemu, qemu, timeout, '2', kernel_wait, 'marker',
  '--exact-failed-zero', '--forbid', 'KERNEL PANIC', '--stop-when', '[scenario] wait publication ok',
  '[scenario] pending VM wait completed after remote shootdown', '[scenario] wait publication ok'])

run_target('run-ubus', command: [find_program('python3'), files('tools/build/run-io-platform.py'),
  qemu, kernel_debug, '1,4', '--bundle', bundles['ubus-test'], '--timeout', '30', '--report', '[ubus]',
  '[ubus] dynamic methods, facets, capabilities, bulk and deferred views ok'],
  depends: [kernel_debug, bundles['ubus-test']])

run_target('run-mgr', command: [find_program('python3'), files('tools/build/run-io-platform.py'),
  qemu, kernel_debug, '1,2,4', '--bundle', bundles['mgr-test'], '--timeout', '8',
  '[mgr] environment, lazy Get, direct RPC, linear Receive, Resource and native drain ok'])
run_target('run-channel-matrix', command: [find_program('python3'), files('tools/build/run-io-platform.py'),
  qemu, kernel_debug, '1,2,4', '--bundle', channel_test_bundle, '--timeout', '30', '--report', '[channel]',
  '[channel] native capacity, MCMP payloads, Reply cancellation, Port facts and reassigning ok'],
  depends: [kernel_debug, channel_test_bundle])

run_target('run-io-dual-console-matrix', command: [find_program('python3'),
  files('tools/build/run-io-platform.py'), qemu, kernel, '1,2,4',
  '--bundle', boot_bundle, '--second-disk', '--timeout', '18', '/ > '],
  depends: [kernel, boot_bundle])
run_target('run-test-smp', command: [sh, run_qemu, qemu, timeout, smp, kernel_debug, 'timeout', '--exact-failed-zero', 'cpu: online=' + smp])
if get_option('panic_probe') == 1
  run_target('run-panic-smp', command: [sh, run_qemu, qemu, timeout, smp, kernel_debug, 'shutdown', '--forbid', 'no acknowledgement', '--count-regex', 'cpu [0-9]+: stopped pc=', (get_option('qemu_smp') - 1).to_string(), 'KERNEL PANIC', 'context: call-site', 'expression: false'])
elif get_option('panic_probe') == 2
  run_target('run-panic-degraded-smp', command: [sh, run_qemu, qemu, timeout, smp, kernel_debug, 'shutdown', 'KERNEL PANIC', 'no acknowledgement'])
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
