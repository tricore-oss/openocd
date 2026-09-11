/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2026 Infineon Technologies AG
 */
#ifndef TRICORE_MMIO_H
#define TRICORE_MMIO_H

#include <stdint.h>

static inline void __attribute__((always_inline))
mmio_write_u32(uintptr_t addr, uint32_t value) {
  *(volatile uint32_t *)addr = value;
}

static inline void __attribute__((always_inline))
mmio_write_u64(uintptr_t addr, uint64_t value) {
  //__asm__ volatile("st.d [%0], %1" ::"a"(addr), "d"(value) : "memory");
  *(volatile uint64_t *)addr = value;
}

static inline void __attribute__((always_inline)) mmio_barrier(void) {
  __asm__ volatile("dsync" ::: "memory");
}

static inline uint32_t __attribute__((always_inline))
mmio_read_u32(uintptr_t addr) {
  return *(volatile uint32_t *)addr;
}

static inline uint64_t __attribute__((always_inline))
mmio_read_u64(uintptr_t addr) {
  return *(volatile uint64_t *)addr;
}

#endif /* TRICORE_MMIO_H */
