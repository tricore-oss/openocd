/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2026 Infineon Technologies AG
 */
#ifndef TC4X_RRAM_H
#define TC4X_RRAM_H

#include <stdint.h>

#if (defined(TC4X_RRAM_PNVM) + defined(TC4X_RRAM_DNVM)) != 1
#error "define exactly one of TC4X_RRAM_PNVM/TC4X_RRAM_DNVM"
#endif

/* CPU0 STM (TC48x) */
#define CPU0_STM_ABS_ADDR 0xF8800020u

/* {PMUR,DMUR}.UR register offsets from the module base */
#define RRAM_UR_NVMADDR 0x3Cu
#define RRAM_UR_WDATA 0x40u
#define RRAM_UR_REQUEST 0x74u
#define RRAM_UR_REQSTAT 0x78u
#define RRAM_UR_CLRSTAT 0x7Cu

/* UR.REQUEST bits */
#define RRAM_REQUEST_INITWR (1u << 0)
#define RRAM_REQUEST_STWR (1u << 1)

/* UR.REQSTAT bits */
#define RRAM_REQSTAT_BUSY (1u << 0)
#define RRAM_REQSTAT_DONE (1u << 1)
#define RRAM_REQSTAT_ERROR_MASK 0x0018FFFCu

/* UR.CLRSTAT bits */
#define RRAM_CLRSTAT_CPROTER (1u << 0)
#define RRAM_CLRSTAT_COPER (1u << 1)
#define RRAM_CLRSTAT_CWVER (1u << 2)
#define RRAM_CLRSTAT_CMARDER (1u << 3)
#define RRAM_CLRSTAT_CNOMRDER (1u << 4)
#define RRAM_CLRSTAT_CTMRADER (1u << 5)
#define RRAM_CLRSTAT_CWLRADER (1u << 6)
#define RRAM_CLRSTAT_CWRMER (1u << 7)
#define RRAM_CLRSTAT_CDONE (1u << 9)
#define RRAM_CLRSTAT_CSNOOZER (1u << 10)

#define RRAM_CLRSTAT_COMMON                                                    \
  (RRAM_CLRSTAT_CPROTER | RRAM_CLRSTAT_COPER | RRAM_CLRSTAT_CWVER |            \
   RRAM_CLRSTAT_CMARDER | RRAM_CLRSTAT_CNOMRDER | RRAM_CLRSTAT_CTMRADER |      \
   RRAM_CLRSTAT_CDONE | RRAM_CLRSTAT_CSNOOZER)

#if defined(TC4X_RRAM_PNVM)
#define PAGE_SIZE 32u
#define RRAM_CLRSTAT_MASK RRAM_CLRSTAT_COMMON
#elif defined(TC4X_RRAM_DNVM)
#define PAGE_SIZE 8u
#define RRAM_CLRSTAT_MASK                                                      \
  (RRAM_CLRSTAT_COMMON | RRAM_CLRSTAT_CWLRADER | RRAM_CLRSTAT_CWRMER)
#endif

#endif /* TC4X_RRAM_H */
