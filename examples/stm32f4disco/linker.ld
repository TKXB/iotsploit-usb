/*
 * Linker script for STM32F407VGT6 (STM32F4-Discovery).
 *
 * Flash: 1 MB at 0x08000000
 * SRAM:  128 KB at 0x20000000  (USB OTG FS cannot access CCM at 0x10000000,
 *        so all USB buffers must live in this 128 KB region.)
 *
 * Entry point is reset_handler, provided by libopencm3's vector.c.
 * Symbol names (_data, _edata, _ebss, _data_loadaddr, _stack, __init_array_*)
 * must match what libopencm3's reset_handler expects.
 */

ENTRY(reset_handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 1024K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
}

/* Stack pointer initial value: top of RAM (used by libopencm3 vector table) */
_stack = ORIGIN(RAM) + LENGTH(RAM);

SECTIONS
{
    .text :
    {
        KEEP(*(.vectors))      /* vector table from libopencm3 vector.c */
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH

    /* C++ / newlib init/fini arrays */
    .preinit_array :
    {
        PROVIDE(__preinit_array_start = .);
        KEEP(*(.preinit_array*))
        PROVIDE(__preinit_array_end = .);
    } > FLASH

    .init_array :
    {
        PROVIDE(__init_array_start = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array*))
        PROVIDE(__init_array_end = .);
    } > FLASH

    .fini_array :
    {
        PROVIDE(__fini_array_start = .);
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array*))
        PROVIDE(__fini_array_end = .);
    } > FLASH

    .ARM.exidx :
    {
        *(.ARM.exidx*)
        . = ALIGN(4);
        _etext = .;
    } > FLASH

    .data : AT (_etext)
    {
        _data = .;
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > RAM

    .bss :
    {
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    /* Heap boundary for newlib _sbrk (nosys stub) */
    . = ALIGN(8);
    _end = .;
    PROVIDE(end = .);

    _data_loadaddr = _etext;
}
