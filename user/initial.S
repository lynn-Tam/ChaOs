#include <uapi/syscall.h>
#include <uapi/arch/riscv64/syscall_abi.h>
#include <uapi/status.h>
#include <uapi/capability.h>
#include <uapi/vm.h>

//Confirmatory experiment.
// Exit condition: replace this proof payload with the first real userspace
// boot image once a user program build/package boundary exists.

.section .user.image, "ax", @progbits
.global initial_user_entry
.hidden initial_user_entry
initial_user_entry:
    // a0: VSpace cap, a1: mapped stack base, a2: stack MemoryObject cap,
    // a3: a secondary-CPU peer was prepared.
    mv s0, a0
    mv s1, a1
    mv s2, a2
    mv s4, a3

    li a7, MYOS_SYS_YIELD
    ecall

    // A prepared peer may still fail firmware bring-up, so this confirmatory
    // barrier is bounded. Seeing the store proves the peer reached U-mode
    // with this same VSpace active; it then remains in a leaf loop.
    beqz s4, 3f
    li t1, 1000000
4:
    ld t0, 0(s1)
    bnez t0, 3f
    addi t1, t1, -1
    bnez t1, 4b
3:

    // Build a child Region and a capability-backed alias of the stack page.
    // The resulting Mapping is a persistent attachment to the stack Grant.
    mv a0, s0
    li a1, 0x600000
    li a2, 4096
    li a3, (MYOS_VM_READ | MYOS_VM_WRITE)
    li a4, MYOS_VM_NORMAL
    li a5, MYOS_RIGHT_MAP
    li a7, MYOS_SYS_VM_CREATE_REGION
    ecall
    bnez a0, 1f
    mv s3, a1

    mv a0, s3
    mv a1, s2
    li a2, 0x600000
    li a3, 4096
    li a4, 0
    li a5, (MYOS_VM_READ | MYOS_VM_WRITE)
    li a7, MYOS_SYS_VM_MAP
    ecall
    beqz a0, 5f
    li t0, MYOS_STATUS_PENDING
    bne a0, t0, 1f
5:

    // include_source=1 invalidates that Mapping. The syscall may return only
    // after the caller blocked and VSpace/PTE retirement completed.
    mv a0, s2
    li a1, 1
    li a7, MYOS_SYS_CAP_REVOKE
    ecall
    bnez a0, 1f

    // Reaching this instruction proves the original TrapFrame survived both
    // a cooperative dispatch and the asynchronous revoke block/wake path.
    mv a0, s0
    mv a1, s1
    li a2, 4096
    li a3, (MYOS_VM_READ | MYOS_VM_WRITE)
    li a7, MYOS_SYS_VM_PROTECT
    ecall
    beqz a0, 5f
    li t0, MYOS_STATUS_PENDING
    bne a0, t0, 1f
5:

    // The low guard has no Mapping. Terminate policy must exit only this
    // Thread and leave the kernel/idle execution alive.
    li t0, 0x1000
    ld t1, 0(t0)

1:
    li a7, MYOS_SYS_EXIT
    ecall
2:
    j 2b

.global initial_user_peer
.hidden initial_user_peer
initial_user_peer:
    // a0: shared ready word in the main user's stack mapping.
    li t0, 1
    sd t0, 0(a0)
    fence rw, rw
3:
    j 3b
