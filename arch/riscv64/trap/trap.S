//arch/riscv64/trap/trap.S

#include "arch/riscv64/trap/trapframe.hpp"
#include "arch/riscv64/cpu/local_entry.hpp"

.section .text, "ax", @progbits
.balign 4

.global arch_riscv64_trap_entry
.hidden arch_riscv64_trap_entry
.type arch_riscv64_trap_entry, @function
arch_riscv64_trap_entry:
    /*
    The first instructions run before any stack is trusted. sscratch must hold
    CpuEntryBlock*. csrrw gives us that entry view in t0 while preserving the
    interrupted t0 value in sscratch long enough to copy it into entry scratch.
    */
    csrrw t0, sscratch, t0
    sd sp, RISCV64_CPU_ENTRY_SP_OFFSET(t0)
    sd t1, RISCV64_CPU_ENTRY_T1_OFFSET(t0)
    sd t2, RISCV64_CPU_ENTRY_T2_OFFSET(t0)
    csrr t1, sscratch
    sd t1, RISCV64_CPU_ENTRY_T0_OFFSET(t0)
    csrw sscratch, t0

    ld t1, RISCV64_CPU_ENTRY_TRAP_DEPTH_OFFSET(t0)
    addi t1, t1, 1
    sd t1, RISCV64_CPU_ENTRY_TRAP_DEPTH_OFFSET(t0)

    /*
    A trap taken while another trap is active no longer trusts the interrupted
    stack. The first nested trap claims the emergency stack; any further trap
    parks without resetting its top and overwriting live panic frames.
    */
    li t2, 1
    bgtu t1, t2, .Lnested_trap

    /*
    Stack choice is based on previous privilege, not on the trap cause. Kernel
    origin can keep the interrupted kernel-owned sp. User origin must land on a
    kernel-owned stack supplied through CpuEntryBlock.
    */
    csrr t1, sstatus
    andi t2, t1, RISCV64_SSTATUS_SPP_MASK
    bnez t2, 1f
    ld sp, RISCV64_CPU_ENTRY_ACTIVE_STACK_TOP_OFFSET(t0)
    j 2f
1:
    ld sp, RISCV64_CPU_ENTRY_SP_OFFSET(t0)
2:
    addi sp, sp, -RISCV64_TRAP_FRAME_SIZE

    // The borrowed registers are restored into the TrapFrame from entry scratch.
    ld t1, RISCV64_CPU_ENTRY_T0_OFFSET(t0)
    sd t1, T0_OFFSET(sp)
    ld t1, RISCV64_CPU_ENTRY_T1_OFFSET(t0)
    sd t1, T1_OFFSET(sp)
    ld t1, RISCV64_CPU_ENTRY_T2_OFFSET(t0)
    sd t1, T2_OFFSET(sp)
    ld t1, RISCV64_CPU_ENTRY_SP_OFFSET(t0)
    sd t1, SP_OFFSET(sp)

    //保存gpr寄存器
    sd ra, RA_OFFSET(sp)
    sd gp, GP_OFFSET(sp)
    sd tp, TP_OFFSET(sp)
    sd s0, S0_OFFSET(sp)
    sd s1, S1_OFFSET(sp)
    sd a0, A0_OFFSET(sp)
    sd a1, A1_OFFSET(sp)
    sd a2, A2_OFFSET(sp)
    sd a3, A3_OFFSET(sp)
    sd a4, A4_OFFSET(sp)
    sd a5, A5_OFFSET(sp)
    sd a6, A6_OFFSET(sp)
    sd a7, A7_OFFSET(sp)
    sd s2, S2_OFFSET(sp)
    sd s3, S3_OFFSET(sp)
    sd s4, S4_OFFSET(sp)
    sd s5, S5_OFFSET(sp)
    sd s6, S6_OFFSET(sp)
    sd s7, S7_OFFSET(sp)
    sd s8, S8_OFFSET(sp)
    sd s9, S9_OFFSET(sp)
    sd s10, S10_OFFSET(sp)
    sd s11, S11_OFFSET(sp)
    sd t3, T3_OFFSET(sp)
    sd t4, T4_OFFSET(sp)
    sd t5, T5_OFFSET(sp)
    sd t6, T6_OFFSET(sp)


    /*
    LEARN:在riscv 中进入trap时候，硬件主要自动做这些事情：
        - 把当前pc存到sepc
        - 把trap原因写进scause
        - 把辅助错误地址写道stval
        - 修改sstatus里的状态位
        - pc跳到stvec
        - 切换权限模式
    这里需要保存写好的信息到普通内存结构中方便C++ handler读取以及避免嵌套中断
    造成CSR丢失
    */
    csrr t0, sepc
    sd t0, SEPC_OFFSET(sp)
    csrr t0, sstatus
    sd t0, SSTATUS_OFFSET(sp)
    csrr t0, scause
    sd t0, SCAUSE_OFFSET(sp)
    csrr t0, stval
    sd t0, STVAL_OFFSET(sp)

    // The handler returns the selected user-return frame.  Cross-domain
    // continuations may replace the interrupted frame before the common exit
    // hook observes scheduler and wait state.
    mv a0, sp
    call arch_riscv64_trap_handler
    mv sp, a0
    /*---- 恢复边界 ----*/
    // sscratch is restored to CpuEntryBlock before C++ runs, so return can update
    // trap_depth without depending on any frame-resident temporary register.
    csrr t0, sscratch
    ld t1, RISCV64_CPU_ENTRY_TRAP_DEPTH_OFFSET(t0)
    addi t1, t1, -1
    sd t1, RISCV64_CPU_ENTRY_TRAP_DEPTH_OFFSET(t0)

    /*
    The selected TrapFrame remains on its owning stack. A future context
    switch may resume at this call site before restoring this same frame.
    */
    mv a0, sp
    call arch_riscv64_trap_exit
    // The selected-arch exit hook may redirect this return to a synthetic
    // frame on another guarded kernel stack (Endpoint Activation).
    mv sp, a0

.Lrestore_frame:
    //NOTE: sepc和sstatus要写回csr,因为sret真正依赖他们
    ld t0, SEPC_OFFSET(sp)
    csrw sepc, t0
    ld t0, SSTATUS_OFFSET(sp)
    csrw sstatus, t0

    ld ra, RA_OFFSET(sp)
    ld gp, GP_OFFSET(sp)
    ld tp, TP_OFFSET(sp)
    ld t1, T1_OFFSET(sp)
    ld t2, T2_OFFSET(sp)
    ld s0, S0_OFFSET(sp)
    ld s1, S1_OFFSET(sp)
    ld a0, A0_OFFSET(sp)
    ld a1, A1_OFFSET(sp)
    ld a2, A2_OFFSET(sp)
    ld a3, A3_OFFSET(sp)
    ld a4, A4_OFFSET(sp)
    ld a5, A5_OFFSET(sp)
    ld a6, A6_OFFSET(sp)
    ld a7, A7_OFFSET(sp)
    ld s2, S2_OFFSET(sp)
    ld s3, S3_OFFSET(sp)
    ld s4, S4_OFFSET(sp)
    ld s5, S5_OFFSET(sp)
    ld s6, S6_OFFSET(sp)
    ld s7, S7_OFFSET(sp)
    ld s8, S8_OFFSET(sp)
    ld s9, S9_OFFSET(sp)
    ld s10, S10_OFFSET(sp)
    ld s11, S11_OFFSET(sp)
    ld t3, T3_OFFSET(sp)
    ld t4, T4_OFFSET(sp)
    ld t5, T5_OFFSET(sp)
    ld t6, T6_OFFSET(sp)

    /*
    NOTE: sp 必须很晚恢复。因为现在的 sp 是 TrapFrame 基址
          t0 也要很晚恢复。因为它在恢复 CSR 时还被当 scratch 用
    */
    ld t0, T0_OFFSET(sp)
    ld sp, SP_OFFSET(sp)
    sret

.Lnested_trap:
    ld t1, RISCV64_CPU_ENTRY_EMERGENCY_DEPTH_OFFSET(t0)
    bnez t1, .Ldouble_trap_park
    li t1, 1
    sd t1, RISCV64_CPU_ENTRY_EMERGENCY_DEPTH_OFFSET(t0)
    ld sp, RISCV64_CPU_ENTRY_EMERGENCY_STACK_TOP_OFFSET(t0)
    beqz sp, .Ldouble_trap_park
    j 2b

.Ldouble_trap_park:
    csrci sstatus, 2
    /*
    No stack is trustworthy here. Emit one fixed legacy-SBI line directly;
    the structured C++ double-panic path prints CPU/PC/cause when a valid
    emergency frame still exists.
    */
    li a7, 1
    li a0, '\n'
    ecall
    li a0, 'D'
    ecall
    li a0, 'O'
    ecall
    li a0, 'U'
    ecall
    li a0, 'B'
    ecall
    li a0, 'L'
    ecall
    li a0, 'E'
    ecall
    li a0, ' '
    ecall
    li a0, 'T'
    ecall
    li a0, 'R'
    ecall
    li a0, 'A'
    ecall
    li a0, 'P'
    ecall
    li a0, '\n'
    ecall
3:
    wfi
    j 3b
.size arch_riscv64_trap_entry, .-arch_riscv64_trap_entry

.global arch_riscv64_resume_user
.hidden arch_riscv64_resume_user
.type arch_riscv64_resume_user, @function
arch_riscv64_resume_user:
    // a0 names the synthetic TrapFrame built on the Thread home stack.
    mv sp, a0
    j .Lrestore_frame
.size arch_riscv64_resume_user, .-arch_riscv64_resume_user
