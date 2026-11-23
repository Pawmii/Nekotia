/* Higher-half kernel image for the Limine boot protocol. */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(kmain)

PHDRS
{
    limine_requests PT_LOAD FLAGS((1 << 1) | (1 << 2)); /* RW */
    text            PT_LOAD FLAGS((1 << 0) | (1 << 2)); /* RX */
    rodata          PT_LOAD FLAGS((1 << 2));            /* R  */
    data            PT_LOAD FLAGS((1 << 1) | (1 << 2)); /* RW */
    symbols         PT_LOAD FLAGS((1 << 2));            /* R: stop-screen symbol table, added in link pass 2 */
}

SECTIONS
{
    . = 0xffffffff80000000;
    __kernel_start = .;

    .limine_requests : {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    } :limine_requests

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    __text_start = .;
    .text : {
        *(.text .text.*)
    } :text
    __text_end = .;

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    __rodata_start = .;
    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    .init_array : {
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        __init_array_end = .;
    } :rodata
    __rodata_end = .;

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    __data_start = .;
    .data : {
        *(.data .data.*)
    } :data

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data
    __data_end = .;

    /* Last, so adding it in the second link pass moves nothing (scripts/gen-symbols.py --verify). */
    . = ALIGN(CONSTANT(MAXPAGESIZE));
    __symbols_start = .;
    .neko_symbols : {
        KEEP(*(.neko_symbols))
    } :symbols
    __symbols_end = .;

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
        *(.comment)
    }
}
