#include <uapi/start.h>

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

.section .text._thread_start, "ax", @progbits
.global _thread_start
.hidden _thread_start
.type _thread_start, @function
_thread_start:
    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop
    mv tp, a1
    tail _thread_boot
.size _thread_start, . - _thread_start

.section .note.native, "a", @note
.balign 4
.long 5, 4, NATIVE_ABI_NOTE
.asciz NATIVE_ABI_NAME
.balign 4
.long NATIVE_ABI_VERSION
