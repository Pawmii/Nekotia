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

    .limine_requests : {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    } :limine_requests

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .text : {
        *(.text .text.*)
    } :text

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    .init_array : {
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        __init_array_end = .;
    } :rodata

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .data : {
        *(.data .data.*)
    } :data

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    /* Last, so adding it in the second link pass moves nothing (scripts/gen-symbols.py --verify). */
    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .neko_symbols : {
        KEEP(*(.neko_symbols))
    } :symbols

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
        *(.comment)
    }
}
