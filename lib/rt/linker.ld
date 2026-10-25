OUTPUT_ARCH(riscv)
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
    tls PT_TLS FLAGS(4);
    abi PT_NOTE FLAGS(4);
}

SECTIONS
{
    . = 0x200000;

    .text : ALIGN(0x1000) {
        KEEP(*(.text.start))
        *(.text .text.*)
    } :text

    .note.native : ALIGN(0x1000) { KEEP(*(.note.native)) } :rodata :abi

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
        PROVIDE(__global_pointer$ = . + 0x800);
    } :data

    .tdata : ALIGN(16) {
        __tls_start = .;
        *(.tdata .tdata.*)
        __tls_data_end = .;
    } :data :tls
    .tbss (NOLOAD) : {
        *(.tbss .tbss.*)
        __tls_end = .;
    } :data :tls
    __tls_size = __tls_end - __tls_start;
    __tls_align = MAX(16, MAX(ALIGNOF(.tdata), ALIGNOF(.tbss)));

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
