/*
 * ld/boot.ld - A/B bootloader linker script
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  STM32F767ZIT6, dual-bank flash (nDBANK = 0, BASM-001)
 * SRS:     LAY-001, BCON-005, BNFR-001
 *
 * Dual-bank sectors (RM0410): each 1 MB bank has 4 x 16 KB, 1 x 64 KB and
 * 7 x 128 KB. BOOT owns bank 1 sectors 0-1 (0x08000000..0x08007FFF); the
 * metadata sector and slot A follow at 0x08008000 / 0x0800C000.
 */

ENTRY(Reset_Handler)

/* Main stack top: top of the BOOT RAM budget in DTCM. */
_estack = ORIGIN(RAM) + LENGTH(RAM);

MEMORY
{
    /* Bank 1 sectors 0-1 only (LAY-001, BCON-005). */
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 32K
    /* BNFR-001: at most 64 KB of RAM; the lower half of DTCM. */
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 64K
}

/* BCON-003: no heap. */
_Min_Heap_Size  = 0;
_Min_Stack_Size = 0x1000;

SECTIONS
{
    .isr_vector :
    {
        . = ALIGN(4);
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } > FLASH

    .text :
    {
        . = ALIGN(4);
        *(.text .text.*)
        *(.rodata .rodata.*)
        KEEP(*(.init))
        KEEP(*(.fini))
        . = ALIGN(4);
        _etext = .;
    } > FLASH

    .ARM.extab :
    {
        *(.ARM.extab* .gnu.linkonce.armextab.*)
    } > FLASH

    .ARM.exidx :
    {
        __exidx_start = .;
        *(.ARM.exidx*)
        __exidx_end = .;
    } > FLASH

    .preinit_array :
    {
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array*))
        PROVIDE_HIDDEN(__preinit_array_end = .);
    } > FLASH

    .init_array :
    {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array*))
        PROVIDE_HIDDEN(__init_array_end = .);
    } > FLASH

    .fini_array :
    {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array*))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } > FLASH

    _sidata = LOADADDR(.data);

    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data .data.*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH

    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        _sbss = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    ._user_heap_stack (NOLOAD) :
    {
        . = ALIGN(8);
        PROVIDE(end = .);
        . = . + _Min_Heap_Size;
        . = . + _Min_Stack_Size;
        . = ALIGN(8);
    } > RAM
}

/* LAY-001: the whole image (including the .data load image) stays inside
 * bank 1 sectors 0-1. The build fails otherwise. */
__boot_image_end = LOADADDR(.data) + SIZEOF(.data);
ASSERT(__boot_image_end <= 0x08008000,
       "LAY-001: BOOT image exceeds 32 KB (overlaps the metadata sector)")
