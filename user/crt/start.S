.section .text.start, "ax", @progbits
.global _start
.hidden _start
.type _start, @function
_start:
    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop
    mv a0, sp
    call crt_start
.size _start, . - _start
