#include <stdlib.h>

#include <helper/log.h>
#include <target/breakpoints.h>
#include <target/register.h>
#include <target/target.h>

#include "tricore.h"

int tricore_arch_info_init(struct target *target, struct tricore *tricore) {
  target->arch_info = tricore;

  return ERROR_OK;
}

int tricore_init(struct target *target) { return 0; }

int tricore_examine(struct target *target) {
  struct tricore *tricore = target_to_tricore(target);
  int ret;

  uint32_t tccon;
  ret = tricore->read_reg_u32(target, TRICORE_TCCON, &tccon);
  if (ret) {
    return ret;
  }
  tricore->fpu = (tccon & TRICORE_TCCON_DP_FPU)   ? TRICORE_FPU_DOUBLE
                 : (tccon & TRICORE_TCCON_SP_FPU) ? TRICORE_FPU_SINGLE
                                                  : TRICORE_FPU_NONE;

  return 0;
}

int tricore_poll(struct target *target) {
  struct tricore *tricore = target_to_tricore(target);
  uint32_t dbgsr, bootcon, core_id;
  int ret;

  ret = tricore->read_reg_u32(target, TRICORE_BOOTCON, &bootcon);
  if (ret) {
    return ret;
  }

  if (bootcon & TRICORE_BOOTCON_BHALT) {
    target->state = TARGET_HALTED;
    target->debug_reason = DBG_REASON_UNDEFINED;
    tricore->active_vm = 1;
    return ERROR_OK;
  }

  ret = tricore->read_reg_u32(target, TRICORE_DBGSR, &dbgsr);
  if (ret) {
    return ret;
  }
  target->state = (dbgsr & TRICORE_DBGSR_HALT) ? TARGET_HALTED : TARGET_RUNNING;

  if (target->state == TARGET_HALTED) {
    if (tricore->in_single_step) {
      target->debug_reason = DBG_REASON_SINGLESTEP;
    } else {
      target->debug_reason = DBG_REASON_DBGRQ;
      uint32_t i;
      for (i = 0; i < 8; i++) {
        if (FIELD_GET(TRICORE_DBGSR_EVTSRC, dbgsr) == (16 + i) &&
            tricore->events[i].enable) {
          target->debug_reason = tricore->events[i].type == TRICORE_EVENT_PC
                                     ? DBG_REASON_BREAKPOINT
                                     : DBG_REASON_WATCHPOINT;
        }
        if ((i % 2) == 0 && tricore->events[i].enable &&
            tricore->events[i].compare == TRICORE_EVENT_RANGE) {
          i++;
        }
      }
    }
  } else {
    target->debug_reason = DBG_REASON_NOTHALTED;
  }

  return ERROR_OK;
}

int tricore_halt(struct target *target) {
  struct tricore *tricore = target_to_tricore(target);
  return tricore->write_reg_u32(target, TRICORE_DBGSR,
                                FIELD_PREP(TRICORE_DBGSR_HALT, 3));
}

int tricore_continue(struct target *target) {
  struct tricore *tricore = target_to_tricore(target);

  /* registers are now invalid */
  register_cache_invalidate(target->reg_cache);

  return tricore->write_reg_u32(target, TRICORE_DBGSR,
                                FIELD_PREP(TRICORE_DBGSR_HALT, 2));
}

static inline int tricore_event_set(struct target *target, uint8_t event_id) {
  struct tricore *tricore = target_to_tricore(target);
  struct tricore_event *event = &tricore->events[event_id];
  uint32_t trxevt;
  int ret;
  trxevt = FIELD_PREP(TRICORE_TRxEVT_EN, event->enable) |
           FIELD_PREP(TRICORE_TRxEVT_BBM, event->bbm) |
           FIELD_PREP(TRICORE_TRxEVT_RNG, event->compare) |
           FIELD_PREP(TRICORE_TRxEVT_TYP, event->type);

  if (event->enable) {

    if (event->type == TRICORE_EVENT_PC) {
      ret = tricore->write_reg_u32(target, TRICORE_TRxADR(event_id),
                                   event->bp->address);
      if (ret) {
        return ret;
      }
      if (event->bp->length > 4) {
        ret = tricore->write_reg_u32(target, TRICORE_TRxADR(event_id + 1),
                                     event->bp->address + event->bp->length);
        if (ret) {
          return ret;
        }
      }
    } else {
      ret = tricore->write_reg_u32(target, TRICORE_TRxADR(event_id),
                                   event->wp->address);
      if (ret) {
        return ret;
      }
      if (event->bp->length > 4) {
        ret = tricore->write_reg_u32(target, TRICORE_TRxADR(event_id + 1),
                                     event->wp->address + event->wp->length);
        if (ret) {
          return ret;
        }
      }
    }
  }

  return tricore->write_reg_u32(target, TRICORE_TRxEVT(event_id), trxevt);
}

static inline int tricore_event_set_breakpoint(struct target *target,
                                               uint8_t event_id,
                                               struct breakpoint *bp) {
  struct tricore *tricore = target_to_tricore(target);
  struct tricore_event *event;
  if (event_id >= 8) {
    return ERROR_BREAKPOINT_NOT_FOUND;
  }
  if (bp->length > 4 && event_id >= 7) {
    return ERROR_BREAKPOINT_NOT_FOUND;
  }

  event = &tricore->events[event_id];
  event->access = 0;
  event->type = TRICORE_EVENT_PC;
  event->bbm = true;
  event->compare =
      bp->length > 4 ? TRICORE_EVENT_RANGE : TRICORE_EVENT_EQUALITY;
  event->enable = true;

  return tricore_event_set(target, event_id);
}

int tricore_event_clear_all(struct target *target) {
  struct tricore *tricore = target_to_tricore(target);
  uint32_t event_id;
  int ret;

  for (event_id = 0; event_id < 8; event_id++) {
    memset(&tricore->events[event_id], 0x0, sizeof(struct tricore_event));
    ret = tricore_event_set(target, event_id);
    if (ret) {
      return ret;
    }
  }

  return 0;
}

int tricore_event_restore_all(struct target *target) {
  struct tricore *tricore = target_to_tricore(target);
  uint8_t event_id = 0;
  int ret;

  tricore->in_single_step = false;

  for (event_id = 0; event_id < 8; event_id++) {
    ret = tricore_event_set(target, event_id);
    if (ret) {
      return ret;
    }

    if (tricore->events[event_id].compare == TRICORE_EVENT_RANGE) {
      event_id++;
    }
  }

  return ERROR_OK;
}

int tricore_event_set_step(struct target *target, target_addr_t address) {
  struct tricore *tricore = target_to_tricore(target);
  int ret;
  uint32_t trxevt = FIELD_PREP(TRICORE_TRxEVT_EN, 1) |
                    FIELD_PREP(TRICORE_TRxEVT_BBM, 1) |
                    FIELD_PREP(TRICORE_TRxEVT_RNG, TRICORE_EVENT_EQUALITY) |
                    FIELD_PREP(TRICORE_TRxEVT_TYP, TRICORE_EVENT_PC);

  tricore->in_single_step = true;

  ret = tricore->write_reg_u32(target, TRICORE_TRxADR(0), address);
  if (ret) {
    return ret;
  }

  return tricore->write_reg_u32(target, TRICORE_TRxEVT(0), trxevt);
}

int tricore_event_set_single_step(struct target *target) {
  struct tricore *tricore = target_to_tricore(target);
  int ret;
  uint32_t trxevt = FIELD_PREP(TRICORE_TRxEVT_EN, 1) |
                    FIELD_PREP(TRICORE_TRxEVT_BBM, 0) |
                    FIELD_PREP(TRICORE_TRxEVT_RNG, TRICORE_EVENT_RANGE) |
                    FIELD_PREP(TRICORE_TRxEVT_TYP, TRICORE_EVENT_PC);

  tricore->in_single_step = true;
  ret = tricore->write_reg_u32(target, TRICORE_TRxADR(0), 0);
  if (ret) {
    return ret;
  }
  ret = tricore->write_reg_u32(target, TRICORE_TRxADR(1), 0xFFFFFFFF);
  if (ret) {
    return ret;
  }

  return tricore->write_reg_u32(target, TRICORE_TRxEVT(0), trxevt);
}

static const struct {
  const char *const name;
  uint16_t addr;
  bool caller_saved;
  bool per_hr;
} tricore_core_regs[] = {
    {.name = "d0", .addr = 0xFF00, .caller_saved = true},
    {.name = "d1", .addr = 0xFF04, .caller_saved = true},
    {.name = "d2", .addr = 0xFF08, .caller_saved = true},
    {.name = "d3", .addr = 0xFF0C, .caller_saved = true},
    {.name = "d4", .addr = 0xFF10, .caller_saved = true},
    {.name = "d5", .addr = 0xFF14, .caller_saved = true},
    {.name = "d6", .addr = 0xFF18, .caller_saved = true},
    {.name = "d7", .addr = 0xFF1C, .caller_saved = true},
    {.name = "d8", .addr = 0xFF20},
    {.name = "d9", .addr = 0xFF24},
    {.name = "d10", .addr = 0xFF28},
    {.name = "d11", .addr = 0xFF2C},
    {.name = "d12", .addr = 0xFF30},
    {.name = "d13", .addr = 0xFF34},
    {.name = "d14", .addr = 0xFF38},
    {.name = "d15", .addr = 0xFF3C},
    {.name = "a0", .addr = 0xFF80, .caller_saved = true},
    {.name = "a1", .addr = 0xFF84, .caller_saved = true},
    {.name = "a2", .addr = 0xFF88, .caller_saved = true},
    {.name = "a3", .addr = 0xFF8C, .caller_saved = true},
    {.name = "a4", .addr = 0xFF90, .caller_saved = true},
    {.name = "a5", .addr = 0xFF94, .caller_saved = true},
    {.name = "a6", .addr = 0xFF98, .caller_saved = true},
    {.name = "a7", .addr = 0xFF9C, .caller_saved = true},
    {.name = "a8", .addr = 0xFFA0, .caller_saved = true},
    {.name = "a9", .addr = 0xFFA4, .caller_saved = true},
    {.name = "a10", .addr = 0xFFA8},
    {.name = "a11", .addr = 0xFFAC},
    {.name = "a12", .addr = 0xFFB0},
    {.name = "a13", .addr = 0xFFB4},
    {.name = "a14", .addr = 0xFFB8},
    {.name = "a15", .addr = 0xFFBC},
    {.name = "lcx", .addr = 0xFE3C},
    {.name = "fcx", .addr = 0xFE38},
    {.name = "pcx", .addr = 0xFE00},
    {.name = "psw", .addr = 0xFE04},
    {.name = "pc", .addr = 0xFE08},
    {.name = "icr", .addr = 0xFE2C},
    {.name = "isp", .addr = 0xFE28},
    {.name = "btv", .addr = 0xFE24},
    {.name = "biv", .addr = 0xFE20},
    {.name = "syscon", .addr = 0xFE14},
    {.name = "pcon0", .addr = 0x920C},
    {.name = "dcon0", .addr = 0x9040},
};

struct reg_feature tricore_core_feature = {
    .name = "org.gnu.gdb.tricore.core",
};

int tricore_build_reg_cache(struct target *target,
                            const struct reg_arch_type *type) {
  int num_regs = ARRAY_SIZE(tricore_core_regs);

  struct reg_cache *cache = malloc(sizeof(struct reg_cache));
  struct reg *reg_list = calloc(num_regs, sizeof(struct reg));
  struct tricore_reg *reg_arch_info =
      calloc(num_regs, sizeof(struct tricore_reg));
  int i;

  if (!cache || !reg_list || !reg_arch_info) {
    free(cache);
    free(reg_list);
    free(reg_arch_info);
    target->reg_cache = NULL;
    return ERROR_FAIL;
  }
  target->reg_cache = cache;

  cache->name = "TriCore registers";
  cache->next = NULL;
  cache->reg_list = reg_list;
  cache->num_regs = 0;

  for (i = 0; i < num_regs; i++) {
    reg_arch_info[i].addr = tricore_core_regs[i].addr;
    reg_arch_info[i].target = target;
    reg_arch_info[i].per_hr = tricore_core_regs[i].per_hr;

    reg_list[i].name = tricore_core_regs[i].name;
    reg_list[i].number = i;
    reg_list[i].size = 32;
    reg_list[i].value = reg_arch_info[i].value;
    reg_list[i].type = type;
    reg_list[i].arch_info = &reg_arch_info[i];
    reg_list[i].exist = true;

    /* This really depends on the calling convention in use */
    reg_list[i].caller_save = tricore_core_regs[i].caller_saved;

    /* Registers data type, as used by GDB target description */
    reg_list[i].reg_data_type = calloc(1, sizeof(struct reg_data_type));
    if (i < 16) {
      reg_list[i].reg_data_type->type = REG_TYPE_INT;
    } else if (i < 32) {
      reg_list[i].reg_data_type->type = REG_TYPE_DATA_PTR;
    } else if (i == 34 || i == 35) {
      reg_list[i].reg_data_type->type = REG_TYPE_UINT32;
    } else if (i == 36 || i == 40 || i == 39) {
      reg_list[i].reg_data_type->type = REG_TYPE_CODE_PTR;
    } else {
      reg_list[i].reg_data_type->type = REG_TYPE_INT;
    }

    reg_list[i].feature = &tricore_core_feature;
    reg_list[i].group = "general";

    cache->num_regs++;
  }

  struct tricore *tricore = target_to_tricore(target);
  tricore->pc = reg_list + 34;

  return ERROR_OK;
}

void tricore_free_reg_cache(struct target *target) {
  free(target->reg_cache->reg_list->arch_info);
  free(target->reg_cache->reg_list);
  free(target->reg_cache);
}