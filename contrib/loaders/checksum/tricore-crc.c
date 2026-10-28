#include <stdint.h>

extern void __tricore_crc(void);
extern void __tricore_exit(void) __attribute__((noreturn));

int __attribute__((noreturn)) main(uint8_t *data, uint32_t length) {
  __asm__ volatile("jl __tricore_crc\n\t");
  __asm__ volatile("j __tricore_exit");
  __builtin_unreachable();
}