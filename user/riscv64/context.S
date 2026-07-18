.section .text, "ax", @progbits
.global user_enter
.type user_enter, @function
user_enter:
    mv t0, a0
    mv sp, a1
    mv a0, a2
    mv a1, a3
    la ra, .Luser_entry_returned
    jr t0

.Luser_entry_returned:
    li a7, 1 /* MYOS_SYS_EXIT */
    ecall
1:
    j 1b
.size user_enter, . - user_enter
