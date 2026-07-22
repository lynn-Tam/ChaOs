// arch/riscv64/cpu/local_entry.hpp
/*
这段入口代码职责不是在处理trap逻辑，而是把一次硬件trap安全落到内核可控的执行环境中。
1. 安装stvec之前，sscratch 必须已经指向一个有效的CpuEntryBlock
2. CpuEntryBlock 所在内存必须由内核拥有，并且在trap发生时已经映射可访问。
    trap.S最早期会直接通过sscratch访问，不能依赖 page fault
3. CpuEntryBlock里的entry sscratch 只用于“还没可行sp”这一小段窗口。
    用于暂存trap.S早期借用的寄存器原值。一旦TrapFrame构造完成，完整线程应当以TrapFrame为准。
4.如果trap前来自User mode, 则当前sp不可信。entry必须使用entry.active_stack_top作为可信栈
    然后在该栈中构造Trapframe.
5. 如果trap前来自Kernel mode， 则当前sp已经是kernel-owned stack entry可以继续压栈构造TrapFrame.
6. Trapframe 必须构造在selected stack上，且必须是内核可控栈，不是用户态的栈。
7. cpp trap handler只能接受安全落地的Trapframe
*/

#pragma once

#if !defined(__ASSEMBLER__)
#include <core/types.hpp>
#include <libk/typetraits.hpp>
#include <stddef.h>
#endif

#define RISCV64_SSTATUS_SPP_MASK 0x100

#define RISCV64_CPU_ENTRY_T0_OFFSET 0
#define RISCV64_CPU_ENTRY_T1_OFFSET 8
#define RISCV64_CPU_ENTRY_T2_OFFSET 16
#define RISCV64_CPU_ENTRY_SP_OFFSET 24
#define RISCV64_CPU_ENTRY_SIZE 32

#define RISCV64_CPU_ENTRY_OWNER_CPU_OFFSET 32
#define RISCV64_CPU_ENTRY_ACTIVE_STACK_TOP_OFFSET 40
#define RISCV64_CPU_ENTRY_TRAP_DEPTH_OFFSET 48
#define RISCV64_CPU_ENTRY_EMERGENCY_STACK_TOP_OFFSET 56
#define RISCV64_CPU_ENTRY_PANIC_SLOT_OFFSET 64
#define RISCV64_CPU_ENTRY_EMERGENCY_DEPTH_OFFSET 72
#define RISCV64_CPU_ENTRY_PANIC_STOP_OFFSET 80
#define RISCV64_CPU_ENTRY_TRAP_TICK_OFFSET 88
#define RISCV64_CPU_ENTRY_BLOCK_SIZE 96

#if !defined(__ASSEMBLER__)
namespace arch::riscv64 {

// Scratch for the no-trusted-stack window. This is not a partial TrapFrame.
struct EntryScratch final {
    /*保存trap之前的部分寄存器状态，因为trap.S要借用以下寄存器完成无栈操作*/
    usize t0{};
    usize t1{};
    usize t2{};
    usize sp{};
};

// CpuEntryBlock is the only CPU-local layout trap.S may know. It contains
// exactly the state consumed before a TrapFrame exists.
struct CpuEntryBlock final {
    EntryScratch scratch{};
    void* owner_cpu{};
    usize active_stack_top{};
    usize trap_depth{};
    usize emergency_stack_top{};
    void* panic_slot{};
    usize emergency_depth{};
    usize panic_stop_requested{};
    u64 trap_entry_tick{};

    void initialize(void* owner) noexcept {
        scratch = {};
        owner_cpu = owner;
        active_stack_top = 0;
        trap_depth = 0;
        emergency_stack_top = 0;
        panic_slot = nullptr;
        emergency_depth = 0;
        panic_stop_requested = 0;
        trap_entry_tick = 0;
    }

    void publish_active_stack(usize stack_top) noexcept {
        active_stack_top = stack_top;
    }

    void publish_diagnostics(usize stack_top, void* slot) noexcept {
        emergency_stack_top = stack_top;
        panic_slot = slot;
    }
};

static_assert(libk::is_standard_layout_v<EntryScratch>);
static_assert(libk::is_standard_layout_v<CpuEntryBlock>);
static_assert(libk::is_trivially_destructible_v<EntryScratch>);
static_assert(libk::is_trivially_destructible_v<CpuEntryBlock>);

static_assert(sizeof(EntryScratch) == RISCV64_CPU_ENTRY_SIZE);
static_assert(alignof(EntryScratch) == alignof(usize));
static_assert(offsetof(EntryScratch, t0) == RISCV64_CPU_ENTRY_T0_OFFSET);
static_assert(offsetof(EntryScratch, t1) == RISCV64_CPU_ENTRY_T1_OFFSET);
static_assert(offsetof(EntryScratch, t2) == RISCV64_CPU_ENTRY_T2_OFFSET);
static_assert(offsetof(EntryScratch, sp) == RISCV64_CPU_ENTRY_SP_OFFSET);

static_assert(sizeof(CpuEntryBlock) == RISCV64_CPU_ENTRY_BLOCK_SIZE);
static_assert(alignof(CpuEntryBlock) == alignof(usize));
static_assert(offsetof(CpuEntryBlock, scratch) + offsetof(EntryScratch, t0)
    == RISCV64_CPU_ENTRY_T0_OFFSET);
static_assert(offsetof(CpuEntryBlock, scratch) + offsetof(EntryScratch, t1)
    == RISCV64_CPU_ENTRY_T1_OFFSET);
static_assert(offsetof(CpuEntryBlock, scratch) + offsetof(EntryScratch, t2)
    == RISCV64_CPU_ENTRY_T2_OFFSET);
static_assert(offsetof(CpuEntryBlock, scratch) + offsetof(EntryScratch, sp)
    == RISCV64_CPU_ENTRY_SP_OFFSET);
static_assert(offsetof(CpuEntryBlock, owner_cpu)
    == RISCV64_CPU_ENTRY_OWNER_CPU_OFFSET);
static_assert(offsetof(CpuEntryBlock, active_stack_top)
    == RISCV64_CPU_ENTRY_ACTIVE_STACK_TOP_OFFSET);
static_assert(offsetof(CpuEntryBlock, trap_depth)
    == RISCV64_CPU_ENTRY_TRAP_DEPTH_OFFSET);
static_assert(offsetof(CpuEntryBlock, emergency_stack_top)
    == RISCV64_CPU_ENTRY_EMERGENCY_STACK_TOP_OFFSET);
static_assert(offsetof(CpuEntryBlock, panic_slot)
    == RISCV64_CPU_ENTRY_PANIC_SLOT_OFFSET);
static_assert(offsetof(CpuEntryBlock, emergency_depth)
    == RISCV64_CPU_ENTRY_EMERGENCY_DEPTH_OFFSET);
static_assert(offsetof(CpuEntryBlock, panic_stop_requested)
    == RISCV64_CPU_ENTRY_PANIC_STOP_OFFSET);
static_assert(offsetof(CpuEntryBlock, trap_entry_tick)
    == RISCV64_CPU_ENTRY_TRAP_TICK_OFFSET);

} // namespace arch::riscv64
#endif
