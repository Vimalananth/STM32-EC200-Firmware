/*
 * STM32G070KBTX_APP.ld — Application linker script for OTA dual-bank layout
 *
 * Flash layout (128 KB total):
 *   0x08000000 - 0x08001FFF  Bootloader (8 KB)   — NOT in this script
 *   0x08002000 - 0x0800FFFF  App Slot A (56 KB)  ← this app lives here
 *   0x08010000 - 0x0801DFFF  App Slot B (56 KB)  — OTA download target
 *   0x0801E000 - 0x0801FFFF  OTA Flags  (8 KB)   — boot decision flag
 */

ENTRY(Reset_Handler)

_estack = ORIGIN(RAM) + LENGTH(RAM);

_Min_Heap_Size  = 0x200;
_Min_Stack_Size = 0x400;

MEMORY
{
  RAM   (xrw) : ORIGIN = 0x20000000, LENGTH = 36K
  FLASH  (rx) : ORIGIN = 0x08002000, LENGTH = 56K
}

/* Symbols used by ota.c and bootloader */
OTA_SLOT_B_ADDR  = 0x08010000;
OTA_FLAGS_ADDR   = 0x0801E000;
OTA_PAGE_SIZE    = 2048;

SECTIONS
{
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } >FLASH

  .text :
  {
    . = ALIGN(4);
    *(.text)
    *(.text*)
    *(.glue_7)
    *(.glue_7t)
    *(.eh_frame)
    KEEP (*(.init))
    KEEP (*(.fini))
    . = ALIGN(4);
    _etext = .;
  } >FLASH

  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)
    *(.rodata*)
    . = ALIGN(4);
  } >FLASH

  .ARM.extab : {
    . = ALIGN(4);
    *(.ARM.extab* .gnu.linkonce.armextab.*)
    . = ALIGN(4);
  } >FLASH

  .ARM : {
    . = ALIGN(4);
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
    . = ALIGN(4);
  } >FLASH

  .preinit_array :
  {
    . = ALIGN(4);
    PROVIDE_HIDDEN (__preinit_array_start = .);
    KEEP (*(.preinit_array*))
    PROVIDE_HIDDEN (__preinit_array_end = .);
    . = ALIGN(4);
  } >FLASH

  .init_array :
  {
    . = ALIGN(4);
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP (*(SORT(.init_array.*)))
    KEEP (*(.init_array*))
    PROVIDE_HIDDEN (__init_array_end = .);
    . = ALIGN(4);
  } >FLASH

  .fini_array :
  {
    . = ALIGN(4);
    PROVIDE_HIDDEN (__fini_array_start = .);
    KEEP (*(SORT(.fini_array.*)))
    KEEP (*(.fini_array*))
    PROVIDE_HIDDEN (__fini_array_end = .);
    . = ALIGN(4);
  } >FLASH

  _sidata = LOADADDR(.data);

  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data)
    *(.data*)
    *(.RamFunc)
    *(.RamFunc*)
    . = ALIGN(4);
    _edata = .;
  } >RAM AT> FLASH

  . = ALIGN(4);
  .bss :
  {
    _sbss = .;
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
    __bss_end__ = _ebss;
  } >RAM

  /* .noinit at FIXED address 0x20007000 — NOT zero-initialised on reset,
   * survives NVIC_SystemReset and the bootloader (which only zeros its own
   * small .bss near 0x20000000).  Fixed address is CRITICAL: the OTA reboot
   * sentinel must be at the same address in every firmware version so old
   * firmware can write it and new firmware can read it after OTA swap.
   * 0x20007000 = 28 KB into RAM, safely above .bss (~6 KB) and 8 KB below
   * the stack top (0x20009000), leaving plenty of margin.                 */
  .noinit 0x20007000 (NOLOAD) :
  {
    . = ALIGN(4);
    KEEP(*(.noinit))
    KEEP(*(.noinit*))
    . = ALIGN(4);
  } >RAM

  ._user_heap_stack :
  {
    . = ALIGN(8);
    PROVIDE ( end = . );
    PROVIDE ( _end = . );
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } >RAM

  /DISCARD/ :
  {
    libc.a ( * )
    libm.a ( * )
    libgcc.a ( * )
  }

  .ARM.attributes 0 : { *(.ARM.attributes) }
}
