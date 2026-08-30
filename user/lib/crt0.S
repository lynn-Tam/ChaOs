.section .text.start, "ax", @progbits
.global _start
.hidden _start
.type _start, @function
_start:
    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop

    /* The bootstrap ABI passes the info view in a0/a1. */
    mv s0, a0
    mv s1, a1
    call __myos_init
    mv a0, s0
    mv a1, s1
    call myos_main

    /* An accidental return is still the canonical implicit OK exit. */
    li a0, 0 /* MYOS_STATUS_OK */
    li a7, 1 /* MYOS_SYS_EXIT */
    ecall
1:
    j 1b
.size _start, . - _start
