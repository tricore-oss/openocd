#include "jtag/jtag.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <helper/command.h>
#include <helper/log.h>
#include <helper/types.h>
#include <jtag/interface.h>
#include <target/register.h>
#include <target/target.h>
#include <target/target_type.h>

#include "aurix_device_family.h"
#include "ocmts.h"
#include "tricore.h"
#include "tricore_register.h"

struct tricore_private_config {
  struct ocmts *ocmts;
};

int tricore_reg_get(struct reg *reg) {
  struct tricore_reg *arch_reg = (struct tricore_reg *)reg->arch_info;
  struct target *target = arch_reg->target;
  struct tricore_info *tricore = target_to_tricore(target);
  int ret = 0;

  if (reg->valid) {
    return ERROR_OK;
  }

  ret = ocmts_io_read_u32(tricore->ocmts,
                          tricore->get_reg_addr(target, arch_reg->addr),
                          &arch_reg->value);
  if (ret) {
    LOG_TARGET_ERROR(target, "Failed to read core reg 0x%04x", arch_reg->addr);
    return ret;
  }

  /* If target is unhalted all register reads should be uncached. */
  if (target->state == TARGET_HALTED)
    reg->valid = true;
  else
    reg->valid = false;

  reg->dirty = false;

  return ERROR_OK;
}

int tricore_reg_set(struct reg *reg, uint8_t *buf) {
  struct tricore_reg *arch_reg = (struct tricore_reg *)reg->arch_info;
  struct target *target = arch_reg->target;
  uint32_t value = target_buffer_get_u32(target, buf);

  target_buffer_set_u32(target, reg->value, value);

  reg->valid = true;
  reg->dirty = true;

  return 0;
}

static const struct reg_arch_type __attribute__((used)) tricore_reg_type = {
    .get = tricore_reg_get, .set = tricore_reg_set};

enum tricore_debug_reason {
  TRICORE_ERROR = -1,
  TRICORE_NOT_HALTED = 0,
  TRICORE_HALTED,
  TRICORE_SUSPENDED,
  TRICORE_EXTERNAL_EVENT,
  TRICORE_COREREG_EVENT,
  TRICORE_SOFTWARE_EVENT,
  TRICORE_TRIGGER0_EVENT,
  TRICORE_TRIGGER1_EVENT,
  TRICORE_TRIGGER2_EVENT,
  TRICORE_TRIGGER3_EVENT,
  TRICORE_TRIGGER4_EVENT,
  TRICORE_TRIGGER5_EVENT,
};

static inline enum target_debug_reason
tricore_get_debug_reason(struct tricore_info *tricore,
                         enum tricore_debug_reason debug_reason) {
  if (debug_reason == TRICORE_NOT_HALTED) {
    return DBG_REASON_NOTHALTED;
  }
  if (debug_reason == TRICORE_SOFTWARE_EVENT) {
    return DBG_REASON_BREAKPOINT;
  }
  if (debug_reason >= TRICORE_TRIGGER0_EVENT) {
    return tricore->events[debug_reason - TRICORE_TRIGGER0_EVENT].type ==
                   TRICORE_EVENT_PC
               ? DBG_REASON_BREAKPOINT
               : DBG_REASON_WATCHPOINT;
  }

  return DBG_REASON_DBGRQ;
}

static int tricore_poll(struct target *target) {
  struct tricore_info *tricore = target_to_tricore(target);
  enum target_state prev_target_state = target->state;
  int ret = ERROR_OK;
  enum tricore_debug_reason debug_reason = tricore->poll(target);

  if (debug_reason == TRICORE_ERROR) {
    LOG_TARGET_ERROR(target, "Failed to get target state");
    return ret;
  }

  if (debug_reason == TRICORE_NOT_HALTED) {
    target->state = TARGET_RUNNING;
    target->debug_reason = DBG_REASON_NOTHALTED;
    return ERROR_OK;
  }

  LOG_TARGET_DEBUG(target, "Halted for debug reason %d", debug_reason);

  if (prev_target_state != TARGET_HALTED) {
    target->state = TARGET_HALTED;
    target->debug_reason = tricore_get_debug_reason(tricore, debug_reason);

    if (prev_target_state == TARGET_DEBUG_RUNNING) {
      ret = target_call_event_callbacks(target, TARGET_EVENT_DEBUG_HALTED);
    } else {
      ret = target_call_event_callbacks(target, TARGET_EVENT_HALTED);
    }
    if (ret) {
      return ret;
    }
  }

  return ret;
}

/* Invoked only from target_arch_state().
 * Issue USER() w/architecture specific status.  */
int tricore_arch_state(struct target *target) { return ERROR_FAIL; }

/* target request support */
int tricore_target_request_data(struct target *target, uint32_t size,
                                uint8_t *buffer) {
  return ERROR_FAIL;
}

int tricore_halt(struct target *target) {
  struct tricore_info *tricore = target_to_tricore(target);
  int ret = 0;

  if (target->state == TARGET_HALTED) {
    LOG_TARGET_DEBUG(target, "target already halted");
    return ERROR_OK;
  }

  ret = ocmts_io_write_u32(tricore->ocmts,
                           tricore->get_reg_addr(target, TRICORE_DBGSR), 0x6);
  if (ret) {
    LOG_TARGET_ERROR(target, "Failed to halt target");
    return ret;
  }

  target->debug_reason = DBG_REASON_DBGRQ;

  return ret;
}

int tricore_resume(struct target *target, int current, target_addr_t address,
                   int handle_breakpoints, int debug_execution) {
  struct tricore_info *tricore = target_to_tricore(target);
  int ret = 0;

  if (target->state == TARGET_RUNNING) {
    LOG_TARGET_WARNING(target, "Target already running");
    return ERROR_OK;
  }

  if (current == 0) {
    ret = tricore_reg_set(tricore->pc, (uint8_t *)&address);
    if (ret) {
      LOG_TARGET_ERROR(target, "Failed to set PC before continue");
    }
  }

  // TODO: ret = tricore_event_restore_all(target);
  if (ret) {
    LOG_TARGET_ERROR(target, "Failed to restore events");
    return ret;
  }

  ret = target_write_u32(target, 0, 0x4);
  if (ret) {
    LOG_TARGET_ERROR(target, "Failed to continue target");
    return ret;
  }

  if (!debug_execution) {
    target->state = TARGET_RUNNING;
    ret = target_call_event_callbacks(target, TARGET_EVENT_RESUMED);
    if (ret) {
      return ret;
    }
  } else {
    target->state = TARGET_DEBUG_RUNNING;
    ret = target_call_event_callbacks(target, TARGET_EVENT_DEBUG_RESUMED);
    if (ret) {
      return ret;
    }
  }

  return ERROR_OK;
}

int tricore_step(struct target *target, int current, target_addr_t address,
                 int handle_breakpoints) {
  struct tricore_info *tricore = target_to_tricore(target);
  int ret = ERROR_OK;

  if (target->state != TARGET_HALTED) {
    LOG_TARGET_ERROR(target, "not halted");
    return ERROR_TARGET_NOT_HALTED;
  }

  if (current == 0) {
    ret = tricore_reg_set(tricore->pc, (uint8_t *)&address);
    if (ret) {
      LOG_TARGET_ERROR(target, "Failed to set PC before continue");
    }
  }

  // TODO: ret = tricore_event_set_step(target, address);
  if (ret) {
    LOG_TARGET_ERROR(target, "Failed to set step event");
    return ret;
  }

  return ret;
}

/* target reset control. assert reset can be invoked when OpenOCD and
 * the target is out of sync.
 *
 * A typical example is that the target was power cycled while OpenOCD
 * thought the target was halted or running.
 *
 * assert_reset() can therefore make no assumptions whatsoever about the
 * state of the target
 *
 * Before assert_reset() for the target is invoked, a TRST/tms and
 * chain validation is executed. TRST should not be asserted
 * during target assert unless there is no way around it due to
 * the way reset's are configured.
 *
 */
int tricore_assert_reset(struct target *target) {
  enum reset_types reset_config = jtag_get_reset_config();

  LOG_TARGET_DEBUG(target, "Reset asserted");

  /* Issue some kind of warm reset. */
  if (target_has_event_action(target, TARGET_EVENT_RESET_ASSERT))
    target_handle_event(target, TARGET_EVENT_RESET_ASSERT);
  else if (reset_config & RESET_HAS_SRST) {
    adapter_assert_reset();
    if (reset_config & RESET_SRST_PULLS_TRST) {
      LOG_ERROR("TRST with PORST will put chip in test mode");
      return ERROR_FAIL;
    }
    if (reset_config & RESET_CNCT_UNDER_SRST) {
      LOG_ERROR("Connect under SRST not possible");
      return ERROR_FAIL;
    }
  } else {
    LOG_ERROR("%s: how to reset?", target_name(target));
    return ERROR_FAIL;
  }

  /* registers are now invalid */
  register_cache_invalidate(target->reg_cache);

  target->state = TARGET_RESET;

  return ERROR_OK;
}
/**
 * The implementation is responsible for polling the
 * target such that target->state reflects the
 * state correctly.
 *
 * Otherwise the following would fail, as there will not
 * be any "poll" invoked between the "reset run" and
 * "halt".
 *
 * reset run; halt
 */
int tricore_deassert_reset(struct target *target) {
  int ret;

  /* be certain SRST is off */
  adapter_deassert_reset();

  if (!target_was_examined(target))
    return ERROR_OK;

  ret = tricore_poll(target);
  if (ret != ERROR_OK)
    return ret;

  if (target->reset_halt) {
    /* Breakpoints clear */
    // TODO: tricore_event_clear_all(target);

    if (target->state != TARGET_HALTED) {
      LOG_TARGET_WARNING(target, "ran after reset and before halt ...");
      if (target_was_examined(target)) {
        ret = tricore_halt(target);
        if (ret != ERROR_OK)
          return ret;
      } else {
        target->state = TARGET_UNKNOWN;
      }
    }
  }

  return ERROR_OK;
}
int tricore_soft_reset_halt(struct target *target) { return ERROR_FAIL; }

/**
 * Target architecture for GDB.
 *
 * The string returned by this function will not be automatically freed;
 * if dynamic allocation is used for this value, it must be managed by
 * the target, ideally by caching the result for subsequent calls.
 */
const char *tricore_get_gdb_arch(const struct target *target) {
  return "tricore";
}

/**
 * Target register access for GDB.  Do @b not call this function
 * directly, use target_get_gdb_reg_list() instead.
 *
 * Danger! this function will succeed even if the target is running
 * and return a register list with dummy values.
 *
 * The reason is that GDB connection will fail without a valid register
 * list, however it is after GDB is connected that monitor commands can
 * be run to properly initialize the target
 */
int tricore_get_gdb_reg_list(struct target *target, struct reg **reg_list[],
                             int *reg_list_size,
                             enum target_register_class reg_class) {

  switch (reg_class) {
  case REG_CLASS_ALL:
  case REG_CLASS_GENERAL:
    *reg_list_size = target->reg_cache->num_regs;
    *reg_list = malloc(sizeof(struct reg *) * (*reg_list_size));

    int i;
    for (i = 0; i < *reg_list_size; i++) {
      (*reg_list)[i] = &target->reg_cache->reg_list[i];
    }
    return ERROR_OK;
    break;
  default:
    LOG_ERROR("not a valid register class type in query.");
    return ERROR_FAIL;
  }
}

/**
 * Same as get_gdb_reg_list, but doesn't read the register values.
 * */
int tricore_get_gdb_reg_list_noread(struct target *target,
                                    struct reg **reg_list[], int *reg_list_size,
                                    enum target_register_class reg_class) {
  return ERROR_FAIL;
}

int tricore_read_memory(struct target *target, target_addr_t address,
                        uint32_t size, uint32_t count, uint8_t *buffer) {
  struct ocmts *ocmts = target->tap->priv;
  int ret;

  switch (size) {
  case 1:
  case 2:
    while (count > 0) {
      ret = ocmts_queue_io_set_address(ocmts, address);
      if (ret) {
        return ret;
      }
      if (size == 1) {
        ret = ocmts_queue_io_read_byte(ocmts, buffer);
      } else {
        ret = ocmts_queue_io_read_hword(ocmts, (void *)buffer);
      }
      if (ret) {
        return ret;
      }
      count--;
      address += size;
      buffer += size;
    }
    return ocmts_run(ocmts);
  case 4:
    while (count > 0) {
      uint32_t chunk_size = MIN(count, 256) * size;
      ret = ocmts_io_read_block(ocmts, address, buffer, chunk_size);
      if (ret) {
        return ret;
      }
      count -= chunk_size / size;
      address += chunk_size;
      buffer += chunk_size;
    }
    return ERROR_OK;
  default:
    LOG_ERROR("Unsupported write size %u", size);
    return ERROR_FAIL;
  }
}

int tricore_write_memory(struct target *target, target_addr_t address,
                         uint32_t size, uint32_t count, const uint8_t *buffer) {
  struct ocmts *ocmts = target->tap->priv;
  int ret;

  switch (size) {
  case 1:
  case 2:
    while (count) {
      ret = ocmts_queue_io_set_address(ocmts, address);
      if (ret) {
        return ret;
      }
      if (size == 1) {
        ret = ocmts_queue_io_write_byte(ocmts, *buffer);
      } else {
        uint16_t data;
        memcpy(&data, buffer, 2);
        ret = ocmts_queue_io_write_hword(ocmts, data);
      }
      if (ret) {
        return ret;
      }
      count--;
      address += size;
      buffer += size;
    }
    break;
  case 4:
    while (count > 256) {
      uint32_t chunk_size = MIN(count, 256) * size;
      ret = ocmts_io_write_block(ocmts, address, buffer, chunk_size);
      if (ret) {
        return ret;
      }
      count -= chunk_size / size;
      address += chunk_size;
      buffer += chunk_size;
    }
    break;
  default:
    LOG_ERROR("Unsupported write size %u", size);
    return ERROR_FAIL;
  }

  return ocmts_run(ocmts);
}

int tricore_checksum_memory(struct target *target, target_addr_t address,
                            uint32_t count, uint32_t *checksum) {
  return ERROR_FAIL;
}
int tricore_blank_check_memory(struct target *target,
                               struct target_memory_check_block *blocks,
                               int num_blocks, uint8_t erased_value) {
  return ERROR_FAIL;
}

/*
 * target break-/watchpoint control
 * rw: 0 = write, 1 = read, 2 = access
 *
 * Target must be halted while this is invoked as this
 * will actually set up breakpoints on target.
 *
 * The breakpoint hardware will be set up upon adding the
 * first breakpoint.
 *
 * Upon GDB connection all breakpoints/watchpoints are cleared.
 */
int tricore_add_breakpoint(struct target *target,
                           struct breakpoint *breakpoint) {
#if 0
                             struct tricore_info *tricore = target_to_tricore(target);
  if ((breakpoint->type == BKPT_HARD) && (tricore->events_available < 1)) {
    LOG_INFO("no hardware event available");
    return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
  }
  if ((breakpoint->type == BKPT_HARD) &&
      (breakpoint->length > 4)(tricore->events_available < 2)) {
    LOG_INFO("only one hardware event for range available");
    return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
  }

  return tricore_set_breakpoint(target, breakpoint);
#endif
  return ERROR_FAIL;
}

/* remove breakpoint. hw will only be updated if the target
 * is currently halted.
 * However, this method can be invoked on unresponsive targets.
 */
int tricore_remove_breakpoint(struct target *target,
                              struct breakpoint *breakpoint) {
  return ERROR_FAIL;
}

/* add watchpoint ... see add_breakpoint() comment above. */
int tricore_add_watchpoint(struct target *target,
                           struct watchpoint *watchpoint) {
  return ERROR_FAIL;
}

/* remove watchpoint. hw will only be updated if the target
 * is currently halted.
 * However, this method can be invoked on unresponsive targets.
 */
int tricore_remove_watchpoint(struct target *target,
                              struct watchpoint *watchpoint) {
  return ERROR_FAIL;
}

/* Find out just hit watchpoint. After the target hits a watchpoint, the
 * information could assist gdb to locate where the modified/accessed memory is.
 */
int tricore_hit_watchpoint(struct target *target,
                           struct watchpoint **hit_watchpoint) {
  return ERROR_FAIL;
}

/**
 * Target algorithm support.  Do @b not call this method directly,
 * use target_run_algorithm() instead.
 */
int tricore_run_algorithm(struct target *target, int num_mem_params,
                          struct mem_param *mem_params, int num_reg_params,
                          struct reg_param *reg_param,
                          target_addr_t entry_point, target_addr_t exit_point,
                          unsigned int timeout_ms, void *arch_info) {
  return ERROR_FAIL;
}
int tricore_start_algorithm(struct target *target, int num_mem_params,
                            struct mem_param *mem_params, int num_reg_params,
                            struct reg_param *reg_param,
                            target_addr_t entry_point, target_addr_t exit_point,
                            void *arch_info) {
  return ERROR_FAIL;
}
int tricore_wait_algorithm(struct target *target, int num_mem_params,
                           struct mem_param *mem_params, int num_reg_params,
                           struct reg_param *reg_param,
                           target_addr_t exit_point, unsigned int timeout_ms,
                           void *arch_info) {
  return ERROR_FAIL;
}

/* called when target is created */
int tricore_target_create(struct target *target, Jim_Interp *interp) {
  struct tricore_info *tricore = calloc(1, sizeof(struct tricore_info));
  if (!tricore) {
    LOG_TARGET_ERROR(target, "Failed to allocate target memory");
    return ERROR_FAIL;
  }
  target->arch_info = tricore;

  return ERROR_OK;
}

const struct command_registration *commands;
static const struct jim_nvp nvp_config_opts[] = {{.name = "-ocmts", .value = 0},
                                                 {.name = NULL, .value = -1}};
/* called for various config parameters */
/* returns JIM_CONTINUE - if option not understood */
/* otherwise: JIM_OK, or JIM_ERR, */
int tricore_target_jim_configure(struct target *target,
                                 struct jim_getopt_info *goi) {

  int e;
  struct jim_nvp *n;
  struct tricore_private_config *pc = target->private_config;

  if (!pc) {
    pc = calloc(1, sizeof(struct tricore_private_config));
    if (!pc) {
      LOG_ERROR("Out of memory");
      return JIM_ERR;
    }
    target->private_config = pc;
  }

  if (!goi->argc)
    return JIM_OK;

  Jim_SetEmptyResult(goi->interp);

  e = jim_nvp_name2value_obj(goi->interp, nvp_config_opts, goi->argv[0], &n);
  if (e != JIM_OK)
    return JIM_CONTINUE;

  e = jim_getopt_obj(goi, NULL);
  if (e != JIM_OK)
    return e;

  switch (n->value) {
  case 0:
    if (goi->isconfigure) {
      Jim_Obj *o_t;
      struct ocmts *ocmts;
      e = jim_getopt_obj(goi, &o_t);
      if (e != JIM_OK)
        return e;
      ocmts = ocmts_by_jim_obj(goi->interp, o_t);
      if (!ocmts) {
        Jim_SetResultString(goi->interp, "OCMTS name invalid!", -1);
        return JIM_ERR;
      }
      if (pc->ocmts && pc->ocmts != ocmts) {
        Jim_SetResultString(goi->interp, "OCMTS assignment cannot be changed!",
                            -1);
        return JIM_ERR;
      }
      pc->ocmts = ocmts;
    } else {
      if (goi->argc)
        goto err_no_param;
      if (!pc->ocmts) {
        Jim_SetResultString(goi->interp, "OCMTS not configured", -1);
        return JIM_ERR;
      }
      Jim_SetResultString(goi->interp, pc->ocmts->name, -1);
    }
    break;
  }

  if (pc->ocmts) {
    target->tap = pc->ocmts->tap;
    target->ocmts_configured = true;
    target->has_ocmts = true;
  }

  return JIM_OK;

err_no_param:
  Jim_WrongNumArgs(goi->interp, goi->argc, goi->argv, "No parameters");
  return JIM_ERR;
}

/* target commands specifically handled by the target */
/* returns JIM_OK, or JIM_ERR, or JIM_CONTINUE - if option not understood */
int tricore_target_jim_commands(struct target *target,
                                struct jim_getopt_info *goi) {
  return JIM_OK;
}

int tricore_examine(struct target *target) {
  int ret = ERROR_OK;
  struct ocmts *ocmts =
      ((struct tricore_private_config *)target->private_config)->ocmts;
  if (!target_was_examined(target)) {
    uint32_t CPU_ID;
    ret = ocmts_io_read_u32(ocmts, 0xF8800000 + 0x1FE18, &CPU_ID);
    if (ret) {
      return ret;
    }
    if ((CPU_ID & 0xFFFF00) != 0xC0C000) {
      LOG_TARGET_ERROR(target, "Invalid CPU_ID value: 0x%08x", CPU_ID);
      return ERROR_TARGET_INVALID;
    }
    if ((CPU_ID & 0xFF) == 0x31) {
      uint32_t ttcon;
      LOG_TARGET_INFO(target, "Tricore version 1.8 found");
      ret = ocmts_io_read_u32(ocmts, 0xF8800000 + 0x1FE6C, &ttcon);
      if (ret) {
        return ret;
      }
    } else if ((CPU_ID & 0xFF) == 0x21) {
      LOG_TARGET_INFO(target, "Tricore version 1.6.2P found");
    }

    // TODO: Maybe check jtag ID and core version
    target_set_examined(target);
  }

  // ret = tricore_event_clear_all(target);
  if (ret)
    return ret;

  ret = tricore_poll(target);
  if (ret)
    return ret;

  return ERROR_OK;
}

/* Set up structures for target.
 *
 * It is illegal to talk to the target at this stage as this fn is invoked
 * before the JTAG chain has been examined/verified
 * */
int tricore_init_target(struct command_context *cmd_ctx,
                        struct target *target) {

  return ERROR_OK; // tricore_build_reg_cache(target, &tricore_reg_type);
}

/**
 * Free all the resources allocated by the target.
 * @param target The target to deinit
 */
void tricore_deinit_target(struct target *target) {
  // tricore_free_reg_cache(target);
  free(target_to_tricore(target));
}

/* after reset is complete, the target can check if things are properly set up.
 *
 * This can be used to check if e.g. DCC memory writes have been enabled for
 * arm7/9 targets, which they really should except in the most contrived
 * circumstances.
 */
int tricore_check_reset(struct target *target) { return ERROR_OK; }

/* Parse target-specific GDB query commands.
 * The string pointer "response_p" is always assigned by the called function
 * to a pointer to a NULL-terminated string, even when the function returns
 * an error. The string memory is not freed by the caller, so this function
 * must pay attention for possible memory leaks if the string memory is
 * dynamically allocated.
 */
int tricore_gdb_query_custom(struct target *target, const char *packet,
                             char **response_p) {
  return ERROR_FAIL;
}

unsigned int tricore_address_bits(struct target *target) { return 32; }

unsigned int tricore_data_bits(struct target *target) { return 32; }

static const struct command_registration tricore_commands[] = {
    COMMAND_REGISTRATION_DONE};

struct target_type tricore_target = {
    .name = "aurix",

    .poll = tricore_poll,
    .arch_state = tricore_arch_state,

    .halt = tricore_halt,
    .resume = tricore_resume,
    .step = tricore_step,

    .assert_reset = tricore_assert_reset,
    .deassert_reset = tricore_deassert_reset,
    .soft_reset_halt = tricore_soft_reset_halt,

    .read_memory = tricore_read_memory,
    .write_memory = tricore_write_memory,

    .checksum_memory = tricore_checksum_memory,

    .get_gdb_arch = tricore_get_gdb_arch,
    .get_gdb_reg_list = tricore_get_gdb_reg_list,

    .run_algorithm = tricore_run_algorithm,
    .start_algorithm = tricore_start_algorithm,
    .wait_algorithm = tricore_wait_algorithm,

    .add_breakpoint = tricore_add_breakpoint,
    .remove_breakpoint = tricore_remove_breakpoint,

    .add_watchpoint = tricore_add_watchpoint,
    .remove_watchpoint = tricore_remove_watchpoint,

    .target_create = tricore_target_create,

    .target_jim_configure = tricore_target_jim_configure,
    .target_jim_commands = tricore_target_jim_commands,

    .init_target = tricore_init_target,
    .examine = tricore_examine,
    .deinit_target = tricore_deinit_target,

    .commands = tricore_commands,
};
