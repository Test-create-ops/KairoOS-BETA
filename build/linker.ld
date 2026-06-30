OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

PHDRS
{
    headers PT_LOAD FLAGS(4);
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
    note PT_NOTE;
}

SECTIONS
{
    . = 1M;

    .multiboot ALIGN(8) : {
        *(.multiboot)
    } :headers

    .pvh_note ALIGN(4) : {
        *(.pvh_note)
    } :headers :note

    .text ALIGN(4K) : {
        *(.text*)
    } :text

    .rodata ALIGN(4K) : {
        *(.rodata*)
        *(.comment*)
        *(.eh_frame*)
    } :text

    .data ALIGN(4K) : {
        *(.data*)
    } :data

    .bss ALIGN(4K) : {
        *(.bss*)
        *(COMMON)
    } :data
}