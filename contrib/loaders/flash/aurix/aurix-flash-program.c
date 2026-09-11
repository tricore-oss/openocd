/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2026 Infineon Technologies AG
 */
#include <stdbool.h>
#include <stdint.h>

#include "errors.h"

#if defined(TC2X_FLASH_PFLASH) || defined(TC2X_FLASH_DFLASH)
#include "tc2x-flash.h"
#elif defined(TC3X_FLASH_PFLASH) || defined(TC3X_FLASH_DFLASH)
#include "tc3x-flash.h"
#elif defined(TC4X_FLASH_PFLASH) || defined(TC4X_FLASH_DFLASH) ||              \
    defined(TC4X_FLASH_PFLASHCS) || defined(TC4X_FLASH_DFLASHCS)
#include "tc4x-flash.h"
#else
#error "no flash bank selected, see tc2-flash.h / tc3x-flash.h / tc4x-flash.h"
#endif

#if defined(TC2X_FLASH_PFLASH)
static inline uint32_t __attribute__((always_inline))
tc2x_safety_endinit(uint32_t endinit) {
  uint32_t i;
  const uint32_t value = TC2X_WDTS_CON0;

  TC2X_WDTS_CON0 = (value & 0xFFFFFF00u) | (~value & 0xFCu) | 0x01u;
  TC2X_WDTS_CON0 = (value & 0xFFFFFF00u) | (~value & 0xFCu) | 0x02u | endinit;
  for (i = 0; i < 100000u; i++) {
    if ((TC2X_WDTS_CON0 & 1u) == endinit)
      return 0;
  }
  return 1u << 28;
}
#endif

static inline void __attribute__((always_inline)) clear_status(void) {
  mmio_write_u32(HOST_CMD_ADDR + 0x5554u, 0xFAu);
  mmio_barrier();
}

static inline uint32_t __attribute__((always_inline)) enter_page_mode(void) {
  mmio_write_u32(HOST_CMD_ADDR + 0x5554u, ENTER_PAGE_CMD);
  mmio_barrier();

  uint32_t timeout = 0;
  do {
    if (FLASH_PAGE_READY())
      return 0;

    uint32_t ret = FLASH_ERRSR();
    if (ret)
      return ret;
  } while (++timeout < 1000u);

  return ERROR_PAGE;
}

static inline uint32_t __attribute__((always_inline))
wait_write_complete(void) {
  uint64_t start = flash_read_timebase();
  const uint64_t stop = start + WRITE_TIMEOUT_TICKS;

  while (flash_read_timebase() < stop) {
    uint32_t ret = FLASH_ERRSR();
    if (ret)
      return ret;

    if (FLASH_IDLE())
      return 0;
  }

  return ERROR_TIMEOUT;
}

int __attribute__((noreturn)) main(void *buffer_start, uint32_t buffer_size,
                                   uintptr_t addr, uint32_t size) {
  uint32_t ret = 0;
  volatile uintptr_t *wptr = (volatile uintptr_t *)buffer_start;
  volatile uintptr_t *rptr =
      (volatile uintptr_t *)((uintptr_t)buffer_start + 4u);
  const uintptr_t fifo_end = (uintptr_t)buffer_start + buffer_size;

  *rptr = (uintptr_t)buffer_start + 8u;

  /* Disable interrupts */
  __asm__ volatile("disable");
  clear_status();
  if (FLASH_BUSY()) {
    ret = ERROR_BUSY;
    goto out_noclear;
  }
  FLASH_SET_PRMODE();

  while (size) {
    uint32_t i;
    uintptr_t cur_rptr = *rptr;
    const bool burst_mode = (addr % BURST_SIZE == 0u) && (size >= BURST_SIZE);
    const uintptr_t copy_size = burst_mode ? BURST_SIZE : PAGE_SIZE;

    /* Clear status before writing data */
    clear_status();

    /* Enter page mode */
    ret = enter_page_mode();
    if (ret)
      goto out;

#if defined(TC4X_FLASH_PFLASH) || defined(TC4X_FLASH_DFLASH) ||                \
    defined(TC4X_FLASH_PFLASHCS) || defined(TC4X_FLASH_DFLASHCS)
    /* Clear done bit before writing data */
    clear_status();
#endif

    for (i = 0; i < copy_size; i += 8u) {
      /* Wait until there is space in the FIFO */
      while (*wptr == cur_rptr) {
      }

      mmio_write_u64(HOST_CMD_ADDR + 0x55F0u, *(uint64_t *)(cur_rptr));
      mmio_barrier();
      /* Check for sequence error */
      ret = FLASH_ERRSR();
      if (ret)
        goto out;

      cur_rptr += 8u;
      if (cur_rptr >= fifo_end)
        cur_rptr = (uintptr_t)buffer_start + 8u;
      *rptr = cur_rptr;
    }

#if defined(TC4X_FLASH_PFLASH) || defined(TC4X_FLASH_DFLASH) ||                \
    defined(TC4X_FLASH_PFLASHCS) || defined(TC4X_FLASH_DFLASHCS)
    /* Clear status before issuing write command */
    clear_status();
#endif

#if defined(TC2X_FLASH_PFLASH)
    ret = tc2x_safety_endinit(0u);
    if (ret)
      goto out;
#endif

    /* Issue page write command */
    mmio_write_u32(HOST_CMD_ADDR + 0xAA50u, (uint32_t)addr);
    mmio_write_u32(HOST_CMD_ADDR + 0xAA58u, 0u);
    mmio_write_u32(HOST_CMD_ADDR + 0xAAA8u, 0xA0u);
    mmio_write_u32(HOST_CMD_ADDR + 0xAAA8u, FLASH_WRITE_CMD(burst_mode));
    mmio_barrier();

#if defined(TC2X_FLASH_PFLASH)
    ret = tc2x_safety_endinit(1u);
    if (ret)
      goto out;
#endif

    size -= (uint32_t)copy_size;
    addr += copy_size;

    flash_post_write_delay();

    /* Wait for the write to complete */
    ret = wait_write_complete();
    if (ret)
      goto out;
  }

  goto out_noerr;

out:
  mmio_write_u32(HOST_CMD_ADDR + 0x5554u, 0xF0u);
  clear_status();
out_noclear:
  *rptr = 0u;
out_noerr:
  /* Report error code and notify debugger */
  {
    register uint32_t result __asm__("d2") = ret;
    while (1)
      __asm__ volatile("debug" ::"d"(result));
  }

  __builtin_unreachable();
}
