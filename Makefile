ARCH ?= riscv64
PROFILE ?= kernel
LOCK_DIAG ?= $(if $(filter kernel,$(PROFILE)),off,trace)
CONCURRENCY_DIAG ?= $(if $(filter kernel,$(PROFILE)),off,trace)

SUPPORTED_ARCHES := riscv64
SUPPORTED_PROFILES := kernel test proof
SUPPORTED_LOCK_DIAG := off verify trace profile
SUPPORTED_CONCURRENCY_DIAG := off snapshot trace watch profile

ifeq ($(filter $(ARCH),$(SUPPORTED_ARCHES)),)
$(error Unsupported ARCH=$(ARCH); supported: $(SUPPORTED_ARCHES))
endif
ifeq ($(filter $(PROFILE),$(SUPPORTED_PROFILES)),)
$(error Unsupported PROFILE=$(PROFILE); supported: $(SUPPORTED_PROFILES))
endif
ifeq ($(filter $(LOCK_DIAG),$(SUPPORTED_LOCK_DIAG)),)
$(error Unsupported LOCK_DIAG=$(LOCK_DIAG); supported: $(SUPPORTED_LOCK_DIAG))
endif
ifeq ($(filter $(CONCURRENCY_DIAG),$(SUPPORTED_CONCURRENCY_DIAG)),)
$(error Unsupported CONCURRENCY_DIAG=$(CONCURRENCY_DIAG); supported: $(SUPPORTED_CONCURRENCY_DIAG))
endif

LOCK_DIAG_LEVEL_off := 0
LOCK_DIAG_LEVEL_verify := 1
LOCK_DIAG_LEVEL_trace := 2
LOCK_DIAG_LEVEL_profile := 3
LOCK_DIAG_LEVEL := $(LOCK_DIAG_LEVEL_$(LOCK_DIAG))

CONCURRENCY_DIAG_LEVEL_off := 0
CONCURRENCY_DIAG_LEVEL_snapshot := 1
CONCURRENCY_DIAG_LEVEL_trace := 2
CONCURRENCY_DIAG_LEVEL_watch := 3
CONCURRENCY_DIAG_LEVEL_profile := 4
CONCURRENCY_DIAG_LEVEL := $(CONCURRENCY_DIAG_LEVEL_$(CONCURRENCY_DIAG))

ENABLE_TESTS := 0
ifeq ($(PROFILE),test)
ENABLE_TESTS := 1
endif
ifeq ($(PROFILE),proof)
ENABLE_TESTS := 1
endif

# ---------- Toolchain selection ----------
# Priority:
#   1. CROSS=... passed by user, e.g. make CROSS=riscv64-unknown-elf-
#   2. xPack riscv-none-elf-gcc
#   3. PATH riscv-none-elf-gcc
#   4. PATH riscv64-unknown-elf-gcc, Ubuntu apt package

XPACK_RISCV_GCC := $(firstword $(wildcard $(HOME)/.local/xPacks/riscv-none-elf-gcc/xpack-riscv-none-elf-gcc-*/bin/riscv-none-elf-gcc))
RISCV_NONE_ELF_GCC := $(shell command -v riscv-none-elf-gcc 2>/dev/null)
RISCV64_UNKNOWN_ELF_GCC := $(shell command -v riscv64-unknown-elf-gcc 2>/dev/null)

ifeq ($(strip $(CROSS)),)
ifneq ($(strip $(XPACK_RISCV_GCC)),)
CROSS := $(dir $(XPACK_RISCV_GCC))riscv-none-elf-
else ifneq ($(strip $(RISCV_NONE_ELF_GCC)),)
CROSS := riscv-none-elf-
else ifneq ($(strip $(RISCV64_UNKNOWN_ELF_GCC)),)
CROSS := riscv64-unknown-elf-
else
$(error No RISC-V toolchain found. Install gcc-riscv64-unknown-elf or xPack riscv-none-elf-gcc)
endif
endif

CC      := $(CROSS)gcc
CXX     := $(CROSS)g++
OBJDUMP := $(CROSS)objdump
NM      := $(CROSS)nm
READELF := $(CROSS)readelf
QEMU    ?= qemu-system-riscv64
CLANGXX ?= clang++
HOST_CXX ?= c++
QEMU_SMP ?= 4
QEMU_TIMEOUT ?= 10s
PANIC_PROBE ?= 0
CONCURRENCY_PROBE ?= 0
STAGE_F_PROBE ?= 0
SCENARIO ?=
GDB_HOST ?= 127.0.0.1
GDB_PORT ?= 1237

ifneq ($(filter-out 0,$(CONCURRENCY_PROBE)),)
ifneq ($(filter-out 0,$(STAGE_F_PROBE)),)
$(error CONCURRENCY_PROBE and STAGE_F_PROBE are exclusive scenario selectors)
endif
LEGACY_SCENARIO_1 := ordinary
LEGACY_SCENARIO_2 := ordinary
LEGACY_SCENARIO_3 := initrd
LEGACY_SCENARIO_4 := initrd
LEGACY_SCENARIO_5 := publication
LEGACY_SCENARIO_6 := report
LEGACY_SCENARIO_7 := remote
LEGACY_SCENARIO_8 := publication
LEGACY_SCENARIO_9 := dispatch
LEGACY_SCENARIO_10 := dispatch
LEGACY_SCENARIO_11 := dispatch
LEGACY_SCENARIO_12 := trap
LEGACY_SCENARIO_13 := remote
LEGACY_SCENARIO_14 := publication
LEGACY_SCENARIO_15 := report
else ifneq ($(filter-out 0,$(STAGE_F_PROBE)),)
# Legacy Stage-F CLI maps to the mechanism scenario ``dispatch``.
LEGACY_SCENARIO_STAGE_F := dispatch
else
LEGACY_SCENARIO_STAGE_F := off
endif

ifneq ($(strip $(SCENARIO)),)
ifneq ($(filter-out 0,$(CONCURRENCY_PROBE) $(STAGE_F_PROBE)),)
$(error SCENARIO cannot be combined with legacy probe selectors)
endif
endif
SCENARIO_KEY := $(or $(strip $(SCENARIO)),$(if $(filter 0,$(CONCURRENCY_PROBE)),$(or $(LEGACY_SCENARIO_STAGE_F),off),$(or $(LEGACY_SCENARIO_$(CONCURRENCY_PROBE)),invalid)))
SCENARIO_ID_off := 0
SCENARIO_ID_ordinary := 1
SCENARIO_ID_initrd := 2
SCENARIO_ID_trap := 3
SCENARIO_ID_remote := 4
SCENARIO_ID_publication := 5
SCENARIO_ID_report := 6
SCENARIO_ID_dispatch := 7
SCENARIO_ID := $(SCENARIO_ID_$(SCENARIO_KEY))
SUPPORTED_SCENARIOS := off ordinary initrd trap remote publication report dispatch
ifeq ($(filter $(SCENARIO_KEY),$(SUPPORTED_SCENARIOS)),)
$(error Unsupported scenario selector $(SCENARIO_KEY); use off/ordinary/initrd/trap/remote/publication/report/dispatch)
endif

# Verify tools exist early.
ifeq ($(shell command -v $(CC) 2>/dev/null),)
$(error C compiler not found: $(CC))
endif

ifeq ($(shell command -v $(CXX) 2>/dev/null),)
$(error C++ compiler not found: $(CXX))
endif

ifeq ($(shell command -v $(OBJDUMP) 2>/dev/null),)
$(error objdump not found: $(OBJDUMP))
endif

ifeq ($(shell command -v $(NM) 2>/dev/null),)
$(error nm not found: $(NM))
endif

ifeq ($(shell command -v $(READELF) 2>/dev/null),)
$(error readelf not found: $(READELF))
endif

RISCV_ARCH ?= rv64gc
RISCV_ABI  ?= lp64d
RISCV_ARCH_FLAGS := -march=$(RISCV_ARCH) -mabi=$(RISCV_ABI)
BUILD_REVISION := $(shell git rev-parse --short=12 HEAD 2>/dev/null || printf unknown)
BUILD_DIRTY := $(if $(shell git status --porcelain 2>/dev/null),dirty,clean)
BUILD_VARIANT := $(if $(filter-out 0,$(PANIC_PROBE)),-panic$(PANIC_PROBE),)-lock$(LOCK_DIAG)-conc$(CONCURRENCY_DIAG)
BUILD_ID := $(BUILD_REVISION)-$(BUILD_DIRTY)-$(ARCH)-$(PROFILE)$(BUILD_VARIANT)

# BUILD_DIR names the image output only.  Core and module objects are keyed
# independently so profile/diagnostic/scenario changes do not create another
# complete object tree.
BUILD_DIR ?= build/$(ARCH)/$(PROFILE)
TARGET    := $(BUILD_DIR)/kernel.elf
MAPFILE   := $(BUILD_DIR)/kernel.map
IMAGE_ID_FILE := $(BUILD_DIR)/image.id
# Make's timestamp graph cannot observe a command-line BUILD_ID change by
# itself.  Give each identity a distinct prerequisite instead of forcing the
# metadata object on every invocation; the stamp recipe updates the readable
# image.id only when this identity has not been seen in this build directory.
IMAGE_ID_KEY := $(subst /,_,$(subst :,_,$(BUILD_ID)))
IMAGE_ID_STAMP := $(BUILD_DIR)/.image-id-$(IMAGE_ID_KEY)
# The image target is reused while a selector/module key changes.  Keep one
# image-local composition state file whose content is compared on every make
# invocation; this forces a relink only when the selected module set or image
# identity actually changes, including switching back to an older selector.
IMAGE_LINK_INPUTS = build=$(BUILD_ID)|profile=$(PROFILE)|scenario=$(SCENARIO_KEY)|tests=$(ENABLE_TESTS)|toolchain=$(TOOLCHAIN_ID)|objs=$(OBJS)|ldflags=$(LDFLAGS)|script=$(LINKER_SCRIPT)
IMAGE_LINK_KEY = $(shell printf '%s' "$(IMAGE_LINK_INPUTS)" | sha256sum | cut -c1-16)
IMAGE_LINK_STATE := $(BUILD_DIR)/image.link
IMAGE_LINK_MANIFEST := $(BUILD_DIR)/image.link.config
LINKER_SCRIPT := arch/$(ARCH)/linker.ld
# Trace builds deliberately add one owner word to every tracked lock. Built-in
# tests construct dense object fixtures on their private kernel stacks, so
# audit that instrumentation profile against its own explicit bound while the
# production kernel keeps the original limit.
BOOT_STACK_FRAME_BUDGET := $(if $(filter 1,$(ENABLE_TESTS)),2048,1792)

USER_ARCH ?= rv64imac_zicsr_zifencei
USER_ABI ?= lp64
USER_ARCH_FLAGS := -march=$(USER_ARCH) -mabi=$(USER_ABI)
USER_BUILD_DIR := build/$(ARCH)/user
INIT_USER_TARGET := $(USER_BUILD_DIR)/init.elf
INIT_USER_MAPFILE := $(USER_BUILD_DIR)/init.map
PROOF_USER_TARGET := $(USER_BUILD_DIR)/proof.elf
PROOF_USER_MAPFILE := $(USER_BUILD_DIR)/proof.map
UART_USER_TARGET := $(USER_BUILD_DIR)/uart.elf
UART_USER_MAPFILE := $(USER_BUILD_DIR)/uart.map
USER_LINKER_SCRIPT := user/$(ARCH)/linker.ld
BOOT_BUNDLE := $(USER_BUILD_DIR)/boot.bundle
PROOF_BOOT_BUNDLE := $(USER_BUILD_DIR)/proof.bundle
HOST_BUILD_DIR := build/host
BOOTPACK := $(HOST_BUILD_DIR)/bootpack

COMMON_FLAGS := -ffreestanding -Wall -Wextra -O2 -g3 \
                $(RISCV_ARCH_FLAGS) \
                -mcmodel=medany -msmall-data-limit=0 \
                -I . \
                -I kernel \
                -I kernel/include \
                -I arch/$(ARCH)/include

CFLAGS   := $(COMMON_FLAGS) \
            -Werror=implicit-function-declaration

CXXFLAGS := $(COMMON_FLAGS) -std=gnu++2b \
            -fno-exceptions -fno-rtti \
            -fno-threadsafe-statics -fno-use-cxa-atexit \
            -fno-omit-frame-pointer -fno-optimize-sibling-calls \
            -fstack-usage

# Build identity is image metadata, not a common code-generation input. Only
# the dedicated metadata TU receives this define; changing PROFILE/build ID
# therefore does not invalidate compatible kernel objects.
IMAGE_BUILD_INFO_OBJ := $(BUILD_DIR)/kernel/image/build_info-$(IMAGE_ID_KEY).cpp.o
$(IMAGE_BUILD_INFO_OBJ): kernel/image/build_info.cpp $(IMAGE_ID_STAMP)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -DMYOS_BUILD_ID=\"$(BUILD_ID)\" -MMD -MP -c $< -o $@
$(IMAGE_ID_STAMP):
	@mkdir -p $(dir $@)
	@printf '%s\n' '$(BUILD_ID)' > $(IMAGE_ID_FILE)
	@printf '%s\n' '$(BUILD_ID)' > $@

ASFLAGS  := $(COMMON_FLAGS)

# Object identity is a content key over the effective common commands, not a
# profile/stage label. Include compiler executable/version and every language
# flag which can change generated core code or ABI. Keep a short readable
# prefix while the full command inputs are recorded in the per-store config.
TOOLCHAIN_ID := CC=$(shell command -v $(CC));$(shell $(CC) -dumpmachine);$(shell $(CC) -dumpfullversion)|CXX=$(shell command -v $(CXX));$(shell $(CXX) -dumpmachine);$(shell $(CXX) -dumpfullversion)|AS=$(shell command -v $(CC))
CORE_INPUTS := arch=$(ARCH)|isa=$(RISCV_ARCH)|abi=$(RISCV_ABI)|toolchain=$(TOOLCHAIN_ID)|cflags=$(CFLAGS)|cxxflags=$(CXXFLAGS)|asflags=$(ASFLAGS)
CORE_HASH := $(shell printf '%s' "$(CORE_INPUTS)" | sha256sum | cut -c1-16)
CORE_KEY := $(ARCH)-$(CORE_HASH)
CORE_DIR := build/core/$(CORE_KEY)
DIAG_INPUTS := core=$(CORE_INPUTS)|lock=$(LOCK_DIAG_LEVEL)|conc=$(CONCURRENCY_DIAG_LEVEL)|panic=$(PANIC_PROBE)|cxxflags=$(CXXFLAGS) -DMYOS_LOCK_DIAG=$(LOCK_DIAG_LEVEL) -DMYOS_CONCURRENCY_DIAG=$(CONCURRENCY_DIAG_LEVEL) -DMYOS_PANIC_PROBE=$(PANIC_PROBE)
DIAG_HASH := $(shell printf '%s' "$(DIAG_INPUTS)" | sha256sum | cut -c1-16)
DIAG_KEY := $(subst /,_,$(subst :,_,$(LOCK_DIAG)-$(CONCURRENCY_DIAG)-panic$(PANIC_PROBE)-$(DIAG_HASH)))
DIAG_DIR := build/module/diag/$(CORE_KEY)-$(DIAG_KEY)
SCENARIO_INPUTS := core=$(CORE_INPUTS)|cxxflags=$(CXXFLAGS)|selector-source=kernel/diag/scenario_real.cpp
SCENARIO_HASH := $(shell printf '%s' "$(SCENARIO_INPUTS)" | sha256sum | cut -c1-16)
SCENARIO_DIR := build/module/scenario/$(CORE_KEY)-$(SCENARIO_HASH)
TEST_INPUTS := core=$(CORE_INPUTS)|cxxflags=$(CXXFLAGS) -fomit-frame-pointer -foptimize-sibling-calls|tests=$(ENABLE_TESTS)
TEST_HASH := $(shell printf '%s' "$(TEST_INPUTS)" | sha256sum | cut -c1-16)
TEST_DIR := build/module/test/$(CORE_KEY)-$(TEST_HASH)
CORE_CONFIG := $(CORE_DIR)/config
DIAG_CONFIG := $(DIAG_DIR)/config
SCENARIO_CONFIG := $(SCENARIO_DIR)/config
TEST_CONFIG := $(TEST_DIR)/config
MODULE_CONFIGS := $(CORE_CONFIG) $(DIAG_CONFIG) $(SCENARIO_CONFIG) $(TEST_CONFIG)
VTABLE_ALLOWLIST_PREFIXES = $(CORE_DIR)/kernel/object/

USER_COMMON_FLAGS := -ffreestanding -Wall -Wextra -Werror -O2 -g3 \
                $(USER_ARCH_FLAGS) -mcmodel=medany -msmall-data-limit=0 \
                -I .
USER_CXXFLAGS := $(USER_COMMON_FLAGS) -std=gnu++2b \
                -fno-exceptions -fno-rtti \
                -fno-threadsafe-statics -fno-use-cxa-atexit
USER_ASFLAGS := $(USER_COMMON_FLAGS)
USER_LDFLAGS := $(USER_ARCH_FLAGS) -nostdlib \
                -Wl,-T,$(USER_LINKER_SCRIPT)

LDFLAGS  := $(RISCV_ARCH_FLAGS) \
            -nostdlib \
            -Wl,-T,$(LINKER_SCRIPT) \
            -Wl,-Map,$(MAPFILE)

ARCH_SRCS := \
  arch/riscv64/boot/entry.S \
  arch/riscv64/boot/early_entry.S \
  arch/riscv64/boot/high_entry.cpp \
  arch/riscv64/boot/kernel_image.cpp \
  arch/riscv64/cpu/local_entry.cpp \
  arch/riscv64/cpu/ipi.cpp \
  arch/riscv64/cpu/instruction.cpp \
  arch/riscv64/cpu/start.cpp \
  arch/riscv64/cpu/secondary_entry.S \
  arch/riscv64/context/kernel_context.cpp \
  arch/riscv64/context/kernel_context.S \
  arch/riscv64/sbi/call.cpp \
  arch/riscv64/sbi/console.cpp \
  arch/riscv64/sbi/system.cpp \
  arch/riscv64/time/clock.cpp \
  arch/riscv64/time/timer.cpp \
  arch/riscv64/uart/uart.cpp \
  arch/riscv64/mmu/sv39_builder.cpp \
  arch/riscv64/mmu/sv39_editor.cpp \
  arch/riscv64/mmu/range_map.cpp \
  arch/riscv64/mmu/initial_kernel_map.cpp \
  arch/riscv64/mmu/initial_page_table.cpp \
  arch/riscv64/trap/trap.S \
  arch/riscv64/trap/context.cpp \
  arch/riscv64/trap/event.cpp \
  arch/riscv64/trap/trap.cpp \
  arch/riscv64/trap/user.cpp

KERNEL_SRCS := \
  kernel/boot/boot.cpp \
  kernel/boot/cpu_topology.cpp \
  kernel/boot/timebase.cpp \
  kernel/boot/firmware/devicetree/fdt.cpp \
  kernel/image/boot_bundle.cpp \
  kernel/image/build_info.cpp \
  kernel/init/root_task.cpp \
  kernel/init/run.cpp \
  kernel/diag/console.cpp \
  kernel/diag/panic.cpp \
  kernel/diag/config.cpp \
  kernel/diag/concurrency.cpp \
  kernel/diag/flight.cpp \
  kernel/diag/report.cpp \
  kernel/diag/watch.cpp \
  kernel/core/kernel_state.cpp \
  kernel/cpu/cpu_runtime.cpp \
  kernel/cap/policy.cpp \
  kernel/cap/grant_graph.cpp \
  kernel/cap/cspace.cpp \
	kernel/ipc/notification.cpp \
	kernel/ipc/buffer.cpp \
	kernel/ipc/tunnel.cpp \
	kernel/ipc/channel.cpp \
  kernel/pager/pager.cpp \
  kernel/irq/irq.cpp \
  kernel/ipc/endpoint.cpp \
  kernel/ipc/transfer.cpp \
  kernel/object/object_ref.cpp \
  kernel/object/object_store.cpp \
	kernel/resource/pool.cpp \
	kernel/resource/allocation.cpp \
	kernel/sched/context.cpp \
	kernel/sched/authority.cpp \
	kernel/sched/refill_queue.cpp \
	kernel/sched/domain.cpp \
	kernel/sched/builtin_policy.cpp \
	kernel/sched/timer_queue.cpp \
	kernel/sched/remote_queue.cpp \
  kernel/sched/dispatcher.cpp \
  kernel/cpu/cpu_provisioner.cpp \
  kernel/cpu/cpu_registry.cpp \
  kernel/cpu/cpu_local.cpp \
  kernel/cpu/ipi.cpp \
  kernel/cpu/start.cpp \
	kernel/thread/thread.cpp \
    kernel/execution/authority.cpp \
    kernel/execution/target.cpp \
    kernel/execution/vproc.cpp \
    kernel/execution/vproc_notification.cpp \
    kernel/execution/vproc_tunnel.cpp \
	kernel/execution/stop.cpp \
  kernel/operation/completion.cpp \
  kernel/operation/wait.cpp \
  kernel/execution/execution.cpp \
  kernel/execution/binding.cpp \
  kernel/syscall/syscall.cpp \
  kernel/syscall/common.cpp \
  kernel/syscall/execution.cpp \
  kernel/syscall/capability.cpp \
  kernel/syscall/construction.cpp \
  kernel/syscall/object.cpp \
    kernel/syscall/notification.cpp \
    kernel/syscall/vproc.cpp \
    kernel/syscall/tunnel.cpp \
    kernel/syscall/endpoint.cpp \
  kernel/syscall/channel.cpp \
  kernel/syscall/pager_irq.cpp \
  kernel/syscall/terminal.cpp \
  kernel/syscall/vm.cpp \
  kernel/time/clock.cpp \
  kernel/mm/page_state.cpp \
  kernel/fault/terminal.cpp \
  kernel/fault/observation.cpp \
  kernel/mm/kernel_stack.cpp \
	kernel/mm/memory_object.cpp \
	kernel/mm/physical_alias.cpp \
	kernel/mm/vspace.cpp \
	kernel/mm/vspace_fault.cpp \
	kernel/mm/vspace_invalidation.cpp \
	kernel/mm/vspace_view.cpp \
	kernel/mm/vspace_layout.cpp \
	kernel/mm/vspace_mapping.cpp \
	kernel/mm/vspace_protection.cpp \
	kernel/mm/vspace_unmap.cpp \
	kernel/mm/vspace_work.cpp \
  kernel/mm/kernel_vspace.cpp \
  kernel/mm/translation.cpp \
  kernel/mm/direct_map.cpp \
  kernel/trap/dump.cpp \
  kernel/trap/trap.cpp \
  kernel/mm/boot_map.cpp \
  kernel/mm/pmm.cpp \
  libk/mem.c

# The lock facade is a diagnostic module provider.  It is linked in every
# image, including off/off, so callers use one stable out-of-line contract.
KERNEL_SRCS += kernel/sync/lock.cpp

TEST_SRCS := \
  test/framework.cpp \
  test/scenario.cpp \
  test/scenario_basic.cpp \
  test/scenario_remote.cpp \
  test/scenario_publication.cpp \
  test/scenario_report.cpp \
  test/libk_test.cpp \
  test/sync_test.cpp \
  test/allocator_test.cpp \
  test/bootinfo_test.cpp \
  test/boot_bundle_test.cpp \
  test/cpu_topology_test.cpp \
  test/sched_test.cpp \
  test/cap_test.cpp \
  test/memory_test.cpp \
  test/translation_test.cpp \
  test/vspace_test.cpp \
  test/user_test.cpp \
  test/ipc_test.cpp \
  test/e7_test.cpp

USER_RUNTIME_SRCS := \
  user/lib/crt0.S \
  user/lib/crt.cpp \
  user/riscv64/context.S \
  libk/mem.c
INIT_USER_SRCS := servers/init/main.cpp
PROOF_USER_SRCS := servers/proof/main.cpp
UART_USER_SRCS := servers/uart/main.cpp
USER_SRCS := $(USER_RUNTIME_SRCS) $(INIT_USER_SRCS) $(PROOF_USER_SRCS) $(UART_USER_SRCS)

USER_CPP_SRCS := $(filter %.cpp,$(USER_SRCS))
USER_C_SRCS := $(filter %.c,$(USER_SRCS))
USER_S_SRCS := $(filter %.S,$(USER_SRCS))
USER_CPP_OBJS := $(addprefix $(USER_BUILD_DIR)/,$(USER_CPP_SRCS:.cpp=.cpp.o))
USER_C_OBJS := $(addprefix $(USER_BUILD_DIR)/,$(USER_C_SRCS:.c=.c.o))
USER_S_OBJS := $(addprefix $(USER_BUILD_DIR)/,$(USER_S_SRCS:.S=.S.o))
USER_OBJS := $(USER_S_OBJS) $(USER_C_OBJS) $(USER_CPP_OBJS)
USER_DEPS := $(USER_OBJS:.o=.d)
USER_RUNTIME_OBJS := $(addprefix $(USER_BUILD_DIR)/,$(USER_RUNTIME_SRCS:=.o))
INIT_USER_OBJS := $(addprefix $(USER_BUILD_DIR)/,$(INIT_USER_SRCS:=.o))
PROOF_USER_OBJS := $(addprefix $(USER_BUILD_DIR)/,$(PROOF_USER_SRCS:=.o))
UART_USER_OBJS := $(addprefix $(USER_BUILD_DIR)/,$(UART_USER_SRCS:=.o))

IMAGE_SRCS := $(filter kernel/image/build_info.cpp,$(KERNEL_SRCS))
DIAG_SRCS := $(filter-out kernel/diag/scenario_%.cpp,$(filter kernel/diag/%.cpp kernel/sync/lock.cpp,$(KERNEL_SRCS)))
SCENARIO_SRCS :=
CORE_KERNEL_SRCS := $(filter-out $(IMAGE_SRCS) $(DIAG_SRCS) $(SCENARIO_SRCS),$(KERNEL_SRCS))
CORE_SRCS := $(ARCH_SRCS) $(CORE_KERNEL_SRCS)
ifeq ($(ENABLE_TESTS),1)
TEST_IMAGE_SRCS := $(TEST_SRCS)
else
TEST_IMAGE_SRCS :=
endif

CORE_C_SRCS := $(filter %.c,$(CORE_SRCS))
CORE_CPP_SRCS := $(filter %.cpp,$(CORE_SRCS))
CORE_S_SRCS := $(filter %.S,$(CORE_SRCS))
DIAG_CPP_SRCS := $(filter %.cpp,$(DIAG_SRCS))
SCENARIO_CPP_SRCS := $(filter %.cpp,$(SCENARIO_SRCS))
IMAGE_CPP_SRCS := $(filter %.cpp,$(IMAGE_SRCS))
TEST_CPP_SRCS := $(filter %.cpp,$(TEST_IMAGE_SRCS))

CORE_C_OBJS := $(addprefix $(CORE_DIR)/,$(CORE_C_SRCS:.c=.c.o))
CORE_CPP_OBJS := $(addprefix $(CORE_DIR)/,$(CORE_CPP_SRCS:.cpp=.cpp.o))
CORE_S_OBJS := $(addprefix $(CORE_DIR)/,$(CORE_S_SRCS:.S=.S.o))
DIAG_CPP_OBJS := $(addprefix $(DIAG_DIR)/,$(DIAG_CPP_SRCS:.cpp=.cpp.o))
SCENARIO_CPP_OBJS := $(addprefix $(SCENARIO_DIR)/,$(SCENARIO_CPP_SRCS:.cpp=.cpp.o))
SCENARIO_SELECTOR_OBJ := $(SCENARIO_DIR)/selector-$(SCENARIO_KEY).cpp.o
IMAGE_CPP_OBJS := $(IMAGE_BUILD_INFO_OBJ)
TEST_CPP_OBJS := $(addprefix $(TEST_DIR)/,$(TEST_CPP_SRCS:.cpp=.cpp.o))
TEST_PROVIDER_SRC := $(if $(filter 1,$(ENABLE_TESTS)),test/boot.cpp,kernel/diag/test_off.cpp)
TEST_PROVIDER_OBJ := $(TEST_DIR)/test_boot_provider-$(ENABLE_TESTS).cpp.o
REPORT_GATE_SRC := $(if $(filter 1,$(ENABLE_TESTS)),test/scenario_gate.cpp,kernel/diag/scenario_gate_off.cpp)
REPORT_GATE_OBJ := $(if $(filter 1,$(ENABLE_TESTS)),$(TEST_DIR)/scenario_gate.cpp.o,$(DIAG_DIR)/scenario_gate_off.cpp.o)

CORE_OBJS := $(CORE_S_OBJS) $(CORE_C_OBJS) $(CORE_CPP_OBJS)
MODULE_OBJS := $(DIAG_CPP_OBJS) $(SCENARIO_CPP_OBJS) $(SCENARIO_SELECTOR_OBJ) $(IMAGE_CPP_OBJS) $(TEST_PROVIDER_OBJ) $(REPORT_GATE_OBJ) $(TEST_CPP_OBJS)
OBJS := $(CORE_OBJS) $(MODULE_OBJS)
DEPS := $(OBJS:.o=.d)
CPP_OBJS := $(CORE_CPP_OBJS) $(DIAG_CPP_OBJS) $(SCENARIO_CPP_OBJS) $(IMAGE_CPP_OBJS) $(TEST_PROVIDER_OBJ) $(TEST_CPP_OBJS)
CPP_STACK_USAGE := $(CPP_OBJS:.o=.su)
CLANG_CPP_SRCS := $(CORE_CPP_SRCS) $(DIAG_CPP_SRCS) $(SCENARIO_CPP_SRCS) $(IMAGE_CPP_SRCS)

all: $(TARGET) audit-boot-stack

bundle: $(BOOT_BUNDLE) $(PROOF_BOOT_BUNDLE) $(UART_USER_TARGET) audit-user

$(TARGET): $(OBJS) $(LINKER_SCRIPT) FORCE | $(MODULE_CONFIGS)
	@key='$(IMAGE_LINK_KEY)'; old=''; newer='$(filter-out FORCE,$?)'; \
	if test -f "$(IMAGE_LINK_STATE)"; then old=$$(cat "$(IMAGE_LINK_STATE)"); fi; \
	if test "$$old" = "$$key" && test -z "$$newer"; then exit 0; fi; \
	mkdir -p $(dir $@); \
	$(CC) $(LDFLAGS) -o $@ $(OBJS) && { printf '%s\n' "$$key" > "$(IMAGE_LINK_STATE)"; printf '%s\n' "$(IMAGE_LINK_INPUTS)" > "$(IMAGE_LINK_MANIFEST)"; }

FORCE:

$(CORE_CONFIG): FORCE
	@mkdir -p $(dir $@); tmp="$@.tmp"; printf '%s\n' '$(CORE_INPUTS)' > "$$tmp"; \
	if test -r "$@" && cmp -s "$$tmp" "$@"; then rm -f "$$tmp"; else mv -f "$$tmp" "$@"; fi

$(DIAG_CONFIG): FORCE
	@mkdir -p $(dir $@); tmp="$@.tmp"; printf '%s\n' '$(DIAG_INPUTS)' > "$$tmp"; \
	if test -r "$@" && cmp -s "$$tmp" "$@"; then rm -f "$$tmp"; else mv -f "$$tmp" "$@"; fi

$(SCENARIO_CONFIG): FORCE
	@mkdir -p $(dir $@); tmp="$@.tmp"; printf '%s\n' '$(SCENARIO_INPUTS)|selected=$(SCENARIO_KEY)' > "$$tmp"; \
	if test -r "$@" && cmp -s "$$tmp" "$@"; then rm -f "$$tmp"; else mv -f "$$tmp" "$@"; fi

$(TEST_CONFIG): FORCE
	@mkdir -p $(dir $@); tmp="$@.tmp"; printf '%s\n' '$(TEST_INPUTS)' > "$$tmp"; \
	if test -r "$@" && cmp -s "$$tmp" "$@"; then rm -f "$$tmp"; else mv -f "$$tmp" "$@"; fi

$(CORE_DIR)/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(CORE_DIR)/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(CORE_DIR)/%.S.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

# Diagnostic providers are the only objects that receive level policy.  Core
# callers compile against the stable ABI with no provider-selection macro.
$(DIAG_DIR)/%.cpp.o: CXXFLAGS += \
            -DMYOS_PANIC_PROBE=$(PANIC_PROBE) \
            -DMYOS_LOCK_DIAG=$(LOCK_DIAG_LEVEL) \
            -DMYOS_CONCURRENCY_DIAG=$(CONCURRENCY_DIAG_LEVEL)
$(DIAG_DIR)/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(SCENARIO_DIR)/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

ifeq ($(SCENARIO_KEY),off)
SCENARIO_SELECTOR_SRC := kernel/diag/scenario_off.cpp
$(SCENARIO_SELECTOR_OBJ): $(SCENARIO_SELECTOR_SRC)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@
else
SCENARIO_SELECTOR_SRC := kernel/diag/scenario_real.cpp
$(SCENARIO_SELECTOR_OBJ): $(SCENARIO_SELECTOR_SRC)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -DMYOS_SCENARIO_ID=$(SCENARIO_ID) -MMD -MP -c $< -o $@
endif

$(BUILD_DIR)/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

# Built-in tests are a link-selected module and do not alter core frames.
$(TEST_DIR)/%.cpp.o: CXXFLAGS += \
            -fomit-frame-pointer -foptimize-sibling-calls
$(TEST_DIR)/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(TEST_PROVIDER_OBJ): $(TEST_PROVIDER_SRC)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -fomit-frame-pointer -foptimize-sibling-calls \
		-MMD -MP -c $< -o $@

$(REPORT_GATE_OBJ): $(REPORT_GATE_SRC)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -fomit-frame-pointer -foptimize-sibling-calls \
		-MMD -MP -c $< -o $@

$(INIT_USER_TARGET): $(USER_RUNTIME_OBJS) $(INIT_USER_OBJS) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(CC) $(USER_LDFLAGS) -Wl,-Map,$(INIT_USER_MAPFILE) \
		-o $@ $(USER_RUNTIME_OBJS) $(INIT_USER_OBJS)

$(PROOF_USER_TARGET): $(USER_RUNTIME_OBJS) $(PROOF_USER_OBJS) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(CC) $(USER_LDFLAGS) -Wl,-Map,$(PROOF_USER_MAPFILE) \
		-o $@ $(USER_RUNTIME_OBJS) $(PROOF_USER_OBJS)

$(UART_USER_TARGET): $(USER_RUNTIME_OBJS) $(UART_USER_OBJS) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(CC) $(USER_LDFLAGS) -Wl,-Map,$(UART_USER_MAPFILE) \
		-o $@ $(USER_RUNTIME_OBJS) $(UART_USER_OBJS)

$(USER_BUILD_DIR)/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(USER_CXXFLAGS) -MMD -MP -c $< -o $@

$(USER_BUILD_DIR)/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_COMMON_FLAGS) -MMD -MP -c $< -o $@

$(USER_BUILD_DIR)/%.S.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(USER_ASFLAGS) -MMD -MP -c $< -o $@

$(BOOTPACK): tools/bootpack/main.cpp uapi/boot_bundle.h
	@mkdir -p $(dir $@)
	$(HOST_CXX) -std=c++23 -O2 -Wall -Wextra -Werror -I . $< -o $@

$(BOOT_BUNDLE): $(INIT_USER_TARGET) $(PROOF_USER_TARGET) $(UART_USER_TARGET) $(BOOTPACK)
	$(BOOTPACK) $@ init init=$(INIT_USER_TARGET) proof=$(PROOF_USER_TARGET) uart=$(UART_USER_TARGET)

$(PROOF_BOOT_BUNDLE): $(PROOF_USER_TARGET) $(BOOTPACK)
	$(BOOTPACK) $@ proof proof=$(PROOF_USER_TARGET)

disasm: $(TARGET)
	$(OBJDUMP) -d $(TARGET) > $(BUILD_DIR)/kernel.disasm

symbols: $(TARGET)
	$(NM) -n $(TARGET) > $(BUILD_DIR)/kernel.sym

audit-symbols: $(TARGET)
	@echo "[audit] checking forbidden atomic runtime fallbacks..."
	@if $(NM) -u $(TARGET) | rg -n "(__atomic_|__sync_)"; then \
		echo "[audit] FAIL: non-lock-free atomic runtime symbol(s) found"; \
		exit 1; \
	else \
		echo "[audit] OK: no atomic runtime fallback symbols"; \
	fi
	@echo "[audit] checking forbidden undefined EH symbols..."
	@if $(NM) -u $(TARGET) | rg -n "(__gxx_personality_v0|__cxa_throw|__cxa_rethrow|__cxa_begin_catch|_Unwind_)"; then \
		echo "[audit] FAIL: forbidden undefined EH symbol(s) found"; \
		exit 1; \
	else \
		echo "[audit] OK: no forbidden undefined EH symbols"; \
	fi
	@echo "[audit] checking forbidden defined RTTI symbols..."
	@if $(NM) --defined-only -n $(TARGET) | rg -n "(_ZTI|_ZTS)"; then \
		echo "[audit] FAIL: forbidden defined RTTI symbol(s) found"; \
		exit 1; \
	else \
		echo "[audit] OK: no forbidden defined RTTI symbols"; \
	fi
	@if [ "$(CONCURRENCY_DIAG_LEVEL)" -eq 0 ]; then \
		echo "[audit] checking bounded off concurrency provider..."; \
		if ! $(NM) -C --defined-only -n $(TARGET) \
			| rg -q "kernel::diag::concurrency::FlightRecorder::(initialize|push|read)"; then \
			echo "[audit] FAIL: off recorder ABI stubs are not linked"; \
			exit 1; \
		fi; \
		if $(NM) -u -C $(TARGET) \
			| rg -q "kernel::diag::concurrency::FlightRecorder::"; then \
			echo "[audit] FAIL: off recorder ABI remains unresolved"; \
			exit 1; \
		fi; \
		if $(NM) -C --defined-only $(TARGET) \
			| rg -q "kernel::test::scenario::detail::"; then \
			echo "[audit] FAIL: scenario driver/state linked into off image"; \
			exit 1; \
		fi; \
		echo "[audit] OK: off recorder resolves to bounded ABI stubs"; \
	fi
	@echo "[audit] checking vtable whitelist..."
	@set -e; \
	violations=0; \
	for obj in $(OBJS); do \
		if $(NM) --defined-only -n "$$obj" | rg -q "_ZTV"; then \
			allowed=0; \
			for prefix in $(VTABLE_ALLOWLIST_PREFIXES); do \
				case "$$obj" in \
					$$prefix*) allowed=1 ;; \
				esac; \
			done; \
			if [ $$allowed -eq 1 ]; then \
				echo "[audit] allow vtable in $$obj"; \
			else \
				echo "[audit] FAIL: vtable symbol found outside whitelist: $$obj"; \
				$(NM) --defined-only -n "$$obj" | rg -n "_ZTV" || true; \
				violations=1; \
			fi; \
		fi; \
	done; \
	if [ $$violations -ne 0 ]; then \
		exit 1; \
	else \
		echo "[audit] OK: vtable whitelist check passed"; \
	fi

audit-boot-stack: $(CPP_OBJS)
	@echo "[audit] checking boot-stack frame budget ($(BOOT_STACK_FRAME_BUDGET) bytes)..."
	@set -e; \
	failed=0; \
	for report in $(CPP_STACK_USAGE); do \
		while IFS="$$(printf '\t')" read -r location frame kind; do \
			if [ "$$kind" != "static" ] || [ "$$frame" -gt $(BOOT_STACK_FRAME_BUDGET) ]; then \
				echo "[audit] FAIL: $$location\t$$frame\t$$kind"; \
				failed=1; \
			fi; \
		done < "$$report"; \
	done; \
	[ $$failed -eq 0 ]
	@echo "[audit] OK: all C++ frames are static and within budget"

audit-clang:
	@echo "[audit] checking Clang RISC-V freestanding syntax..."
	@set -e; \
	for source in $(CLANG_CPP_SRCS); do \
		$(CLANGXX) --target=riscv64-unknown-elf \
			$(COMMON_FLAGS) -std=gnu++2b \
			-fno-exceptions -fno-rtti -fno-threadsafe-statics \
			-fno-use-cxa-atexit -fsyntax-only "$$source"; \
	done
	@echo "[audit] OK: Clang syntax audit passed"

audit-user: $(INIT_USER_TARGET) $(PROOF_USER_TARGET) $(UART_USER_TARGET)
	@echo "[audit] checking independent user ELFs..."
	@set -e; \
	for image in $(INIT_USER_TARGET) $(PROOF_USER_TARGET) $(UART_USER_TARGET); do \
		if $(NM) -u "$$image" | rg -n "."; then \
			echo "[audit] FAIL: undefined user symbol(s) in $$image"; \
			exit 1; \
		fi; \
		if $(NM) --defined-only -n "$$image" | rg -n "(_ZTI|_ZTS|_ZTV)"; then \
			echo "[audit] FAIL: RTTI/vtable symbol(s) in $$image"; \
			exit 1; \
		fi; \
		if $(READELF) -A "$$image" | rg -q 'Tag_RISCV_arch:.*(_f|_d|_v)[0-9]'; then \
			echo "[audit] FAIL: $$image requires F/D/V state"; \
			$(READELF) -A "$$image"; \
			exit 1; \
		fi; \
	done
	@echo "[audit] OK: freestanding integer-only user ELF"

kernel:
	$(MAKE) PROFILE=kernel all

test:
	$(MAKE) PROFILE=test all

proof:
	$(MAKE) PROFILE=proof all bundle

panic:
	$(MAKE) PROFILE=kernel PANIC_PROBE=1 all

run: $(TARGET) audit-boot-stack
	$(QEMU) -machine virt -nographic -bios default -kernel $(TARGET)

run-timeout: $(TARGET) audit-boot-stack
	timeout --foreground 3s $(QEMU) -machine virt -nographic -bios default -kernel $(TARGET) || [ $$? -eq 124 ]

run-test-smp:
	$(MAKE) PROFILE=test _run-test-smp

_run-test-smp: $(TARGET) audit-boot-stack
	@set +e; \
	output=$$(mktemp); \
	timeout --foreground $(QEMU_TIMEOUT) $(QEMU) -machine virt -smp $(QEMU_SMP) -nographic -bios default -kernel $(TARGET) > "$$output" 2>&1; \
	status=$$?; \
	cat "$$output"; \
	if [ $$status -ne 124 ]; then \
		echo "[test] FAIL: QEMU status $$status (expected timeout 124)"; \
		rm -f "$$output"; \
		exit 1; \
	fi; \
	if ! rg -q "cpu: discovered=$(QEMU_SMP) prepared=0 starting=0 online=$(QEMU_SMP) failed=0" "$$output"; then \
		echo "[test] FAIL: expected all $(QEMU_SMP) harts Online"; \
		rm -f "$$output"; \
		exit 1; \
	fi; \
	if ! rg -q '^failed=0\r?$$' "$$output"; then \
		echo "[test] FAIL: builtin tests did not complete cleanly"; \
		rm -f "$$output"; \
		exit 1; \
	fi; \
	rm -f "$$output"; \
	echo "[test] OK: all tests passed and $(QEMU_SMP) harts reached Online"

run-proof-smp:
	$(MAKE) PROFILE=proof _run-proof-smp

run-smp-timeout: run-proof-smp

_run-proof-smp: $(TARGET) $(PROOF_BOOT_BUNDLE) audit-boot-stack audit-user
	@set +e; \
	output=$$(mktemp); \
	timeout --foreground $(QEMU_TIMEOUT) $(QEMU) -machine virt -smp $(QEMU_SMP) -nographic -bios default -kernel $(TARGET) -initrd $(PROOF_BOOT_BUNDLE) > "$$output" 2>&1; \
	status=$$?; \
	cat "$$output"; \
	if [ $$status -ne 124 ]; then \
		echo "[smp] FAIL: QEMU status $$status (expected timeout 124)"; \
		rm -f "$$output"; \
		exit 1; \
	fi; \
	if ! rg -q "cpu: discovered=$(QEMU_SMP) prepared=0 starting=0 online=$(QEMU_SMP) failed=0" "$$output"; then \
		echo "[smp] FAIL: expected all $(QEMU_SMP) harts Online"; \
		rm -f "$$output"; \
		exit 1; \
	fi; \
	if ! rg -q '^failed=0\r?$$' "$$output"; then \
		echo "[smp] FAIL: builtin tests did not complete cleanly"; \
		rm -f "$$output"; \
		exit 1; \
	fi; \
	if ! rg -q "root init: started" "$$output"; then \
		echo "[smp] FAIL: external root init did not start"; \
		rm -f "$$output"; \
		exit 1; \
	fi; \
	if ! rg -q "user: contained fault address=0x1000 after syscalls=5 active-vspace-cpus=1" "$$output"; then \
		echo "[smp] FAIL: independent proof did not complete its continuation chain"; \
		rm -f "$$output"; \
		exit 1; \
	fi; \
	rm -f "$$output"; \
	echo "[smp] OK: all $(QEMU_SMP) harts Online and external root init started"

run-e1-smp:
	$(MAKE) PROFILE=proof _run-e1-smp

_run-e1-smp: $(TARGET) $(BOOT_BUNDLE) audit-boot-stack audit-user
	@set +e; \
	output=$$(mktemp); \
	timeout --foreground $(QEMU_TIMEOUT) $(QEMU) -machine virt -smp $(QEMU_SMP) -nographic -bios default -kernel $(TARGET) -initrd $(BOOT_BUNDLE) > "$$output" 2>&1; \
	status=$$?; \
	cat "$$output"; \
	if [ $$status -ne 124 ]; then \
		echo "[e1] FAIL: QEMU status $$status (expected timeout 124)"; \
		rm -f "$$output"; \
		exit 1; \
	fi; \
	if ! rg -q "cpu: discovered=$(QEMU_SMP) prepared=0 starting=0 online=$(QEMU_SMP) failed=0" "$$output"; then \
		echo "[e1] FAIL: expected all $(QEMU_SMP) harts Online"; \
		rm -f "$$output"; \
		exit 1; \
	fi; \
	if ! rg -q '^failed=0\r?$$' "$$output"; then \
		echo "[e1] FAIL: builtin tests did not complete cleanly"; \
		rm -f "$$output"; \
		exit 1; \
	fi; \
	if ! rg -q "boot bundle: root=init" "$$output" \
		|| ! rg -q "user: contained fault address=0xe100" "$$output"; then \
		echo "[e1] FAIL: init did not load, run, and reclaim the proof task"; \
		rm -f "$$output"; \
		exit 1; \
	fi; \
	rm -f "$$output"; \
	echo "[e1] OK: init loaded and reclaimed the proof task on $(QEMU_SMP) harts"

run-panic-smp:
	$(MAKE) PROFILE=kernel PANIC_PROBE=1 _run-panic-smp

_run-panic-smp: $(TARGET) audit-boot-stack
	@set +e; \
	output=$$(mktemp); \
	timeout --foreground $(QEMU_TIMEOUT) $(QEMU) -machine virt -smp $(QEMU_SMP) -nographic -bios default -kernel $(TARGET) > "$$output" 2>&1; \
	status=$$?; \
	cat "$$output"; \
	if [ $$status -ne 0 ]; then \
		echo "[panic] FAIL: QEMU status $$status (expected shutdown 0)"; \
		rm -f "$$output"; \
		exit 1; \
	fi; \
	if ! rg -q "MYOS KERNEL PANIC" "$$output" \
		|| ! rg -q "context: call-site" "$$output" \
		|| ! rg -q "expression: false" "$$output" \
		|| rg -q "no acknowledgement" "$$output"; then \
		echo "[panic] FAIL: incomplete owner/peer diagnostic dump"; \
		rm -f "$$output"; \
		exit 1; \
	fi; \
	stopped=$$(rg -c 'cpu [0-9]+: stopped pc=' "$$output"); \
	if [ $$stopped -ne $$(( $(QEMU_SMP) - 1 )) ]; then \
		echo "[panic] FAIL: expected $$(( $(QEMU_SMP) - 1 )) stopped peers, got $$stopped"; \
		rm -f "$$output"; \
		exit 1; \
	fi; \
	rm -f "$$output"; \
	echo "[panic] OK: owner dump completed and all peers stopped"

run-panic-degraded-smp:
	$(MAKE) PROFILE=kernel PANIC_PROBE=2 _run-panic-degraded-smp

_run-panic-degraded-smp: $(TARGET) audit-boot-stack
	@set +e; \
	output=$$(mktemp); \
	timeout --foreground $(QEMU_TIMEOUT) $(QEMU) -machine virt -smp $(QEMU_SMP) -nographic -bios default -kernel $(TARGET) > "$$output" 2>&1; \
	status=$$?; \
	cat "$$output"; \
	if [ $$status -ne 0 ]; then \
		echo "[panic-degraded] FAIL: QEMU status $$status (expected shutdown 0)"; \
		rm -f "$$output"; \
		exit 1; \
	fi; \
	if ! rg -q "MYOS KERNEL PANIC" "$$output" \
		|| ! rg -q "no acknowledgement" "$$output"; then \
		echo "[panic-degraded] FAIL: bounded missing-peer report absent"; \
		rm -f "$$output"; \
		exit 1; \
	fi; \
	rm -f "$$output"; \
	echo "[panic-degraded] OK: transport failure produced a bounded partial dump"

run-scenario:
	$(MAKE) PROFILE=test SCENARIO=$(SCENARIO_KEY) \
		CONCURRENCY_DIAG=$(if $(filter publication report,$(SCENARIO_KEY)),watch,trace) \
		_run-scenario

_run-scenario: $(TARGET) $(if $(filter initrd,$(SCENARIO_KEY)),$(BOOT_BUNDLE))
	@set +e; \
	output=$$(mktemp); \
	initrd=""; \
	if [ "$(SCENARIO_KEY)" = initrd ]; then initrd="-initrd $(BOOT_BUNDLE)"; fi; \
	timeout --foreground $(QEMU_TIMEOUT) $(QEMU) -machine virt -smp $(QEMU_SMP) \
		-nographic -bios default -kernel $(TARGET) $$initrd > "$$output" 2>&1; \
	status=$$?; \
	cat "$$output"; \
	if [ $$status -ne 124 ]; then \
		echo "[scenario] FAIL: QEMU status $$status"; \
		rm -f "$$output"; \
		exit 1; \
	fi; \
	case "$(SCENARIO_KEY)" in \
		off) ;; \
		ordinary) marker="\\[scenario\\] ordinary ok";; \
		initrd) marker="\\[scenario\\] initrd ok";; \
		trap) marker="\\[scenario\\] trap ok";; \
		remote) marker="\\[scenario\\] remote-delivery ok";; \
		publication) marker="\\[scenario\\] publication ok";; \
		report) marker="\\[scenario\\] report-retry wake-credit ok";; \
		dispatch) marker="\\[scenario\\] dispatch-flight ok";; \
		esac; \
	if [ -n "$$marker" ] && ! rg -q "$$marker" "$$output"; then \
		echo "[scenario] FAIL: marker missing for $(SCENARIO_KEY)"; \
		rm -f "$$output"; \
		exit 1; \
	fi; \
	rm -f "$$output"; \
	echo "[scenario] OK: $(SCENARIO_KEY) on $(QEMU_SMP) harts"

# Legacy wrapper: numeric selectors are translated by the Make-level mapping
# and no longer select a kernel production image or old stage marker.
run-concurrency-probe:
	$(MAKE) CONCURRENCY_PROBE=0 STAGE_F_PROBE=0 \
		SCENARIO=$(SCENARIO_KEY) run-scenario

# Compatibility wrapper for the former Stage-F command.  The retained
# mechanism is the semantic dispatch/flight scenario; the old stage name is
# intentionally confined to this Make target.
run-stage-f-probe:
	$(MAKE) CONCURRENCY_PROBE=0 STAGE_F_PROBE=0 \
		SCENARIO=dispatch run-scenario

debug: $(TARGET) audit-boot-stack
	@echo "debug: waiting for GDB on $(GDB_HOST):$(GDB_PORT)"
	$(QEMU) -machine virt -nographic -bios default -kernel $(TARGET) -S -gdb tcp:$(GDB_HOST):$(GDB_PORT)

clean:
	rm -rf build kernel.elf kernel.map kernel.disasm kernel.sym

-include $(DEPS) $(USER_DEPS)

.PHONY: FORCE all bundle kernel test proof panic disasm symbols audit-symbols audit-boot-stack audit-clang audit-user run run-timeout run-test-smp _run-test-smp run-proof-smp run-smp-timeout _run-proof-smp run-e1-smp _run-e1-smp run-panic-smp _run-panic-smp run-panic-degraded-smp _run-panic-degraded-smp run-scenario _run-scenario run-concurrency-probe _run-concurrency-probe run-stage-f-probe debug clean
