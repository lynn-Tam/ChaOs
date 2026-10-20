OUTPUT_ARCH(riscv)
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 0x200000;

    .text : ALIGN(0x1000) {
        KEEP(*(.text.start))
        *(.text .text.*)
    } :text

    .rodata : ALIGN(0x1000) {
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
        . = ALIGN(8);
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        __init_array_end = .;
    } :rodata

    .data : ALIGN(0x1000) {
        *(.data .data.*)
        *(.sdata .sdata.*)
        *(.got .got.*)
        *(.tdata .tdata.*)
        *(.tbss .tbss.*)
        PROVIDE(__global_pointer$ = . + 0x800);
    } :data

    .bss (NOLOAD) : ALIGN(0x1000) {
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
    } :data

    . = ALIGN(0x1000);
    __image_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame .eh_frame.*)
        *(.riscv.attributes)
    }
}
