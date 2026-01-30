#if 0
#include <config.h>
#include <target/target.h>

#include "tricore.h"

struct tricore_tc18_regs {
  uint32_t bootcon;
  uint32_t syscon;
  uint32_t dbgsr;
};

static int tricore_tc18_poll(struct target *target) {
  struct tricore *tricore = target_to_tricore(target);
  uint32_t dbgsr;
  uint32_t bootcon;
  int ret;
  bool bhalt = false;
  ret = tricore->read_reg_u32(target, TRICORE_BOOTCON, &bootcon);
  if (ret) {
    return ret;
  }
  bhalt = (bootcon & TRICORE_BOOTCON_BHALT) != 0;

  /* Check for boot halt, which is set after reset */
  if (bhalt) {
    target->state = TARGET_HALTED;
    target->debug_reason = DBG_REASON_UNDEFINED;
    tricore->active_vm = 1;
    return ERROR_OK;
  }

  ret = tricore->read_reg_u32(target, TRICORE_DBGSR, &dbgsr);
  if (ret) {
    return ret;
  }
  if (tricore->has_virt) {
    ret = tricore->read_reg_u32(target, TRICORE_CORE_ID, &core_id);
    if (ret) {
      return ret;
    }
    tricore->active_vm = FIELD_GET(TRICORE_CORE_ID_VMN, core_id);
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

static int tricore_tc18_examine(struct target *target) {
  struct tricore *tricore = target_to_tricore(target);
  uint32_t tccon;
  int ret;

  ret = tricore->read_reg_u32(target, TRICORE_TCCON, &tccon);
  if (ret) {
    return ret;
  }

  tricore->version = TRICORE_1_8;
  tricore->fpu = (tccon & TRICORE_TCCON_DP_FPU)   ? TRICORE_FPU_DOUBLE
                 : (tccon & TRICORE_TCCON_SP_FPU) ? TRICORE_FPU_SINGLE
                                                  : TRICORE_FPU_NONE;
  tricore->has_virt = (tccon & TRICORE_TCCON_VIRT) != 0;
  return ERROR_OK;
}

#endif