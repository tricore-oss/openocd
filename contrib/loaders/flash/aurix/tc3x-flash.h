/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2026 Infineon Technologies AG
 */
#ifndef TC3X_FLASH_H
#define TC3X_FLASH_H

#include <stdint.h>

#include "tricore-mmio.h"

#if (defined(TC3X_FLASH_PFLASH) + defined(TC3X_FLASH_DFLASH)) != 1
#error "define exactly one of TC3X_FLASH_PFLASH/TC3X_FLASH_DFLASH"
#endif

#define HOST_CMD_ADDR 0xAF000000u
#define DMU_HF_STATUS_ADDR 0xF8040010u
#define DMU_HF_ERRSR_ADDR 0xF8040034u
#define STM0_TIM0SV_ADDR 0xF0001050u
#define STM0_CAPSV_ADDR 0xF0001054u

/* DMU_{HF,SF}_STATUS bits */
#define DMU_STATUS_D0BUSY (1u << 0)
#define DMU_STATUS_D1BUSY (1u << 1)
#define DMU_STATUS_DFPAGE (1u << 20)
#define DMU_STATUS_PFPAGE (1u << 21)
/* PFLASH bank busy bits [7:2], masked together with reserved [11:8] */
#define DMU_STATUS_PFBUSY 0xFFCu
/* Any DFLASH or PFLASH bank busy, bits [11:0] */
#define DMU_STATUS_ANYBUSY 0xFFFu

#if defined(TC3X_FLASH_PFLASH)
#define ENTER_PAGE_CMD 0x50u
#define BURST_SIZE 256u
#define PAGE_SIZE 32u
#elif defined(TC3X_FLASH_DFLASH)
#define ENTER_PAGE_CMD 0x5Du
#define BURST_SIZE 32u
#define PAGE_SIZE 8u
#endif


#if defined(TC3X_FLASH_PFLASH)
#define FLASH_WRITE_CMD(burst_mode) ((burst_mode) ? 0xA6u : 0xAAu)
#define FLASH_ERRSR() mmio_read_u32(DMU_HF_ERRSR_ADDR)
#define FLASH_PAGE_READY()                                                     \
(!!(mmio_read_u32(DMU_HF_STATUS_ADDR) & DMU_STATUS_PFPAGE))
#define FLASH_IDLE()                                                           \
((mmio_read_u32(DMU_HF_STATUS_ADDR) & DMU_STATUS_PFBUSY) == 0u)
#define FLASH_BUSY()                                                           \
(!!(mmio_read_u32(DMU_HF_STATUS_ADDR) & DMU_STATUS_ANYBUSY))
#elif defined(TC3X_FLASH_DFLASH)
#define FLASH_WRITE_CMD(burst_mode) ((burst_mode) ? 0xA6u : 0xAAu)
#define FLASH_ERRSR() mmio_read_u32(DMU_HF_ERRSR_ADDR)
#define FLASH_PAGE_READY()                                                     \
  (!!(mmio_read_u32(DMU_HF_STATUS_ADDR) & DMU_STATUS_DFPAGE))
#define FLASH_IDLE()                                                           \
  ((mmio_read_u32(DMU_HF_STATUS_ADDR) & DMU_STATUS_D0BUSY) == 0u)
#define FLASH_BUSY()                                                           \
  (!!(mmio_read_u32(DMU_HF_STATUS_ADDR) & DMU_STATUS_ANYBUSY))
#endif

/* TC3x has no host-interface programming mode bit. */
#define FLASH_SET_PRMODE()                                                     \
  do {                                                                         \
  } while (0)

#define WRITE_TIMEOUT_TICKS (530ull * 300ull)

static inline uint64_t __attribute__((always_inline)) flash_read_timebase(void) {
  uint32_t low = mmio_read_u32(STM0_TIM0SV_ADDR);
  uint32_t high = mmio_read_u32(STM0_CAPSV_ADDR);

  return (uint64_t)low | ((uint64_t)high << 32);
}

/* Wait for 2*1/fFSI ns (DFlash) or 3*1/fFSI + 8*1/fSRI ns (PFlash). */
static inline void __attribute__((always_inline))
flash_post_write_delay(void) {
  for (uint32_t i = 0; i < 11u; i++)
    __asm__ volatile("nop");
}

#endif /* TC3X_FLASH_H */
