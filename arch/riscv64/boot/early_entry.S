/*
 * RISC-V high-half migration.
 *
 * Everything before arch_riscv64_high_entry lives in the physical .boot
 * segment and may address only physical .boot storage or absolute linker LMA
 * symbols.  The transitional root is deliberately not a kernel translation
 * owner: it exists only until KernelVSpace publishes and activates its final
 * root.
 */

.equ SV39_MODE,              0x8000000000000000
.equ PTE_V,                  0x001
.equ PTE_R,                  0x002
.equ PTE_W,                  0x004
.equ PTE_X,                  0x008
.equ PTE_A,                  0x040
.equ PTE_D,                  0x080
.equ PTE_TABLE,              PTE_V
.equ PTE_RX,                 PTE_V | PTE_R | PTE_X | PTE_A
.equ PTE_RW,                 PTE_V | PTE_R | PTE_W | PTE_A | PTE_D
.equ PAGE_SHIFT,             12
.equ MEGAPAGE_SHIFT,         21
.equ GIGAPAGE_SHIFT,         30
.equ DIRECT_ROOT_FIRST,      256
.equ DIRECT_ROOT_COUNT,      128
.equ KERNEL_ROOT_INDEX,      510
.equ KERNEL_VIRT_BASE,       0xffffffff80000000

.section .boot.stack, "aw", @nobits
.balign 16
.Lphysical_boot_stack:
    .space 4096
.Lphysical_boot_stack_top:

.section .boot.pgtables, "aw", @nobits
.balign 4096
.global arch_riscv64_boot_root
arch_riscv64_boot_root:
    .space 4096
.Lboot_low_l1:
    .space 4096
.Lboot_high_l1:
    .space 4096

.section .bootstack, "aw", @nobits
.balign 16
.global boot_stack_guard
boot_stack_guard:
    .space 256
.global boot_stack
boot_stack:
    .space 3840
.global boot_stack_top
boot_stack_top:

.section .text.boot, "ax", @progbits
.balign 4
.global arch_early_entry
.type arch_early_entry, @function
arch_early_entry:
    csrci sstatus, 2
    la sp, .Lphysical_boot_stack_top

    /* Preserve the SBI hart id and physical FDT address across construction. */
    mv s0, a0
    mv s1, a1

    /* Both the high BSS and the transitional tables are NOLOAD storage. */
    la t0, kernel_bss_lma_start
    la t1, kernel_bss_lma_end
.Lclear_high_bss:
    bgeu t0, t1, .Lclear_boot_tables
    sd zero, 0(t0)
    addi t0, t0, 8
    j .Lclear_high_bss

.Lclear_boot_tables:
    la t0, arch_riscv64_boot_root
    la t1, .Lboot_high_l1
    li t2, 4096
    add t1, t1, t2
.Lclear_table_word:
    bgeu t0, t1, .Lmap_identity
    sd zero, 0(t0)
    addi t0, t0, 8
    j .Lclear_table_word

    /* Keep the entire physical .boot window executable during migration. */
.Lmap_identity:
    la t0, arch_riscv64_boot_root
    la t1, .Lboot_low_l1
    srli t2, t1, PAGE_SHIFT
    slli t2, t2, 10
    ori t2, t2, PTE_TABLE
    li t3, 2
    slli t3, t3, 3
    add t3, t0, t3
    sd t2, 0(t3)

    li t2, 0x80200000
    srli t2, t2, PAGE_SHIFT
    slli t2, t2, 10
    ori t2, t2, PTE_RX
    li t3, 1
    slli t3, t3, 3
    add t3, t1, t3
    sd t2, 0(t3)

    /* Map the high-linked image with bounded 2 MiB leaves. */
    la t1, .Lboot_high_l1
    srli t2, t1, PAGE_SHIFT
    slli t2, t2, 10
    ori t2, t2, PTE_TABLE
    li t3, KERNEL_ROOT_INDEX
    slli t3, t3, 3
    add t3, t0, t3
    sd t2, 0(t3)

    la t2, kernel_text_lma_start
    la t3, kernel_phys_end
    li t4, 0x1fffff
    add t3, t3, t4
    srli t3, t3, MEGAPAGE_SHIFT
    slli t3, t3, MEGAPAGE_SHIFT
    li t4, 0
.Lmap_high_image:
    bgeu t2, t3, .Lmap_direct_window
    srli t5, t2, PAGE_SHIFT
    slli t5, t5, 10
    ori t5, t5, PTE_RW
    /* Transitional leaves are RWX; the final root applies section policy. */
    ori t5, t5, PTE_X
    slli t6, t4, 3
    add t6, t1, t6
    sd t5, 0(t6)
    li t6, 0x200000
    add t2, t2, t6
    addi t4, t4, 1
    j .Lmap_high_image

    /*
     * The migration root projects the policy window so high C++ can parse
     * the FDT and construct the exact RAM map.  DirectMap itself exposes only
     * BootMap-proven RAM, and the final root contains only those ranges.
     */
.Lmap_direct_window:
    li t1, DIRECT_ROOT_FIRST
    li t2, DIRECT_ROOT_COUNT
    li t3, 0
.Lmap_direct_gigapage:
    srli t4, t3, PAGE_SHIFT
    slli t4, t4, 10
    ori t4, t4, PTE_RW
    slli t5, t1, 3
    add t5, t0, t5
    sd t4, 0(t5)
    li t5, 0x40000000
    add t3, t3, t5
    addi t1, t1, 1
    addi t2, t2, -1
    bnez t2, .Lmap_direct_gigapage

    sfence.vma x0, x0
    srli t0, t0, PAGE_SHIFT
    li t1, SV39_MODE
    or t0, t0, t1
    csrw satp, t0
    sfence.vma x0, x0

    mv a0, s0
    mv a1, s1
    li t0, KERNEL_VIRT_BASE
    jr t0
.size arch_early_entry, .-arch_early_entry

/* The first high VMA in the image; the physical trampoline jumps here. */
.section .text.high_entry, "ax", @progbits
.balign 4
.global arch_riscv64_high_entry
.type arch_riscv64_high_entry, @function
arch_riscv64_high_entry:
    la sp, boot_stack_top

    la t0, boot_stack_guard
    la t1, boot_stack
    li t2, 0x42535441434b4755
.Lfill_boot_guard:
    sd t2, 0(t0)
    addi t0, t0, 8
    bltu t0, t1, .Lfill_boot_guard

    la t0, .Lhigh_park
    csrw stvec, t0
    call platform_riscv_virt_high_entry
.Lhigh_park:
    csrci sstatus, 2
1:
    wfi
    j 1b
.size arch_riscv64_high_entry, .-arch_riscv64_high_entry

.section .text, "ax", @progbits
.balign 4
.global arch_boot_stack_guard_intact
.type arch_boot_stack_guard_intact, @function
arch_boot_stack_guard_intact:
    la t0, boot_stack_guard
    la t1, boot_stack
    li t2, 0x42535441434b4755
.Lcheck_boot_guard:
    ld t3, 0(t0)
    bne t3, t2, .Lboot_guard_bad
    addi t0, t0, 8
    bltu t0, t1, .Lcheck_boot_guard
    li a0, 1
    ret
.Lboot_guard_bad:
    li a0, 0
    ret
.size arch_boot_stack_guard_intact, .-arch_boot_stack_guard_intact
