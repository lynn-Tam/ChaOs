ENTRY(_start)

PAGE_SIZE       = 0x1000;
MEGAPAGE_SIZE   = 0x200000;
BOOT_PHYS_BASE  = 0x80200000;
KERNEL_PHYS_BASE = 0x80400000;
KERNEL_VIRT_BASE = 0xffffffff80000000;

SECTIONS
{
    /*
     * The SBI enters this segment with translation disabled.  It contains
     * only position-independent migration code, one physical stack and the
     * transitional page tables.  No C++ object is reachable through it.
     */
    . = BOOT_PHYS_BASE;
    kernel_phys_start = .;
    boot_phys_start = .;

    .boot.text ALIGN(PAGE_SIZE) : {
        KEEP(*(.text.entry))
        *(.text.boot)
        *(.rodata.boot)
        . = ALIGN(PAGE_SIZE);
    }

    . = BOOT_PHYS_BASE + 0x10000;
    .boot.secondary ALIGN(PAGE_SIZE) : {
        KEEP(*(.boot.secondary))
        . = ALIGN(PAGE_SIZE);
    }

    .boot.bss ALIGN(PAGE_SIZE) (NOLOAD) : {
        boot_reclaim_start = .;
        *(.boot.stack)
        *(.boot.pgtables)
        . = ALIGN(PAGE_SIZE);
        boot_reclaim_end = .;
    }
    boot_phys_end = .;

    /*
     * All ordinary kernel code is linked in the high half while its ELF load
     * addresses stay in RAM.  The 2 MiB alignment lets the physical
     * trampoline use bounded megapage leaves before the final root exists.
     */
    . = KERNEL_VIRT_BASE;
    kernel_img_start = .;

    .text ALIGN(MEGAPAGE_SIZE) : AT(KERNEL_PHYS_BASE) {
        kernel_text_start = .;
        KEEP(*(.text.high_entry))
        *(.text .text.*)
        . = ALIGN(PAGE_SIZE);
        kernel_text_end = .;
    }
    kernel_text_lma_start = LOADADDR(.text);
    kernel_text_lma_end = LOADADDR(.text) + SIZEOF(.text);

    .user_image ALIGN(PAGE_SIZE) :
        AT(kernel_text_lma_end) {
        user_image_start = .;
        KEEP(*(.user.image))
        . = ALIGN(PAGE_SIZE);
        user_image_end = .;
    }
    user_image_lma_start = LOADADDR(.user_image);
    user_image_lma_end = LOADADDR(.user_image) + SIZEOF(.user_image);

    .rodata ALIGN(PAGE_SIZE) :
        AT(user_image_lma_end) {
        kernel_rodata_start = .;
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
        . = ALIGN(8);
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        __init_array_end = .;
        . = ALIGN(PAGE_SIZE);
        kernel_rodata_end = .;
    }
    kernel_rodata_lma_start = LOADADDR(.rodata);
    kernel_rodata_lma_end = LOADADDR(.rodata) + SIZEOF(.rodata);

    .data ALIGN(PAGE_SIZE) :
        AT(kernel_rodata_lma_end) {
        kernel_data_start = .;
        *(.data .data.*)
        *(.sdata .sdata.*)
        *(.got .got.*)
        . = ALIGN(PAGE_SIZE);
        kernel_data_end = .;
    }
    kernel_data_lma_start = LOADADDR(.data);
    kernel_data_lma_end = LOADADDR(.data) + SIZEOF(.data);

    .bss ALIGN(PAGE_SIZE) (NOLOAD) :
        AT(kernel_data_lma_end) {
        kernel_bss_start = .;
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        . = ALIGN(PAGE_SIZE);
        kernel_bss_end = .;
    }
    kernel_bss_lma_start = LOADADDR(.bss);
    kernel_bss_lma_end = LOADADDR(.bss) + SIZEOF(.bss);

    .bootstack ALIGN(PAGE_SIZE) (NOLOAD) :
        AT(kernel_bss_lma_end) {
        kernel_bootstack_start = .;
        KEEP(*(.bootstack))
        . = ALIGN(PAGE_SIZE);
        kernel_bootstack_end = .;
    }
    kernel_bootstack_lma_start = LOADADDR(.bootstack);
    kernel_bootstack_lma_end = LOADADDR(.bootstack) + SIZEOF(.bootstack);

    kernel_img_end = .;
    kernel_img_end_aligned = .;
    kernel_phys_end = kernel_bootstack_lma_end;
}

ASSERT(boot_phys_end <= KERNEL_PHYS_BASE,
       "physical trampoline exceeds its reserved load window")
ASSERT(ADDR(.boot.secondary) == BOOT_PHYS_BASE + 0x10000,
       "secondary physical entry address changed")
ASSERT(SIZEOF(.boot.text) == PAGE_SIZE,
       "primary physical trampoline must occupy one page")
ASSERT(SIZEOF(.boot.secondary) == PAGE_SIZE,
       "secondary physical trampoline must occupy one page")
ASSERT(boot_reclaim_start == BOOT_PHYS_BASE + 0x11000,
       "transitional resource base changed")
ASSERT((boot_reclaim_end - boot_reclaim_start) == 4 * PAGE_SIZE,
       "transitional resources must occupy four pages")
ASSERT((kernel_img_start % MEGAPAGE_SIZE) == 0,
       "high kernel image must be megapage aligned")
ASSERT((KERNEL_PHYS_BASE % MEGAPAGE_SIZE) == 0,
       "kernel physical image must be megapage aligned")
ASSERT((kernel_img_end % PAGE_SIZE) == 0,
       "kernel image must end on a page boundary")
ASSERT((kernel_phys_end - KERNEL_PHYS_BASE) <= 0x40000000,
       "bootstrap mapper supports at most one GiB of high kernel image")
ASSERT(kernel_rodata_lma_start ==
       KERNEL_PHYS_BASE + (kernel_rodata_start - KERNEL_VIRT_BASE),
       "kernel VMA/LMA delta must remain affine")
ASSERT(user_image_lma_start ==
       KERNEL_PHYS_BASE + (user_image_start - KERNEL_VIRT_BASE),
       "user image VMA/LMA delta must remain affine")
ASSERT((user_image_end - user_image_start) == 0 ||
       (user_image_end - user_image_start) == PAGE_SIZE,
       "optional initial user image must be empty or exactly one page")
ASSERT(kernel_data_lma_start ==
       KERNEL_PHYS_BASE + (kernel_data_start - KERNEL_VIRT_BASE),
       "kernel VMA/LMA delta must remain affine")
ASSERT(kernel_bss_lma_start ==
       KERNEL_PHYS_BASE + (kernel_bss_start - KERNEL_VIRT_BASE),
       "kernel VMA/LMA delta must remain affine")
ASSERT(kernel_bootstack_lma_start ==
       KERNEL_PHYS_BASE + (kernel_bootstack_start - KERNEL_VIRT_BASE),
       "kernel VMA/LMA delta must remain affine")
ASSERT((kernel_bootstack_end - kernel_bootstack_start) == PAGE_SIZE,
       "boot stack must occupy exactly one page")
