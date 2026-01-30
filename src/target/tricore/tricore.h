#ifndef OPENOCD_TARGET_TRICORE_TRICORE_H
#define OPENOCD_TARGET_TRICORE_TRICORE_H

#include <helper/types.h>
#include <target/breakpoints.h>
#include <target/register.h>
#include <target/target.h>

#include "ocmts.h"

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

  uint32_t address;
};

enum tricore_debug_counter {
  TRICORE_DEBUG_COUNTER_NO_CHANGE,
  TRICORE_DEBUG_COUNTER_START,
  TRICORE_DEBUG_COUNTER_STOP,
  TRICORE_DEBUG_COUNTER_TOGGLE,
};

#define TRICORE_NUM_EVENTS 8

struct tricore_info {
  struct ocmts *ocmts;

  enum tricore_version version;
  enum tricore_fpu fpu;
  struct tricore_event events[8];

  uint32_t base;
  void *regs;

  struct reg *sp;
  struct reg *pcx;
  struct reg *psw;
  struct reg *pc;
  struct reg *dbgsr;
  struct reg *bootcon;
  struct reg *core_id;

  uint32_t (*get_reg_addr)(struct target *target, uint16_t reg);

  int (*poll)(struct target *target);

  uint32_t (*get_single_step_event)(struct target *target);
  uint32_t (*get_step_event)(struct target *target);
  uint32_t (*get_event)(struct target *target, struct tricore_event *event);
  uint32_t (*get_sw_event)(struct target *target, bool enable, bool bbm,
                           enum tricore_debug_counter cnt);

  int (*examine)(struct target *target);

  uint8_t active_vm;
  uint8_t active_event;

  bool has_virt;
  bool has_dcache;
  bool has_icache;

  bool boot_halt;
  bool in_single_step;
  bool halted;
  int current_event_id;
};

struct tricore_reg {
  struct target *target;
  uint16_t addr;
  uint32_t value;
  bool per_hr;
  bool per_vm;
};

static inline struct tricore_info *target_to_tricore(struct target *target) {
  return (struct tricore_info *)target->arch_info;
}

#endif