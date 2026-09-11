#ifndef TC2_FLASH_H
#define TC2_FLASH_H

#include <stdint.h>

#include "tricore-mmio.h"

#if (defined(TC2X_FLASH_PFLASH) + defined(TC2X_FLASH_DFLASH)) != 1
#error "define exactly one of TC2X_FLASH_PFLASH/TC2X_FLASH_DFLASH"
#endif

#define HOST_CMD_ADDR 0xAF000000u
#define TC2X_WDTS_CON0 (*(volatile uint32_t *)0xF00360F0u)
#define STM0_TIM0SV_ADDR 0xF0000050u
#define STM0_CAPSV_ADDR 0xF0000054u
#define FSR_ADDR 0xF8002010u

#define FSR_ERRORS                                                             \
  ((1u << 11) | (1u << 12) | (1u << 13) | (1u << 25) | (1u << 26))

#define TC2X_STATUS_BUSY_DFLASH0 (1u << 1)
#define TC2X_STATUS_BUSY_DFLASH1 (1u << 2)
#define TC2X_STATUS_BUSY_PFLASH0 (1u << 3)
#define TC2X_STATUS_BUSY_PFLASH1 (1u << 4)
#define TC2X_STATUS_BUSY_PFLASH2 (1u << 5)
#define TC2X_STATUS_BUSY_PFLASH3 (1u << 6)
#define TC2X_STATUS_PROG (1u << 7)
#define TC2X_STATUS_ERASE (1u << 8)

#define FLASH_WRITE_CMD(burst_mode) ((burst_mode) ? 0x7Au : 0xAAu)

#if defined(TC2X_FLASH_PFLASH)
#define ENTER_PAGE_CMD 0x50u
#define BURST_SIZE 256u
#define PAGE_SIZE 32u
#define TC2X_PAGE_READY_BIT (1u << 9)
#define TC2X_BUSY_BITS                                                         \
  (TC2X_STATUS_BUSY_PFLASH0 | TC2X_STATUS_BUSY_PFLASH1 |                       \
   TC2X_STATUS_BUSY_PFLASH2 | TC2X_STATUS_BUSY_PFLASH3)
#elif defined(TC2X_FLASH_DFLASH)
#define ENTER_PAGE_CMD 0x5Du
#define BURST_SIZE 32u
#define PAGE_SIZE 8u
#define TC2X_PAGE_READY_BIT (1u << 10)
#define TC2X_BUSY_BITS (TC2X_STATUS_BUSY_DFLASH0 | TC2X_STATUS_BUSY_DFLASH1)
#endif

#define WRITE_TIMEOUT_TICKS 40000000ull

static inline uint32_t __attribute__((always_inline)) tc2x_flash_status(void) {
  return mmio_read_u32(FSR_ADDR);
}

#define FLASH_ERRSR() (tc2x_flash_status() & FSR_ERRORS)
#define FLASH_PAGE_READY() (tc2x_flash_status() & TC2X_PAGE_READY_BIT)
#define FLASH_BUSY()                                                           \
  (!!(tc2x_flash_status() &                                                    \
      (TC2X_STATUS_BUSY_DFLASH0 | TC2X_STATUS_BUSY_DFLASH1 |                   \
       TC2X_STATUS_BUSY_PFLASH0 | TC2X_STATUS_BUSY_PFLASH1 |                   \
       TC2X_STATUS_BUSY_PFLASH2 | TC2X_STATUS_BUSY_PFLASH3)))
#define FLASH_IDLE()                                                           \
  ((tc2x_flash_status() & (TC2X_BUSY_BITS | TC2X_STATUS_PROG)) ==              \
   TC2X_STATUS_PROG)
#define FLASH_SET_PRMODE()                                                     \
  do {                                                                         \
  } while (0)

static inline uint64_t
    __attribute__((always_inline)) flash_read_timebase(void) {
  uint32_t low = mmio_read_u32(STM0_TIM0SV_ADDR);
  uint32_t high = mmio_read_u32(STM0_CAPSV_ADDR);

  return (uint64_t)low | ((uint64_t)high << 32);
}

static inline void __attribute__((always_inline)) flash_post_write_delay(void) {
}

#endif
