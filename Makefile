ARCH ?= riscv64
PROFILE ?= kernel

SUPPORTED_ARCHES := riscv64
SUPPORTED_PROFILES := kernel test proof

ifeq ($(filter $(ARCH),$(SUPPORTED_ARCHES)),)
$(error Unsupported ARCH=$(ARCH); supported: $(SUPPORTED_ARCHES))
endif
ifeq ($(filter $(PROFILE),$(SUPPORTED_PROFILES)),)
$(error Unsupported PROFILE=$(PROFILE); supported: $(SUPPORTED_PROFILES))
endif

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
PANIC_PROBE ?= 0
GDB_HOST ?= 127.0.0.1
GDB_PORT ?= 1237

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

VTABLE_ALLOWLIST_PREFIXES = $(BUILD_DIR)/kernel/object/

RISCV_ARCH ?= rv64gc
RISCV_ABI  ?= lp64d
RISCV_ARCH_FLAGS := -march=$(RISCV_ARCH) -mabi=$(RISCV_ABI)
BUILD_REVISION := $(shell git rev-parse --short=12 HEAD 2>/dev/null || printf unknown)
BUILD_DIRTY := $(if $(shell git status --porcelain 2>/dev/null),dirty,clean)
BUILD_VARIANT := $(if $(filter-out 0,$(PANIC_PROBE)),-panic$(PANIC_PROBE),)
BUILD_ID := $(BUILD_REVISION)-$(BUILD_DIRTY)-$(ARCH)-$(PROFILE)$(BUILD_VARIANT)

BUILD_DIR := build/$(ARCH)/$(PROFILE)$(BUILD_VARIANT)
TARGET    := $(BUILD_DIR)/kernel.elf
MAPFILE   := $(BUILD_DIR)/kernel.map
LINKER_SCRIPT := arch/$(ARCH)/linker.ld
BOOT_STACK_FRAME_BUDGET := 1792

USER_ARCH ?= rv64imac_zicsr_zifencei
USER_ABI ?= lp64
USER_ARCH_FLAGS := -march=$(USER_ARCH) -mabi=$(USER_ABI)
USER_BUILD_DIR := build/$(ARCH)/user
INIT_USER_TARGET := $(USER_BUILD_DIR)/init.elf
INIT_USER_MAPFILE := $(USER_BUILD_DIR)/init.map
PROOF_USER_TARGET := $(USER_BUILD_DIR)/proof.elf
PROOF_USER_MAPFILE := $(USER_BUILD_DIR)/proof.map
USER_LINKER_SCRIPT := user/$(ARCH)/linker.ld
BOOT_BUNDLE := $(USER_BUILD_DIR)/boot.bundle
PROOF_BOOT_BUNDLE := $(USER_BUILD_DIR)/proof.bundle
HOST_BUILD_DIR := build/host
BOOTPACK := $(HOST_BUILD_DIR)/bootpack

COMMON_FLAGS := -ffreestanding -Wall -Wextra -O2 -g3 \
                -DMYOS_PANIC_PROBE=$(PANIC_PROBE) \
                -DMYOS_BUILTIN_TESTS=$(ENABLE_TESTS) \
                -DMYOS_BUILD_ID=\"$(BUILD_ID)\" \
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

ASFLAGS  := $(COMMON_FLAGS)

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
  kernel/init/root_task.cpp \
  kernel/init/run.cpp \
  kernel/diag/console.cpp \
  kernel/diag/panic.cpp \
  kernel/core/kernel_state.cpp \
  kernel/cap/policy.cpp \
  kernel/cap/grant_graph.cpp \
  kernel/cap/cspace.cpp \
	kernel/ipc/notification.cpp \
	kernel/ipc/buffer.cpp \
	kernel/ipc/tunnel.cpp \
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
  kernel/syscall/vm.cpp \
  kernel/time/clock.cpp \
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

TEST_SRCS := \
  test/framework.cpp \
  test/libk_test.cpp \
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
  test/ipc_test.cpp

USER_RUNTIME_SRCS := \
  user/lib/crt0.S \
  user/lib/crt.cpp \
  user/riscv64/context.S \
  libk/mem.c
INIT_USER_SRCS := servers/init/main.cpp
PROOF_USER_SRCS := servers/proof/main.cpp
USER_SRCS := $(USER_RUNTIME_SRCS) $(INIT_USER_SRCS) $(PROOF_USER_SRCS)

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

SRCS := $(ARCH_SRCS) $(KERNEL_SRCS)
ifeq ($(ENABLE_TESTS),1)
SRCS += $(TEST_SRCS)
endif

C_SRCS   := $(filter %.c,$(SRCS))
CPP_SRCS := $(filter %.cpp,$(SRCS))
S_SRCS   := $(filter %.S,$(SRCS))

C_OBJS   := $(addprefix $(BUILD_DIR)/,$(C_SRCS:.c=.c.o))
CPP_OBJS := $(addprefix $(BUILD_DIR)/,$(CPP_SRCS:.cpp=.cpp.o))
S_OBJS   := $(addprefix $(BUILD_DIR)/,$(S_SRCS:.S=.S.o))

OBJS := $(S_OBJS) $(C_OBJS) $(CPP_OBJS)
DEPS := $(OBJS:.o=.d)
CPP_STACK_USAGE := $(CPP_OBJS:.o=.su)
CLANG_CPP_SRCS := $(filter-out test/%,$(CPP_SRCS))

all: $(TARGET) audit-boot-stack

bundle: $(BOOT_BUNDLE) $(PROOF_BOOT_BUNDLE) audit-user

$(TARGET): $(OBJS) $(LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $(OBJS)

$(BUILD_DIR)/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

# Built-in tests do not participate in panic backtraces and keep the original
# frame budget; runtime kernel code retains auditable frame chains.
$(BUILD_DIR)/test/%.cpp.o: CXXFLAGS += \
            -fomit-frame-pointer -foptimize-sibling-calls

$(BUILD_DIR)/%.S.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(INIT_USER_TARGET): $(USER_RUNTIME_OBJS) $(INIT_USER_OBJS) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(CC) $(USER_LDFLAGS) -Wl,-Map,$(INIT_USER_MAPFILE) \
		-o $@ $(USER_RUNTIME_OBJS) $(INIT_USER_OBJS)

$(PROOF_USER_TARGET): $(USER_RUNTIME_OBJS) $(PROOF_USER_OBJS) $(USER_LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(CC) $(USER_LDFLAGS) -Wl,-Map,$(PROOF_USER_MAPFILE) \
		-o $@ $(USER_RUNTIME_OBJS) $(PROOF_USER_OBJS)

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

$(BOOT_BUNDLE): $(INIT_USER_TARGET) $(PROOF_USER_TARGET) $(BOOTPACK)
	$(BOOTPACK) $@ init init=$(INIT_USER_TARGET) proof=$(PROOF_USER_TARGET)

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

audit-user: $(INIT_USER_TARGET) $(PROOF_USER_TARGET)
	@echo "[audit] checking independent user ELFs..."
	@set -e; \
	for image in $(INIT_USER_TARGET) $(PROOF_USER_TARGET); do \
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
	timeout --foreground 5s $(QEMU) -machine virt -smp $(QEMU_SMP) -nographic -bios default -kernel $(TARGET) > "$$output" 2>&1; \
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
	timeout --foreground 5s $(QEMU) -machine virt -smp $(QEMU_SMP) -nographic -bios default -kernel $(TARGET) -initrd $(PROOF_BOOT_BUNDLE) > "$$output" 2>&1; \
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
	timeout --foreground 8s $(QEMU) -machine virt -smp $(QEMU_SMP) -nographic -bios default -kernel $(TARGET) -initrd $(BOOT_BUNDLE) > "$$output" 2>&1; \
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
	timeout --foreground 5s $(QEMU) -machine virt -smp $(QEMU_SMP) -nographic -bios default -kernel $(TARGET) > "$$output" 2>&1; \
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
	timeout --foreground 5s $(QEMU) -machine virt -smp $(QEMU_SMP) -nographic -bios default -kernel $(TARGET) > "$$output" 2>&1; \
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

debug: $(TARGET) audit-boot-stack
	@echo "debug: waiting for GDB on $(GDB_HOST):$(GDB_PORT)"
	$(QEMU) -machine virt -nographic -bios default -kernel $(TARGET) -S -gdb tcp:$(GDB_HOST):$(GDB_PORT)

clean:
	rm -rf build kernel.elf kernel.map kernel.disasm kernel.sym

-include $(DEPS) $(USER_DEPS)

.PHONY: all bundle kernel test proof panic disasm symbols audit-symbols audit-boot-stack audit-clang audit-user run run-timeout run-test-smp _run-test-smp run-proof-smp run-smp-timeout _run-proof-smp run-e1-smp _run-e1-smp run-panic-smp _run-panic-smp run-panic-degraded-smp _run-panic-degraded-smp debug clean
