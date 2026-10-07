.section .text.start, "ax", @progbits
.global _start
.hidden _start
.type _start, @function
_start:
    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop

    /* The bootstrap ABI passes the resource view in a0/a1, argument bytes in a2/a3. */
    mv s0, a0
    mv s1, a1
    mv s2, a2
    mv s3, a3
    call run_constructors
    mv a0, s0
    mv a1, s1
    mv a2, s2
    mv a3, s3
#ifdef LIBSYS
    call program_start
#else
    call user_main
#endif

    /* An accidental return is still the canonical implicit OK exit. */
    li a0, 0 /* STATUS_OK */
    li a7, 1 /* SYS_EXIT */
    ecall
1:
    j 1b
.size _start, . - _start
