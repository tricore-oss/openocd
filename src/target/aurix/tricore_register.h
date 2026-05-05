/* SPDX-License-Identifier: GPL-2.0-or-later */

/***************************************************************************
 *   TriCore Register Definitions for Infineon AURIX                       *
 *   Copyright (C) 2026 Infineon Technologies AG                           *
 ***************************************************************************/

#ifndef TRICORE_REGISTER_H
#define TRICORE_REGISTER_H

/* Register defines would go here based on the provided table
 * Virtualization register*/
#define TRICORE_VCON0 0xB000
#define TRICORE_VCON1 0xB004
#define TRICORE_VCON2 0xB008
#define TRICORE_BHV 0xB010

/* Status */
#define TRICORE_ICR 0xFE2C
#define TRICORE_SYSCON 0xFE14
#define TRICORE_CPU_ID 0xFE18
#define TRICORE_BOOTCON 0xFE60
#define TRICORE_TTCON 0xFE6C

/* Debug registers */
#define TRICORE_DBGSR 0xFD00
#define TRICORE_EXEVT 0xFD08
#define TRICORE_CREVT 0xFD0C
#define TRICORE_SWEVT 0xFD10
#define TRICORE_DBGACT 0xFD14
#define TRICORE_TRIG_ACC 0xFD30
#define TRICORE_DMS 0xFD40
#define TRICORE_DCX 0xFD44
#define TRICORE_DBGTCR 0xFD48
#define TRICORE_DBGCFG 0xFD4C

/* Trigger event registers */
#define TRICORE_TRXEVT(x) (0xF000 + (x) * 0x8)
#define TRICORE_TRXADR(x) (0xF004 + (x) * 0x8)

#endif
