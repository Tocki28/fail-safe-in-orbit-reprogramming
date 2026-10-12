/*
 * program.ld — linker script for every controller program (bootloader, test application).
 *
 * Run through the C preprocessor first (see build.mk), so the addresses come from layout.h.
 * The Makefile defines CODE_START and CODE_BYTES as names from layout.h, e.g.
 * CODE_START=SLOT_A_CODE_START. Only C-style comments here: the preprocessor removes them.
 *
 * Symbols _sidata, _sdata, _edata, _sbss, _ebss and _estack are the ones ST's startup file
 * (startup_stm32g431xx.s) uses to copy .data, clear .bss and set the stack.
 */
#include "layout.h"

ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = CODE_START, LENGTH = CODE_BYTES
    RAM   (xrw) : ORIGIN = RAM_START,  LENGTH = RAM_BYTES
}

/* The stack starts at the top of RAM and grows down. */
_estack = ORIGIN(RAM) + LENGTH(RAM);
/* RAM kept free below the stack top; the link fails if .data + .bss leave less. The deepest
 * call chain, the Ed25519 check, needs about 2 KB by the -fstack-usage (.su) files; 8 KB
 * leaves four times that. */
STACK_BYTES = 0x2000;

SECTIONS
{
    /* First, so the vector table sits at CODE_START, the address VTOR is given. */
    .isr_vector :
    {
        KEEP(*(.isr_vector))
    } > FLASH

    .text :
    {
        . = ALIGN(4);
        *(.text*)
        *(.rodata*)
        /* .init/.fini come with the C library start files that __libc_init_array calls. */
        KEEP(*(.init))
        KEEP(*(.fini))
        . = ALIGN(4);
    } > FLASH

    .ARM.exidx :
    {
        *(.ARM.exidx*)
    } > FLASH

    /* Constructor tables walked by __libc_init_array (called by ST's startup file). */
    .init_array :
    {
        . = ALIGN(4);
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array*))
        PROVIDE_HIDDEN(__preinit_array_end = .);
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array*))
        PROVIDE_HIDDEN(__init_array_end = .);
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array*))
        PROVIDE_HIDDEN(__fini_array_end = .);
        . = ALIGN(4);
    } > FLASH

    /* Initialised variables: live in RAM, their first values are stored in flash. */
    _sidata = LOADADDR(.data);
    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH

    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        _sbss = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    .stack_room (NOLOAD) :
    {
        . = . + STACK_BYTES;
    } > RAM
}
