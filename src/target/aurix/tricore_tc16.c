#include "aurix.h"
#include "aurix_ocds.h"
#include "tricore.h"

#define TRICORE_SWEVT 0xFD10
#define TRICORE_SWEVT_EVA 0x7
#define TRICORE_SWEVT_BBM 0x8
#define TRICORE_SWEVT_BOD 0x10
#define TRICORE_SWEVT_SUSP 0x20
#define TRICORE_SWEVT_CNT 0xC0

struct tricore_tc16_regs {
  uint32_t syscon;
  uint32_t dbgsr;
};

static int tricore_tc16_poll(struct target *target) {
  struct tricore *tricore = target_to_tricore(target);
  struct tricore_tc16_regs *regs = tricore->regs;
  struct aurix_ocds *ocds = target_to_aurix(target)->ocds;
  int ret;

  ret = aurix_ocds_queue_soc_read_u32(
      ocds, tricore_get_core_reg_addr(target, TRICORE_SYSCON, false),
      &regs->syscon);
  if (ret) {
    return ret;
  }
  ret = aurix_ocds_queue_soc_read_u32(
      ocds, tricore_get_core_reg_addr(target, TRICORE_DBGSR, false),
      &regs->dbgsr);
  if (ret) {
    return ret;
  }
  ret = aurix_ocds_run(ocds);
  if (ret) {
    return ret;
  }

  /* Check for boot halt, which is set after reset */
  if (regs->syscon & TRICORE_SYSCON_BHALT) {
    target->state = TARGET_HALTED;
    target->debug_reason = DBG_REASON_UNDEFINED;
    tricore->active_vm = 1;
    return ERROR_OK;
  }

  target->state =
      (regs->dbgsr & TRICORE_DBGSR_HALT) ? TARGET_HALTED : TARGET_RUNNING;

  if (target->state == TARGET_HALTED) {
    if (tricore->in_single_step) {
      target->debug_reason = DBG_REASON_SINGLESTEP;
    } else {
      target->debug_reason = DBG_REASON_DBGRQ;
      uint32_t i;
      for (i = 0; i < 8; i++) {
        if (FIELD_GET(TRICORE_DBGSR_EVTSRC, regs->dbgsr) == (16 + i) &&
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

static uint32_t tricore_tc16_get_sw_event(struct target *target, bool enable, bool bbm,
                                  enum tricore_debug_counter cnt) {
  struct tricore *tricore = target_to_tricore(target);
  uint32_t swevt = FIELD_PREP(TRICORE_SWEVT_EVA, 0x2) |
                   FIELD_PREP(TRICORE_SWEVT_CNT, cnt) |
                   (bbm ? TRICORE_SWEVT_BBM : 0);

  return swevt;
}


int tricore_tc16_init(struct target *target) {
    struct tricore *tricore = target_to_tricore(target);

    tricore->poll = tricore_tc16_poll;

    tricore->get_event = tricore_tc16_get_swevent;
    tricore->get_sw_event = tricore_tc16_get_sw_event;
    tricore->get_single_step_event = tricore_tc16_get_single_step_event;
    tricore->get_step_event = tricore_tc16_get_swevent;
}