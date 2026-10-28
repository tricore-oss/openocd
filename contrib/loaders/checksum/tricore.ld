/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2026 Infineon Technologies AG
 */
ENTRY(main)

MEMORY
{
  DSPR (rwx) : ORIGIN = 0x70000000, LENGTH = 0x00040000
  PSPR (rx)  : ORIGIN = 0x70100000, LENGTH = 0x00010000
}
SECTIONS
{
  .vectors : ALIGN(256)
  {
    __tricore_vectors = .;
    KEEP(*(.vectors))
  } > PSPR

  .text :
  {
    *(.text .text.*)
    . = ALIGN(4);
  } > PSPR

  .exit :
  {
    __tricore_exit = .;
    KEEP(*(.exit))
  } > PSPR

  .data :
  {
    *(.rodata .rodata.*)
    *(.data .data.*)
  } > DSPR

  .bss :
  {
    *(.bss .bss.*)
    *(COMMON)
  } > DSPR
}
