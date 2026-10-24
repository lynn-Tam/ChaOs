.section .text.start, "ax", @progbits
.global _start
.hidden _start
.type _start, @function
_start:
    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop
    mv s0, sp
    mv a0, sp
    call crt_boot
    mv tp, a0
    addi sp, a0, -16
    call crt_tls_init
    mv a0, s0
    call crt_start
.size _start, . - _start

.section .text.rt_thread_start, "ax", @progbits
.global rt_thread_start
.hidden rt_thread_start
.type rt_thread_start, @function
rt_thread_start:
    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop
    mv tp, a1
    tail rt_thread_boot
.size rt_thread_start, . - rt_thread_start
