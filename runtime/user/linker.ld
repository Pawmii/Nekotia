/* Nekotia user programs: static, non-PIE, loaded at 0x400000 (see neko/process.hpp).
   Each segment starts on its own page so the kernel can map it with exact permissions. */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS((1 << 0) | (1 << 2)); /* RX */
    rodata PT_LOAD FLAGS((1 << 2));            /* R  */
    data   PT_LOAD FLAGS((1 << 1) | (1 << 2)); /* RW */
}

SECTIONS
{
    . = 0x400000;
    .text : { *(.text .text.*) } :text

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .rodata : { *(.rodata .rodata.*) } :rodata
    .init_array : {
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        __init_array_end = .;
    } :rodata

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .data : { *(.data .data.*) } :data
    .bss : { *(.bss .bss.*) *(COMMON) } :data

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
        *(.comment)
    }
}
