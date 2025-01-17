#ifndef OPENOCD_TARGET_AURIX_TRICORE_H
#define OPENOCD_TARGET_AURIX_TRICORE_H

#include "helper/types.h"
#include "target/breakpoints.h"
#include <target/register.h>
#include <target/target.h>

enum tricore_version {
  TRICORE_1_6,
  TRICORE_1_6_2,
  TRICORE_1_8,
};

enum tricore_fpu { TRICORE_FPU_NONE, TRICORE_FPU_SINGLE, TRICORE_FPU_DOUBLE };

enum tricore_event_type { TRICORE_EVENT_ADDRESS, TRICORE_EVENT_PC };
enum tricore_event_comapre { TRICORE_EVENT_EQUALITY, TRICORE_EVENT_RANGE };
enum tricore_event_access {
  TRICORE_EVENT_STORE = 0x1,
  TRICORE_EVENT_LOAD = 0x2
};

struct tricore_event {
  bool enable;
  bool bbm;
  enum tricore_event_type type;
  enum tricore_event_comapre compare;
  enum tricore_event_access access;

  union {
    struct breakpoint *bp;
    struct watchpoint *wp;
  };
};

struct tricore {
  enum tricore_version version;
  enum tricore_fpu fpu;
  struct tricore_event events[8];

  struct reg *sp;
  struct reg *pcx;
  struct reg *psw;
  struct reg *pc;
  struct reg *dbgsr;
  struct reg *bootcon;
  struct reg *core_id;

  int (*read_reg_u32)(struct target *target, uint16_t addr, uint32_t *value);
  int (*write_reg_u32)(struct target *target, uint16_t addr, uint32_t value);

  uint8_t active_vm;

  bool has_virt;
  bool has_dcache;
  bool has_icache;

  bool in_single_step;
};

#define LSB_GET(mask) ((mask) & -(mask))
#define FIELD_PREP(mask, value) (((value) * LSB_GET(mask)) & (mask))
#define FIELD_GET(mask, value) (((value) & (mask)) / LSB_GET(mask))

#define TRICORE_TRxEVT(x) (0xF000 + x * 8)
#define TRICORE_TRxEVT_EN 0x1
#define TRICORE_TRxEVT_BBM 0x8
#define TRICORE_TRxEVT_CNT 0xC0
#define TRICORE_TRxEVT_TYP 0x1000
#define TRICORE_TRxEVT_RNG 0x2000
#define TRICORE_TRxEVT_AST 0x08000000
#define TRICORE_TRxEVT_ALD 0x10000000
#define TRICORE_TRxADR(x) (0xF004 + x * 8)
#define TRICORE_DBGSR 0xFD00
#define TRICORE_DBGSR_HALT 0x6
#define TRICORE_DBGSR_SUSIN 0x8
#define TRICORE_DBGSR_SUSOUT 0x10
#define TRICORE_DBGSR_PEVT 0x80
#define TRICORE_DBGSR_EVTSRC 0x1F00
#define TRICORE_DBGSR_EVTMN 0x70000
#define TRICORE_CORE_ID 0xFE1C
#define TRICORE_CORE_ID_CORE_ID 0x3
#define TRICORE_CORE_ID_VMN 0x300
#define TRICORE_BOOTCON 0xFE60
#define TRICORE_BOOTCON_BHALT 0x1
#define TRICORE_TCCON 0xFE6C
#define TRICORE_TCCON_SP_FPU 0x1
#define TRICORE_TCCON_DP_FPU 0x2
#define TRICORE_TCCON_OVERLAY 0x4
#define TRICORE_TCCON_VIRT 0x8

static inline struct tricore *target_to_tricore(struct target *target) {
  return (struct tricore *)target->arch_info;
}

int tricore_arch_info_init(struct target *target, struct tricore *tricore);
int tricore_init(struct target *target);
int tricore_examine(struct target *target);
int tricore_poll(struct target *target);
int tricore_halt(struct target *target);
int tricore_continue(struct target *target);

struct tricore_reg {
  struct target *target;
  uint16_t addr;
  uint8_t value[4];
  bool per_hr;
  bool per_vm;
};
int tricore_build_reg_cache(struct target *target,
                            const struct reg_arch_type *type);
void tricore_free_reg_cache(struct target *target);

int tricore_event_set_step(struct target *target, target_addr_t address);
int tricore_event_set_single_step(struct target *target);
int tricore_event_clear_all(struct target *target);
int tricore_event_restore_all(struct target *target);

#endif