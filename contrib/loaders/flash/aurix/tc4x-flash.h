/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2026 Infineon Technologies AG
 */
#ifndef TC4X_FLASH_H
#define TC4X_FLASH_H

#include <stdint.h>

#include "tricore-mmio.h"

#if (defined(TC4X_FLASH_PFLASH) + defined(TC4X_FLASH_DFLASH) +                 \
     defined(TC4X_FLASH_PFLASHCS) + defined(TC4X_FLASH_DFLASHCS)) != 1
#error                                                                         \
    "define exactly one of TC4X_FLASH_PFLASH/TC4X_FLASH_DFLASH/TC4X_FLASH_PFLASHCS/TC4X_FLASH_DFLASHCS"
#endif

#define DMU_HCI_STATUS_ADDR 0xF8040004u
#define DMU_HCI_OCONTROL_ADDR 0xF804000Cu
#define DMU_HCI_ERR_ADDR 0xF8040010u
#define DMU_CSCI_STATUS_ADDR 0xF8040084u
#define DMU_CSCI_OCONTROL_ADDR 0xF804008Cu
#define DMU_CSCI_ERR_ADDR 0xF8040090u
#define CPU0_STM_ABS_ADDR 0xF8800020u

/* DMU_{HCI,CSCI}_STATUS bits */
#define DMU_STATUS_BUSYHOSTDF (1u << 16)
#define DMU_STATUS_BUSYCSRMDF (1u << 17)
#define DMU_STATUS_BUSYCSRMPF (1u << 18)
#define DMU_STATUS_DFPAGE (1u << 24)
#define DMU_STATUS_PFPAGE (1u << 25)
#define DMU_STATUS_REQDONE (1u << 31)
/* PFLASH bank busy bits [11:0] */
#define DMU_STATUS_PFBUSY 0xFFFu
/* Any bank busy on the host interface */
#define DMU_STATUS_HOST_ANYBUSY (DMU_STATUS_PFBUSY | DMU_STATUS_BUSYHOSTDF)
/* Any bank busy on the CSRM interface */
#define DMU_STATUS_CS_ANYBUSY                                                  \
  (DMU_STATUS_PFBUSY | DMU_STATUS_BUSYCSRMDF | DMU_STATUS_BUSYCSRMPF)

/* DMU_{HCI,CSCI}_OCONTROL bits */
#define DMU_OCONTROL_PRMODE (1u << 9)

static inline void __attribute__((always_inline))
flash_set_prmode(uintptr_t addr) {
  mmio_write_u32(addr, mmio_read_u32(addr) | DMU_OCONTROL_PRMODE);
}

#if defined(TC4X_FLASH_PFLASH) || defined(TC4X_FLASH_DFLASH)
#define HOST_CMD_ADDR 0xF8080000u
#elif defined(TC4X_FLASH_PFLASHCS) || defined(TC4X_FLASH_DFLASHCS)
#define HOST_CMD_ADDR 0xF80C0000u
#endif

#if defined(TC4X_FLASH_PFLASH) || defined(TC4X_FLASH_PFLASHCS)
#define ENTER_PAGE_CMD 0x50u
#define BURST_SIZE 512u
#define PAGE_SIZE 32u
#elif defined(TC4X_FLASH_DFLASH) || defined(TC4X_FLASH_DFLASHCS)
#define ENTER_PAGE_CMD 0x5Du
#define BURST_SIZE 32u
#define PAGE_SIZE 8u
#endif

#if defined(TC4X_FLASH_PFLASH)
#define FLASH_WRITE_CMD(burst_mode) ((burst_mode) ? 0xA6u : 0xAAu)
#define FLASH_ERRSR() mmio_read_u32(DMU_HCI_ERR_ADDR)
#define FLASH_PAGE_READY()                                                     \
  (mmio_read_u32(DMU_HCI_STATUS_ADDR) & DMU_STATUS_PFPAGE)
#define FLASH_IDLE()                                                           \
  ((mmio_read_u32(DMU_HCI_STATUS_ADDR) &                                       \
    (DMU_STATUS_REQDONE | DMU_STATUS_PFBUSY)) == DMU_STATUS_REQDONE)
#define FLASH_BUSY()                                                           \
  (!!(mmio_read_u32(DMU_HCI_STATUS_ADDR) & DMU_STATUS_HOST_ANYBUSY))
#define FLASH_SET_PRMODE() flash_set_prmode(DMU_HCI_OCONTROL_ADDR)
#elif defined(TC4X_FLASH_DFLASH)
#define FLASH_WRITE_CMD(burst_mode) ((burst_mode) ? 0xA6u : 0xAAu)
#define FLASH_ERRSR() mmio_read_u32(DMU_HCI_ERR_ADDR)
#define FLASH_PAGE_READY()                                                     \
  (mmio_read_u32(DMU_HCI_STATUS_ADDR) & DMU_STATUS_DFPAGE)
#define FLASH_IDLE()                                                           \
  ((mmio_read_u32(DMU_HCI_STATUS_ADDR) &                                       \
    (DMU_STATUS_REQDONE | DMU_STATUS_BUSYHOSTDF)) == DMU_STATUS_REQDONE)
#define FLASH_BUSY()                                                           \
  (!!(mmio_read_u32(DMU_HCI_STATUS_ADDR) & DMU_STATUS_HOST_ANYBUSY))
#define FLASH_SET_PRMODE() flash_set_prmode(DMU_HCI_OCONTROL_ADDR)
#elif defined(TC4X_FLASH_PFLASHCS)
#define FLASH_WRITE_CMD(burst_mode) ((burst_mode) ? 0xA6u : 0xAAu)
#define FLASH_ERRSR() mmio_read_u32(DMU_CSCI_ERR_ADDR)
#define FLASH_PAGE_READY()                                                     \
  (mmio_read_u32(DMU_CSCI_STATUS_ADDR) & DMU_STATUS_PFPAGE)
#define FLASH_IDLE()                                                           \
  ((mmio_read_u32(DMU_CSCI_STATUS_ADDR) &                                      \
    (DMU_STATUS_REQDONE | DMU_STATUS_BUSYCSRMPF)) == DMU_STATUS_REQDONE)
#define FLASH_BUSY()                                                           \
  (!!(mmio_read_u32(DMU_CSCI_STATUS_ADDR) & DMU_STATUS_CS_ANYBUSY))
#define FLASH_SET_PRMODE() flash_set_prmode(DMU_CSCI_OCONTROL_ADDR)
#elif defined(TC4X_FLASH_DFLASHCS)
#define FLASH_WRITE_CMD(burst_mode) ((burst_mode) ? 0xA6u : 0xAAu)
#define FLASH_ERRSR() mmio_read_u32(DMU_CSCI_ERR_ADDR)
#define FLASH_PAGE_READY()                                                     \
  (mmio_read_u32(DMU_CSCI_STATUS_ADDR) & DMU_STATUS_DFPAGE)
#define FLASH_IDLE()                                                           \
  ((mmio_read_u32(DMU_CSCI_STATUS_ADDR) &                                      \
    (DMU_STATUS_REQDONE | DMU_STATUS_BUSYCSRMDF)) == DMU_STATUS_REQDONE)
#define FLASH_BUSY()                                                           \
  (!!(mmio_read_u32(DMU_CSCI_STATUS_ADDR) & DMU_STATUS_CS_ANYBUSY))
#define FLASH_SET_PRMODE() flash_set_prmode(DMU_CSCI_OCONTROL_ADDR)
#endif

#define WRITE_TIMEOUT_TICKS (550ull * 500ull)

static inline uint64_t __attribute__((always_inline)) flash_read_timebase(void) {
  return mmio_read_u64(CPU0_STM_ABS_ADDR);
}

static inline void __attribute__((always_inline))
flash_post_write_delay(void) {}

#endif /* TC4X_FLASH_H */
