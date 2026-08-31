project(
  'myos',
  ['c', 'cpp'],
  version: '0.1.0',
  default_options: ['c_std=gnu11', 'cpp_std=gnu++23', 'buildtype=plain', 'b_ndebug=true', 'b_pie=false', 'b_staticpic=false'],
)

profile = get_option('profile')

lock_diag_name = get_option('lock_diag')
if lock_diag_name == 'auto'
  lock_diag_name = profile == 'kernel' ? 'off' : 'trace'
endif
lock_diag_level = 0
if lock_diag_name == 'verify'
  lock_diag_level = 1
elif lock_diag_name == 'trace'
  lock_diag_level = 2
elif lock_diag_name == 'profile'
  lock_diag_level = 3
endif

concurrency_diag_name = get_option('concurrency_diag')
if concurrency_diag_name == 'auto'
  concurrency_diag_name = profile == 'kernel' ? 'off' : 'trace'
endif
concurrency_diag_level = 0
if concurrency_diag_name == 'snapshot'
  concurrency_diag_level = 1
elif concurrency_diag_name == 'trace'
  concurrency_diag_level = 2
elif concurrency_diag_name == 'watch'
  concurrency_diag_level = 3
elif concurrency_diag_name == 'profile'
  concurrency_diag_level = 4
endif

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
elif scenario_name == 'publication'
  scenario_id = 5
elif scenario_name == 'report'
  scenario_id = 6
elif scenario_name == 'dispatch'
  scenario_id = 7
elif scenario_name == 'observer'
  scenario_id = 8
endif

root = meson.project_source_root()
kernel_common_args = [
  '-ffreestanding', '-Wall', '-Wextra', '-O2', '-g3',
  '-march=rv64gc', '-mabi=lp64d', '-mcmodel=medany', '-msmall-data-limit=0',
  '-I' + root, '-I' + join_paths(root, 'kernel'), '-I' + join_paths(root, 'kernel/include'),
  '-I' + join_paths(root, 'arch/riscv64/include'),
]
kernel_c_args = kernel_common_args + ['-Werror=implicit-function-declaration']
kernel_cpp_args = kernel_common_args + [
  '-fno-exceptions', '-fno-rtti', '-fno-threadsafe-statics', '-fno-use-cxa-atexit',
  '-fno-omit-frame-pointer', '-fno-optimize-sibling-calls', '-fstack-usage',
]
kernel_link_args = [
  '-march=rv64gc', '-mabi=lp64d', '-nostdlib',
  '-Wl,-T,' + join_paths(root, 'arch/riscv64/linker.ld'),
]

arch_sources = files(
  'arch/riscv64/boot/entry.S', 'arch/riscv64/boot/early_entry.S',
  'arch/riscv64/boot/high_entry.cpp', 'arch/riscv64/boot/kernel_image.cpp',
  'arch/riscv64/cpu/local_entry.cpp', 'arch/riscv64/cpu/ipi.cpp',
  'arch/riscv64/cpu/instruction.cpp', 'arch/riscv64/cpu/start.cpp',
  'arch/riscv64/cpu/secondary_entry.S', 'arch/riscv64/context/kernel_context.cpp',
  'arch/riscv64/context/kernel_context.S', 'arch/riscv64/sbi/call.cpp',
  'arch/riscv64/sbi/console.cpp', 'arch/riscv64/sbi/system.cpp',
  'arch/riscv64/time/clock.cpp', 'arch/riscv64/time/timer.cpp',
  'arch/riscv64/uart/uart.cpp', 'arch/riscv64/mmu/sv39_builder.cpp',
  'arch/riscv64/mmu/sv39_editor.cpp', 'arch/riscv64/mmu/range_map.cpp',
  'arch/riscv64/mmu/initial_kernel_map.cpp', 'arch/riscv64/mmu/initial_page_table.cpp',
  'arch/riscv64/trap/trap.S', 'arch/riscv64/trap/context.cpp',
  'arch/riscv64/trap/event.cpp', 'arch/riscv64/trap/trap.cpp', 'arch/riscv64/trap/user.cpp',
)

core_kernel_sources = files(
  'kernel/boot/boot.cpp', 'kernel/boot/cpu_topology.cpp', 'kernel/boot/timebase.cpp',
  'kernel/boot/firmware/devicetree/fdt.cpp', 'kernel/image/boot_bundle.cpp',
  'kernel/init/root_task.cpp', 'kernel/init/run.cpp', 'kernel/core/kernel_state.cpp',
  'kernel/cpu/cpu_runtime.cpp', 'kernel/cap/policy.cpp', 'kernel/cap/attenuation.cpp', 'kernel/cap/grant_graph.cpp',
  'kernel/cap/cspace.cpp', 'kernel/ipc/notification.cpp', 'kernel/ipc/buffer.cpp',
  'kernel/ipc/tunnel.cpp', 'kernel/ipc/channel.cpp', 'kernel/pager/pager.cpp',
  'kernel/irq/irq.cpp', 'kernel/ipc/endpoint.cpp', 'kernel/ipc/transfer.cpp',
  'kernel/object/object_ref.cpp', 'kernel/object/object_store.cpp',
  'kernel/resource/pool.cpp', 'kernel/resource/allocation.cpp',
  'kernel/sched/context.cpp', 'kernel/sched/authority.cpp', 'kernel/sched/refill_queue.cpp',
  'kernel/sched/domain.cpp', 'kernel/sched/builtin_policy.cpp',
  'kernel/sched/timer_queue.cpp', 'kernel/sched/remote_queue.cpp',
  'kernel/sched/dispatcher.cpp', 'kernel/cpu/cpu_provisioner.cpp',
  'kernel/cpu/cpu_registry.cpp', 'kernel/cpu/cpu_local.cpp', 'kernel/cpu/ipi.cpp',
  'kernel/cpu/start.cpp', 'kernel/thread/thread.cpp',
  'kernel/execution/authority.cpp', 'kernel/execution/target.cpp',
  'kernel/execution/vproc.cpp', 'kernel/execution/vproc_notification.cpp',
  'kernel/execution/vproc_tunnel.cpp', 'kernel/execution/stop.cpp',
  # /*luna change: link the standalone PageFault continuation owner, reason: Wait only transports one page-specific edge*/
  'kernel/operation/completion.cpp', 'kernel/operation/page_fault.cpp',
  'kernel/operation/wait.cpp',
  'kernel/execution/execution.cpp', 'kernel/execution/binding.cpp',
  'kernel/syscall/syscall.cpp', 'kernel/syscall/common.cpp', 'kernel/syscall/execution.cpp',
  'kernel/syscall/capability.cpp', 'kernel/syscall/construction.cpp',
  'kernel/syscall/object.cpp', 'kernel/syscall/notification.cpp',
  'kernel/syscall/vproc.cpp', 'kernel/syscall/tunnel.cpp', 'kernel/syscall/endpoint.cpp',
  'kernel/syscall/channel.cpp', 'kernel/syscall/pager_irq.cpp', 'kernel/syscall/terminal.cpp',
  'kernel/syscall/vm.cpp', 'kernel/time/clock.cpp', 'kernel/mm/page_state.cpp',
  'kernel/fault/terminal.cpp', 'kernel/fault/observation.cpp', 'kernel/mm/kernel_stack.cpp',
  'kernel/mm/reclaim.cpp',
  # /*luna change: build the bounded MemoryExecutor production service, reason: MemoryObject page work must be linked into the runtime executor*/
  'kernel/mm/memory_work.cpp',
  'kernel/mm/memory_object.cpp', 'kernel/mm/physical_alias.cpp', 'kernel/mm/vspace.cpp',
  'kernel/mm/vspace_fault.cpp', 'kernel/mm/vspace_invalidation.cpp', 'kernel/mm/vspace_view.cpp',
  'kernel/mm/vspace_layout.cpp', 'kernel/mm/vspace_mapping.cpp',
  'kernel/mm/vspace_protection.cpp', 'kernel/mm/vspace_unmap.cpp', 'kernel/mm/vspace_work.cpp',
  'kernel/mm/kernel_vspace.cpp', 'kernel/mm/translation.cpp', 'kernel/mm/direct_map.cpp',
  'kernel/trap/dump.cpp', 'kernel/trap/trap.cpp', 'kernel/mm/boot_map.cpp', 'kernel/mm/pmm.cpp',
  'libk/mem.c',
)

core = static_library('kernel-core', arch_sources + core_kernel_sources, c_args: kernel_c_args, cpp_args: kernel_cpp_args, pic: false)
concurrency_sources = files(
  'kernel/diag/config.cpp', 'kernel/diag/concurrency.cpp',
  'kernel/diag/flight.cpp', 'kernel/diag/report.cpp',
  'kernel/diag/watch.cpp',
)
concurrency_off = static_library(
  'kernel-concurrency-off',
  concurrency_sources,
  c_args: kernel_c_args, cpp_args: kernel_cpp_args + ['-DMYOS_CONCURRENCY_DIAG=0'], pic: false,
)
concurrency = concurrency_off
if concurrency_diag_level != 0
  concurrency = static_library(
    'kernel-concurrency',
    concurrency_sources,
    c_args: kernel_c_args, cpp_args: kernel_cpp_args + ['-DMYOS_CONCURRENCY_DIAG=' + concurrency_diag_level.to_string()], pic: false, build_by_default: false,
  )
endif
concurrency_trace = concurrency
if concurrency_diag_level != 2
  concurrency_trace = static_library(
    'kernel-concurrency-trace', concurrency_sources,
    c_args: kernel_c_args,
    cpp_args: kernel_cpp_args + ['-DMYOS_CONCURRENCY_DIAG=2'],
    pic: false, build_by_default: false,
  )
endif
lock_off = static_library(
  'kernel-lock-off', files('kernel/sync/lock.cpp'), c_args: kernel_c_args,
  cpp_args: kernel_cpp_args + ['-DMYOS_LOCK_DIAG=0', '-DMYOS_CONCURRENCY_DIAG=0'], pic: false,
)
lock = lock_off
if lock_diag_level != 0 or concurrency_diag_level != 0
  lock = static_library(
    'kernel-lock', files('kernel/sync/lock.cpp'), c_args: kernel_c_args,
    cpp_args: kernel_cpp_args + ['-DMYOS_LOCK_DIAG=' + lock_diag_level.to_string(), '-DMYOS_CONCURRENCY_DIAG=' + concurrency_diag_level.to_string()], pic: false, build_by_default: false,
  )
endif
lock_trace = lock
if lock_diag_level != 2 or concurrency_diag_level != 2
  lock_trace = static_library(
    'kernel-lock-trace', files('kernel/sync/lock.cpp'),
    c_args: kernel_c_args,
    cpp_args: kernel_cpp_args
      + ['-DMYOS_LOCK_DIAG=2', '-DMYOS_CONCURRENCY_DIAG=2'],
    pic: false, build_by_default: false,
  )
endif
console = static_library('kernel-console', files('kernel/diag/console.cpp'), c_args: kernel_c_args, cpp_args: kernel_cpp_args, pic: false)
report_off = static_library(
  'kernel-report-off', files('kernel/diag/report_drain.cpp'), c_args: kernel_c_args,
  cpp_args: kernel_cpp_args + ['-DMYOS_CONCURRENCY_DIAG=0'], pic: false,
)
report = report_off
if concurrency_diag_level != 0
  report = static_library(
    'kernel-report', files('kernel/diag/report_drain.cpp'), c_args: kernel_c_args,
    cpp_args: kernel_cpp_args + ['-DMYOS_CONCURRENCY_DIAG=' + concurrency_diag_level.to_string()], pic: false, build_by_default: false,
  )
endif
report_trace = report
if concurrency_diag_level != 2
  report_trace = static_library(
    'kernel-report-trace', files('kernel/diag/report_drain.cpp'),
    c_args: kernel_c_args,
    cpp_args: kernel_cpp_args + ['-DMYOS_CONCURRENCY_DIAG=2'],
    pic: false, build_by_default: false,
  )
endif
panic_off = static_library(
  'kernel-panic-off', files('kernel/diag/panic.cpp'), c_args: kernel_c_args,
  cpp_args: kernel_cpp_args + [
    '-DMYOS_PANIC_PROBE=0',
    '-DMYOS_LOCK_DIAG=0',
    '-DMYOS_CONCURRENCY_DIAG=0',
  ], pic: false,
)
panic = panic_off
if get_option('panic_probe') != 0 or lock_diag_level != 0 or concurrency_diag_level != 0
  panic = static_library(
    'kernel-panic', files('kernel/diag/panic.cpp'), c_args: kernel_c_args,
    cpp_args: kernel_cpp_args + [
      '-DMYOS_PANIC_PROBE=' + get_option('panic_probe').to_string(),
      '-DMYOS_LOCK_DIAG=' + lock_diag_level.to_string(),
      '-DMYOS_CONCURRENCY_DIAG=' + concurrency_diag_level.to_string(),
    ], pic: false, build_by_default: false,
  )
endif
panic_trace = panic
if lock_diag_level != 2 or concurrency_diag_level != 2
  panic_trace = static_library(
    'kernel-panic-trace', files('kernel/diag/panic.cpp'),
    c_args: kernel_c_args,
    cpp_args: kernel_cpp_args + [
      '-DMYOS_PANIC_PROBE=' + get_option('panic_probe').to_string(),
      '-DMYOS_LOCK_DIAG=2',
      '-DMYOS_CONCURRENCY_DIAG=2',
    ], pic: false, build_by_default: false,
  )
endif

scenario_off = static_library(
  'kernel-scenario-off', files('kernel/diag/scenario_off.cpp'),
  c_args: kernel_c_args, cpp_args: kernel_cpp_args, pic: false,
)
scenario = scenario_off
if scenario_name != 'off'
  scenario = static_library(
    'kernel-scenario', files('kernel/diag/scenario_real.cpp'),
    c_args: kernel_c_args,
    cpp_args: kernel_cpp_args + ['-DMYOS_SCENARIO_ID=' + scenario_id.to_string()],
    pic: false, build_by_default: false,
  )
endif

test_off = static_library(
  'kernel-test-off',
  files('kernel/diag/test_off.cpp', 'kernel/diag/scenario_gate_off.cpp'),
  c_args: kernel_c_args,
  cpp_args: kernel_cpp_args + ['-fomit-frame-pointer', '-foptimize-sibling-calls'],
  pic: false,
)
test_sources = files(
  'test/boot.cpp', 'test/scenario_gate.cpp', 'test/framework.cpp', 'test/scenario.cpp',
  'test/scenario_basic.cpp', 'test/scenario_remote.cpp', 'test/scenario_publication.cpp',
  'test/scenario_report.cpp', 'test/scenario_observer.cpp',
  # /*luna change: compile the pressure fixture with the existing test module, reason: ordinary images keep the dormant storage while only the pressure image selects it*/
  'test/scenario_pressure.cpp', 'test/libk_test.cpp',
  'test/sync_test.cpp', 'test/allocator_test.cpp', 'test/bootinfo_test.cpp',
  'test/boot_bundle_test.cpp', 'test/cpu_topology_test.cpp', 'test/sched_test.cpp',
  'test/cap_test.cpp', 'test/memory_test.cpp', 'test/translation_test.cpp',
  'test/vspace_test.cpp', 'test/user_test.cpp', 'test/ipc_test.cpp', 'test/e7_test.cpp',
)
test_module = static_library(
  'kernel-test', test_sources, c_args: kernel_c_args,
  cpp_args: kernel_cpp_args + ['-fomit-frame-pointer', '-foptimize-sibling-calls'],
  pic: false, build_by_default: false,
)

sh = find_program('sh')
image_suffix = '-riscv64-' + profile + '-lock' + lock_diag_name + '-conc' + concurrency_diag_name
build_info_source = files('kernel/image/build_info.cpp')
build_info_args = kernel_cpp_args + ['-DMYOS_BUILD_ID="' + get_option('build_id') + '"']
debug_build_info_args = build_info_args
if get_option('build_id') == 'auto'
  build_info_source = vcs_tag(
    command: [sh, files('tools/build/build-id.sh'), root],
    input: 'tools/build/build_info.cpp.in', output: 'build_info_auto.cpp',
    replace_string: '@VCS_TAG@', fallback: 'unknown-clean',
  )
  build_info_args = kernel_cpp_args + ['-DMYOS_IMAGE_SUFFIX="-riscv64-kernel-lockoff-concoff"']
  debug_build_info_args = kernel_cpp_args + ['-DMYOS_IMAGE_SUFFIX="' + image_suffix + '"']
endif
# /*luna change: give the pressure image its own concise build identity, reason: the selected provider is a separate image while explicit build IDs remain shared*/
pressure_build_info_args = build_info_args
pressure_trace_build_info_args = debug_build_info_args
if get_option('build_id') == 'auto'
  pressure_build_info_args = kernel_cpp_args + ['-DMYOS_IMAGE_SUFFIX="-riscv64-pressure-lockoff-concoff"']
  pressure_trace_build_info_args = kernel_cpp_args + ['-DMYOS_IMAGE_SUFFIX="-riscv64-pressure-trace-locktrace-conctrace"']
endif
kernel = executable(
  'kernel.elf', build_info_source,
  c_args: kernel_c_args, cpp_args: build_info_args,
  link_whole: [core, concurrency_off, lock_off, console, report_off, panic_off, scenario_off, test_off],
  link_args: kernel_link_args + ['-Wl,-Map,' + join_paths(meson.current_build_dir(), 'kernel.map')],
  link_language: 'c',
)
kernel_debug = executable(
  'kernel_debug.elf', build_info_source,
  c_args: kernel_c_args, cpp_args: debug_build_info_args,
  link_whole: [core, concurrency, lock, console, report, panic, scenario, test_module],
  link_args: kernel_link_args + ['-Wl,-Map,' + join_paths(meson.current_build_dir(), 'kernel_debug.map')],
  link_language: 'c', build_by_default: false,
)
# /*luna change: compile the pressure selector as a separate provider, reason: kernel_debug keeps its existing scenario provider and link graph*/
pressure_scenario = static_library(
  'kernel-scenario-pressure', files('kernel/diag/scenario_real.cpp'),
  c_args: kernel_c_args,
  cpp_args: kernel_cpp_args + ['-DMYOS_SCENARIO_ID=9'],
  pic: false, build_by_default: false,
)
# /*luna change: link a pressure-only image from shared core and test objects, reason: the physical fixture must not alter ordinary kernel_debug scenario selection or duplicate the object tree*/
# The functional pressure gate keeps panic/assert, console, tests and the exact
# pressure scenario while avoiding full recorder cost. The trace image below
# changes providers only; both executables consume the same semantic archives.
kernel_pressure = executable(
  'kernel_pressure.elf', build_info_source,
  c_args: kernel_c_args, cpp_args: pressure_build_info_args,
  link_whole: [core, concurrency_off, lock_off, console, report_off, panic_off, pressure_scenario, test_module],
  link_args: kernel_link_args + ['-Wl,-Map,' + join_paths(meson.current_build_dir(), 'kernel_pressure.map')],
  link_language: 'c', build_by_default: false,
)
kernel_pressure_trace = executable(
  'kernel_pressure_trace.elf', build_info_source,
  c_args: kernel_c_args, cpp_args: pressure_trace_build_info_args,
  link_whole: [core, concurrency_trace, lock_trace, console, report_trace, panic_trace, pressure_scenario, test_module],
  link_args: kernel_link_args + ['-Wl,-Map,' + join_paths(meson.current_build_dir(), 'kernel_pressure_trace.map')],
  link_language: 'c', build_by_default: false,
)

user_common_args = [
  '-ffreestanding', '-Wall', '-Wextra', '-Werror', '-O2', '-g3',
  '-march=rv64imac_zicsr_zifencei', '-mabi=lp64', '-mcmodel=medany', '-msmall-data-limit=0', '-I' + root,
]
user_cpp_args = user_common_args + ['-fno-exceptions', '-fno-rtti', '-fno-threadsafe-statics', '-fno-use-cxa-atexit']
user_link_args = ['-march=rv64imac_zicsr_zifencei', '-mabi=lp64', '-nostdlib', '-Wl,-T,' + join_paths(root, 'user/riscv64/linker.ld')]
user_runtime = static_library(
  'user-runtime', files('user/lib/crt0.S', 'user/lib/crt.cpp', 'user/riscv64/context.S', 'libk/mem.c'),
  c_args: user_common_args, cpp_args: user_cpp_args, pic: false, build_by_default: false,
)
init_user = executable(
  'init.elf', files('servers/init/main.cpp'), c_args: user_common_args, cpp_args: user_cpp_args,
  link_whole: user_runtime, link_args: user_link_args + ['-Wl,-Map,' + join_paths(meson.current_build_dir(), 'init.map')], link_language: 'c', build_by_default: false,
)
process_server_user = executable(
  'process_server.elf', files('servers/process_server/main.cpp'), c_args: user_common_args, cpp_args: user_cpp_args,
  link_whole: user_runtime, link_args: user_link_args + ['-Wl,-Map,' + join_paths(meson.current_build_dir(), 'process_server.map')], link_language: 'c', build_by_default: false,
)
proof_user = executable(
  'proof.elf', files('programs/proof/main.cpp'), c_args: user_common_args, cpp_args: user_cpp_args,
  link_whole: user_runtime, link_args: user_link_args + ['-Wl,-Map,' + join_paths(meson.current_build_dir(), 'proof.map')], link_language: 'c', build_by_default: false,
)
consumer_user = executable(
  'consumer.elf', files('programs/consumer/main.cpp'), c_args: user_common_args, cpp_args: user_cpp_args,
  link_whole: user_runtime, link_args: user_link_args + ['-Wl,-Map,' + join_paths(meson.current_build_dir(), 'consumer.map')], link_language: 'c', build_by_default: false,
)
pager_user = executable(
  'pager.elf', files('servers/pager/main.cpp'), c_args: user_common_args, cpp_args: user_cpp_args,
  link_whole: user_runtime, link_args: user_link_args + ['-Wl,-Map,' + join_paths(meson.current_build_dir(), 'pager.map')], link_language: 'c', build_by_default: false,
)
 # The supervised-failure workload is test-owned, but it enters the same
 # production packer and ordinary init/process_server lifecycle as proof.elf.
supervised_failure_user = executable(
  'supervised-failure.elf', files('test/user/supervised_failure/failing_proof.cpp'),
  c_args: user_common_args, cpp_args: user_cpp_args,
  link_whole: user_runtime,
  link_args: user_link_args + ['-Wl,-Map,' + join_paths(meson.current_build_dir(), 'supervised-failure.map')],
  link_language: 'c', build_by_default: false,
)
# The broad channel/pager/pressure/resilience fixture remains a
# test-owned workload.  It is deliberately built as independent ELFs so the
# production init/process-server/proof roles cannot silently inherit its
# coordinator state or intentional-fault markers.
e7_init_user = executable(
  'e7-init-test.elf', files('test/user/e7/init.cpp'), c_args: user_common_args, cpp_args: user_cpp_args,
  link_whole: user_runtime, link_args: user_link_args + ['-Wl,-Map,' + join_paths(meson.current_build_dir(), 'e7-init-test.map')], link_language: 'c', build_by_default: false,
)
e7_proof_user = executable(
  'e7-proof-test.elf', files('test/user/e7/proof.cpp'), c_args: user_common_args, cpp_args: user_cpp_args,
  link_whole: user_runtime, link_args: user_link_args + ['-Wl,-Map,' + join_paths(meson.current_build_dir(), 'e7-proof-test.map')], link_language: 'c', build_by_default: false,
)
uart_user = executable(
  'uart.elf', files('servers/uart/main.cpp'), c_args: user_common_args, cpp_args: user_cpp_args,
  link_whole: user_runtime, link_args: user_link_args + ['-Wl,-Map,' + join_paths(meson.current_build_dir(), 'uart.map')], link_language: 'c', build_by_default: false,
)

bootpack = executable(
  'bootpack', files('tools/bootpack/main.cpp'), native: true,
  cpp_args: ['-std=c++23', '-I' + root], build_by_default: false,
)
deploypack = executable(
  'deploypack', files('tools/deploypack/main.cpp'), native: true,
  cpp_args: ['-std=c++23', '-I' + root], build_by_default: false,
)
task_builder_test_manifest = executable(
  'task-builder-test-manifest', files('test/user/task_builder/manifest.cpp'),
  native: true, cpp_args: ['-std=c++23', '-I' + root], build_by_default: false,
)
task_builder_test_manifest_source = custom_target(
  'task-builder-test-manifest-source', output: 'task_builder_test_manifest.cpp',
  command: [task_builder_test_manifest, '@OUTPUT@'], build_by_default: false,
)
task_builder_test_user = executable(
  'task-builder-test.elf', [files('test/user/task_builder/main.cpp'), task_builder_test_manifest_source],
  c_args: user_common_args, cpp_args: user_cpp_args,
  link_whole: user_runtime,
  link_args: user_link_args + ['-Wl,-Map,' + join_paths(meson.current_build_dir(), 'task-builder-test.map')],
  link_language: 'c', build_by_default: false,
)
task_builder_test_child = executable(
  'task-builder-test-child.elf', files('test/user/task_builder/child.cpp'),
  c_args: user_common_args, cpp_args: user_cpp_args,
  link_whole: user_runtime,
  link_args: user_link_args + ['-Wl,-Map,' + join_paths(meson.current_build_dir(), 'task-builder-test-child.map')],
  link_language: 'c', build_by_default: false,
)
deploy_manifest_test = executable(
  'deploy-manifest-test', files('tools/deploy_manifest_test.cpp'), native: true,
  cpp_args: ['-std=c++23', '-I' + root], build_by_default: false,
)
boot_bundle_test = executable(
  'boot-bundle-test', files('tools/boot_bundle_test.cpp'), native: true,
  cpp_args: ['-std=c++23', '-I' + root], build_by_default: false,
)
capability_owner_test = executable(
  'capability-owner-test', files('tools/capability_owner_test.cpp'), native: true,
  cpp_args: ['-std=c++23', '-I' + root], build_by_default: false,
)
deployment_test = executable(
  'deployment-test', files('tools/deployment_test.cpp'), native: true,
  cpp_args: ['-std=c++23', '-I' + root], build_by_default: false,
)
syscall_dependency_test = executable(
  'syscall-dependency-test', files('tools/syscall_dependency_test.cpp'), native: true,
  cpp_args: ['-std=c++23', '-I' + root], build_by_default: false,
)
image_materializer_test = executable(
  'image-materializer-test', files('tools/image_materializer_test.cpp'), native: true,
  cpp_args: ['-std=c++23', '-I' + root], build_by_default: false,
)
task_transaction_test = executable(
  'task-transaction-test', files('tools/task_transaction_test.cpp'), native: true,
  cpp_args: ['-std=c++23', '-I' + root], build_by_default: false,
)
task_authority_test = executable(
  'task-authority-test', files('tools/task_authority_test.cpp'), native: true,
  cpp_args: ['-std=c++23', '-I' + root], build_by_default: false,
)
deploy_fixture = custom_target(
  'deploy-fixture', output: 'deployment.fixture',
  command: [deploypack, '@OUTPUT@'], build_by_default: false,
)
production_manifest = custom_target(
  'production-manifest', input: [process_server_user, proof_user, consumer_user, pager_user, uart_user],
  output: 'deployment.manifest',
  command: [deploypack, 'production', '@OUTPUT@', '@INPUT0@', '@INPUT1@', '@INPUT2@', '@INPUT3@', '@INPUT4@'],
  build_by_default: false,
)
supervised_failure_manifest = custom_target(
  'supervised-failure-manifest',
  input: [process_server_user, supervised_failure_user, consumer_user, pager_user, uart_user],
  output: 'supervised-failure.manifest',
  command: [deploypack, 'production', '@OUTPUT@', '@INPUT0@', '@INPUT1@', '@INPUT2@', '@INPUT3@', '@INPUT4@'],
  build_by_default: false,
)
run_target('test-deploy-manifest',
  command: [deploy_manifest_test, deploy_fixture, production_manifest])
run_target('test-boot-bundle', command: [boot_bundle_test])
run_target('test-bootpack-root-role', command: [sh, '-c',
  '"$1" /dev/null manifest "data:manifest=$2"; status=$?; test "$status" -eq 1',
  'test-bootpack-root-role', bootpack, production_manifest])
run_target('test-capability-owner', command: [capability_owner_test])
run_target('test-deployment', command: [deployment_test])
run_target('test-syscall-dependency', command: [syscall_dependency_test])
run_target('test-image-materializer', command: [image_materializer_test])
run_target('test-task-transaction', command: [task_transaction_test])
run_target('test-task-authority', command: [task_authority_test])
boot_bundle = custom_target(
  'boot-bundle', input: [init_user, process_server_user, proof_user, consumer_user, pager_user, uart_user, production_manifest], output: 'boot.bundle',
  command: [bootpack, '@OUTPUT@', 'init', 'init=@INPUT0@', 'process_server=@INPUT1@', 'proof=@INPUT2@', 'consumer=@INPUT3@', 'pager=@INPUT4@', 'uart=@INPUT5@', 'data:manifest=@INPUT6@'], build_by_default: false,
)
supervised_failure_bundle = custom_target(
  'supervised-failure-bundle',
  input: [init_user, process_server_user, supervised_failure_user, consumer_user, pager_user, uart_user, supervised_failure_manifest],
  output: 'supervised-failure.bundle',
  command: [bootpack, '@OUTPUT@', 'init', 'init=@INPUT0@',
    'process_server=@INPUT1@', 'proof=@INPUT2@', 'consumer=@INPUT3@',
    'pager=@INPUT4@', 'uart=@INPUT5@', 'data:manifest=@INPUT6@'],
  build_by_default: false,
)
# /*luna change: encode pressure mode in the existing root module role, reason: the same user images and kernel composition need an explicit guest-visible run condition*/
pressure_bundle = custom_target(
    'pressure-bundle', input: [e7_init_user, e7_proof_user, uart_user], output: 'pressure.bundle',
    command: [bootpack, '@OUTPUT@', 'pressure', 'pressure=@INPUT0@', 'proof=@INPUT1@', 'uart=@INPUT2@'], build_by_default: false,
)
# /*luna change: encode resilience mode in its own root module role, reason: worker-death redelivery and pager_fail supervision are a separate run condition of the same images*/
resilience_bundle = custom_target(
    'resilience-bundle', input: [e7_init_user, e7_proof_user, uart_user], output: 'resilience.bundle',
    command: [bootpack, '@OUTPUT@', 'resilience', 'resilience=@INPUT0@', 'proof=@INPUT1@', 'uart=@INPUT2@'], build_by_default: false,
)
proof_bundle = custom_target(
  'proof-bundle', input: e7_proof_user, output: 'proof.bundle', command: [bootpack, '@OUTPUT@', 'proof', 'proof=@INPUT@'], build_by_default: false,
)
task_builder_test_bundle = custom_target(
  'task-builder-test.bundle', input: [task_builder_test_user, task_builder_test_child], output: 'task-builder-test.bundle',
  command: [bootpack, '@OUTPUT@', 'task-builder-test', 'task-builder-test=@INPUT0@', 'child=@INPUT1@'],
  build_by_default: false,
)

nm = find_program('riscv64-unknown-elf-nm')
ar = find_program('riscv64-unknown-elf-ar')
readelf = find_program('riscv64-unknown-elf-readelf')
clangxx = find_program('clang++')
audit_symbols = files('tools/build/audit-symbols.sh')
audit_stack = files('tools/build/audit-boot-stack.sh')
audit_user = files('tools/build/audit-user.sh')
audit_task_transaction = files('tools/build/audit-task-transaction.sh')
audit_task_authority = files('tools/build/audit-task-authority.sh')
audit_operation_completion = files('tools/build/audit-operation-completion.sh')
audit_clang = files('tools/build/audit-clang.sh')
run_qemu = files('tools/build/run-qemu.sh')
artifact = files('tools/build/artifact.sh')

run_target('audit-kernel-symbols', command: [sh, audit_symbols, nm, ar, kernel, '0', 'none', core, concurrency_off, lock_off, console, report_off, panic_off, scenario_off, test_off])
run_target('audit-symbols', command: [sh, audit_symbols, nm, ar, kernel_debug, concurrency_diag_level.to_string(), 'any', core, concurrency, lock, console, report, panic, scenario, test_module])
run_target('audit-pressure-symbols', command: [sh, audit_symbols, nm, ar, kernel_pressure, '0', 'pressure', core, concurrency_off, lock_off, console, report_off, panic_off, pressure_scenario, test_module])
run_target('audit-pressure-trace-symbols', command: [sh, audit_symbols, nm, ar, kernel_pressure_trace, '2', 'pressure', core, concurrency_trace, lock_trace, console, report_trace, panic_trace, pressure_scenario, test_module])
run_target('audit-kernel-stack', command: [sh, audit_stack, meson.current_build_dir(), '1792', kernel])
run_target('audit-boot-stack', command: [sh, audit_stack, meson.current_build_dir(), '2048', kernel_debug])
run_target('audit-pressure-stack', command: [sh, audit_stack, meson.current_build_dir(), '2048', kernel_pressure])
run_target('audit-pressure-trace-stack', command: [sh, audit_stack, meson.current_build_dir(), '2048', kernel_pressure_trace])
run_target('audit-user', command: [sh, audit_user, nm, readelf, init_user, process_server_user, proof_user, consumer_user, pager_user, uart_user])
run_target('audit-e7-user', command: [sh, audit_user, nm, readelf, e7_init_user, e7_proof_user])
run_target('audit-task-transaction', command: [sh, audit_task_transaction, join_paths(root, 'user/lib/task_transaction.hpp')])
run_target('audit-task-authority', command: [sh, audit_task_authority,
  join_paths(root, 'user/lib/task_authority.hpp'),
  join_paths(root, 'user/lib/deploy_manifest.hpp')])
run_target('audit-operation-completion', command: [sh, audit_operation_completion])
run_target('audit-clang', command: [sh, audit_clang, clangxx, root, concurrency_diag_level.to_string(), lock_diag_level.to_string(), get_option('panic_probe').to_string()])
objdump = find_program('riscv64-unknown-elf-objdump')
run_target('disasm', command: [sh, artifact, objdump, 'disasm', kernel, join_paths(meson.current_build_dir(), 'kernel.disasm')])
run_target('symbols', command: [sh, artifact, nm, 'symbols', kernel, join_paths(meson.current_build_dir(), 'kernel.sym')])
run_target('debug-disasm', command: [sh, artifact, objdump, 'disasm', kernel_debug, join_paths(meson.current_build_dir(), 'kernel_debug.disasm')])
run_target('debug-symbols', command: [sh, artifact, nm, 'symbols', kernel_debug, join_paths(meson.current_build_dir(), 'kernel_debug.sym')])

qemu = find_program('qemu-system-riscv64')
timeout = get_option('qemu_timeout')
resilience_timeout = get_option('qemu_resilience_timeout')
pressure_timeout = get_option('qemu_pressure_timeout')
smp = get_option('qemu_smp').to_string()
pressure_memory = get_option('qemu_pressure_memory')
run_target('run', command: [qemu, '-machine', 'virt', '-nographic', '-bios', 'default', '-kernel', kernel])
run_target('run-timeout', command: [sh, run_qemu, qemu, '3s', '1', kernel, 'timeout'])
run_target('run-debug', command: [qemu, '-machine', 'virt', '-nographic', '-bios', 'default', '-kernel', kernel_debug])
run_target('debug', command: [qemu, '-machine', 'virt', '-nographic', '-bios', 'default', '-kernel', kernel_debug, '-S', '-gdb', 'tcp:' + get_option('gdb_host') + ':' + get_option('gdb_port').to_string()])
run_target('run-test-smp', command: [sh, run_qemu, qemu, timeout, smp, kernel_debug, 'timeout', '--exact-failed-zero', 'cpu: discovered=' + smp + ' prepared=0 starting=0 online=' + smp + ' failed=0'])
if profile == 'proof'
  run_target('run-proof-smp', command: [sh, run_qemu, qemu, timeout, smp, kernel_debug, 'timeout', '--exact-failed-zero', 'cpu: discovered=' + smp + ' prepared=0 starting=0 online=' + smp + ' failed=0', 'root init: started', 'user: contained fault address=0x1000 after syscalls=5 active-vspace-cpus=1', '--initrd', proof_bundle])
  run_target('run-e1-smp', command: [sh, run_qemu, qemu, timeout, smp, kernel_debug, 'timeout', '--exact-failed-zero', '--forbid', 'MYOS KERNEL PANIC', '--forbid', 'user: contained fault address=', 'cpu: discovered=' + smp + ' prepared=0 starting=0 online=' + smp + ' failed=0', 'boot bundle: root=init', 'init: boot-root', 'init: manifest-decoded', 'init: process-server-prepared', 'init: process-server-running', 'init: process-server-terminal', 'init: authorities-retired', 'init: bundle-views-closed', 'init: complete', '--initrd', boot_bundle])
  # /*luna change: route pressure through its root role, watchdog and fixture image, reason: one explicit A-to-A' run condition keeps ordinary E1 unchanged while exercising the physical PMM floor*/
  run_target('run-pressure-smp', command: [sh, run_qemu, qemu, pressure_timeout, smp, kernel_pressure, 'marker', '--memory', pressure_memory, '--exact-failed-zero', '--forbid', 'MYOS KERNEL PANIC', '--stop-when', 'uart: online', '--stop-when', 'user: contained fault address=0xe0', '--stop-when', 'MYOS KERNEL PANIC', 'cpu: discovered=' + smp + ' prepared=0 starting=0 online=' + smp + ' failed=0', 'boot bundle: root=pressure', '[scenario] pressure armed', '--regex', 'user: contained.*fault address=0xe100', 'uart: online', '--initrd', pressure_bundle])
  # Full trace is an explicit diagnostic observation, not the functional gate.
  run_target('run-pressure-trace-smp', command: [sh, run_qemu, qemu, pressure_timeout, smp, kernel_pressure_trace, 'timeout', '--memory', pressure_memory, '--exact-failed-zero', '--forbid', 'MYOS KERNEL PANIC', 'cpu: discovered=' + smp + ' prepared=0 starting=0 online=' + smp + ' failed=0', 'boot bundle: root=pressure', '[scenario] pressure armed', '--initrd', pressure_bundle])
  # /*luna change: gate the resilience condition on the default memory size, reason: worker death and pager_fail supervision do not manufacture PMM pressure*/
  run_target('run-resilience-smp', command: [sh, run_qemu, qemu, resilience_timeout, smp, kernel_debug, 'timeout', '--exact-failed-zero', '--forbid', 'MYOS KERNEL PANIC', 'cpu: discovered=' + smp + ' prepared=0 starting=0 online=' + smp + ' failed=0', 'boot bundle: root=resilience', 'user: contained fault address=0x25001000', 'user: contained fault address=0xe100', '--initrd', resilience_bundle])
  run_target('run-task-builder-test-smp', command: [sh, run_qemu, qemu, timeout, smp, kernel_debug, 'marker', '--forbid', 'MYOS KERNEL PANIC', '--forbid', 'user: contained fault address=', '--stop-when', 'task-builder-test: diag-cleanup-console-start', 'task-builder-test: boot-root', 'task-builder-test: manifest-decoded', 'task-builder-test: cuts-ok', 'task-builder-test: passed', 'task-builder-test: diag-cleanup-console-start', '--initrd', task_builder_test_bundle])
  run_target('audit-task-builder-test-user', command: [sh, audit_user, nm, readelf, task_builder_test_user, task_builder_test_child])
endif
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
elif scenario_name == 'publication'
  scenario_marker = '[scenario] publication ok'
elif scenario_name == 'report'
  scenario_marker = '[scenario] report-retry wake-credit ok'
elif scenario_name == 'dispatch'
  scenario_marker = '[scenario] dispatch-flight ok'
elif scenario_name == 'observer'
  scenario_marker = '[scenario] observer ok'
endif
scenario_args_qemu = [sh, run_qemu, qemu, timeout, smp, kernel_debug, 'timeout']
if scenario_marker != ''
  scenario_args_qemu += [scenario_marker]
endif
if scenario_name == 'initrd'
  scenario_args_qemu += ['--initrd', boot_bundle]
endif
if scenario_name != 'off'
  run_target('run-scenario', command: scenario_args_qemu)
endif

alias_target('kernel', kernel)
alias_target('kernel-debug', kernel_debug)
# /*luna change: expose the pressure image without changing ordinary aliases, reason: build and audit selection must remain explicit for the physical fixture*/
alias_target('kernel-pressure', kernel_pressure)
alias_target('kernel-pressure-trace', kernel_pressure_trace)
alias_target('bundle', [boot_bundle, production_manifest, pressure_bundle, resilience_bundle, proof_bundle, uart_user, e7_init_user, e7_proof_user])
