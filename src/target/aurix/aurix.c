#include <assert.h>
#include <stdlib.h>
#include <time.h>

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

#include <target/aurix/aurix.h>
#include <target/aurix/aurix_device_family.h>
#include <target/aurix/aurix_ocds.h>
#include <target/aurix/tricore.h>

int aurix_reg_get(struct reg *reg) {
  struct tricore_reg *arch_reg = (struct tricore_reg *)reg->arch_info;
  struct target *target = arch_reg->target;
  struct aurix_ocds *ocds = target_to_aurix(target)->ocds;
  uint32_t value;

  int ret = aurix_ocds_atomic_read_u32(
      ocds, aurix_core_get_reg_addr(target, reg), &value);
  memcpy(arch_reg->value, &value, 4);

  if (ret == 0) {
    reg->valid = true;
    reg->dirty = false;
  }

  return ret;
}

int aurix_reg_set(struct reg *reg, uint8_t *buf) {
  struct tricore_reg *arch_reg = (struct tricore_reg *)reg->arch_info;

  memcpy(arch_reg->value, buf, 4);
  reg->valid = 1;
  reg->dirty = 1;

  return 0;
}

static const struct reg_arch_type aurix_reg_type = {.get = aurix_reg_get,
                                                    .set = aurix_reg_set};

static int aurix_poll(struct target *target) {
  enum target_state prev_target_state;
  int ret = ERROR_OK;

  prev_target_state = target->state;
  ret = tricore_poll(target);
  if (ret) {
    LOG_TARGET_ERROR(target, "Failed to get target state");
    return ret;
  }

  if (target->state == TARGET_HALTED && prev_target_state != TARGET_HALTED) {
    /* We have a halting debug event */
    LOG_DEBUG("Target %s halted", target_name(target));

#if 0
      ret = tricore_debug_entry(target);
      if (ret != ERROR_OK)
        return ret;
#endif

    /* TODO: Multi-core */

    switch (prev_target_state) {
    case TARGET_RUNNING:
    case TARGET_UNKNOWN:
    case TARGET_RESET:
      target_call_event_callbacks(target, TARGET_EVENT_HALTED);
      break;
    case TARGET_DEBUG_RUNNING:
      target_call_event_callbacks(target, TARGET_EVENT_DEBUG_HALTED);
      break;
    default:
      break;
    }
  }

  /* TODO: Maybe reset */

  return ret;
}

/* Invoked only from target_arch_state().
 * Issue USER() w/architecture specific status.  */
int aurix_arch_state(struct target *target) { return ERROR_FAIL; }

/* target request support */
int aurix_target_request_data(struct target *target, uint32_t size,
                              uint8_t *buffer) {
  return ERROR_FAIL;
}

int aurix_halt(struct target *target) {
  int ret = 0;

  if (target->state == TARGET_HALTED) {
    LOG_TARGET_DEBUG(target, "target already halted");
    return ERROR_OK;
  }

  ret = tricore_halt(target);
  if (ret) {
    LOG_TARGET_ERROR(target, "Failed to halt target");
    return ret;
  }

  target->debug_reason = DBG_REASON_DBGRQ;

  return ret;
}

int aurix_resume(struct target *target, int current, target_addr_t address,
                 int handle_breakpoints, int debug_execution) {
  struct tricore *tricore = target_to_tricore(target);
  int ret = 0;

  if (target->state == TARGET_RUNNING) {
    LOG_TARGET_DEBUG(target, "target already halted");
    return ERROR_OK;
  }

  if (current == 0) {
    ret = aurix_reg_set(tricore->pc, (uint8_t *)&address);
    if (ret) {
      LOG_TARGET_ERROR(target, "Failed to set PC before continue");
    }
  }

  ret = tricore_event_restore_all(target);
  if (ret) {
    LOG_TARGET_ERROR(target, "Failed to restore events");
    return ret;
  }

  ret = tricore_continue(target);
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
    // LOG_TARGET_DEBUG(target, "resumed at 0x%08" PRIx32, resume_pc);
  } else {
    target->state = TARGET_DEBUG_RUNNING;
    ret = target_call_event_callbacks(target, TARGET_EVENT_DEBUG_RESUMED);
    if (ret) {
      return ret;
    }
    // LOG_DEBUG("target debug resumed at 0x%08" PRIx32, resume_pc);
  }

  return ERROR_OK;
}

int aurix_step(struct target *target, int current, target_addr_t address,
               int handle_breakpoints) {
  int ret;

  if (target->state != TARGET_HALTED) {
    LOG_TARGET_ERROR(target, "not halted");
    return ERROR_TARGET_NOT_HALTED;
  }

  if (!current) {
    tricore_event_set_step(target, address);
  } else {
    tricore_event_set_single_step(target);
  }

  ret = target_call_event_callbacks(target, TARGET_EVENT_RESUMED);
  if (ret != ERROR_OK)
    return ret;

  ret = tricore_continue(target);
  if (ret) {
    LOG_TARGET_ERROR(target, "Failed to step target");
    return ret;
  }
  target->state = TARGET_RUNNING;

  return ERROR_OK;
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
int aurix_assert_reset(struct target *target) {
  enum reset_types reset_config = jtag_get_reset_config();

  /* Issue some kind of warm reset. */
  if (target_has_event_action(target, TARGET_EVENT_RESET_ASSERT))
    target_handle_event(target, TARGET_EVENT_RESET_ASSERT);
  else if (reset_config & RESET_HAS_SRST) {
    bool srst_asserted = false;

    if (target->reset_halt && !(reset_config & RESET_SRST_PULLS_TRST)) {
      if (target_was_examined(target)) {

        if (reset_config & RESET_SRST_NO_GATING) {
          /*
           * SRST needs to be asserted *before* Reset Catch
           * debug event can be set up.
           */
          adapter_assert_reset();
          srst_asserted = true;
        }

        /* Catch logic currently implemented in adapter */
      } else {
        LOG_WARNING(
            "%s: Target not examined, will not halt immediately after reset!",
            target_name(target));
      }
    }

    /* REVISIT handle "pulls" cases, if there's
     * hardware that needs them to work.
     */
    if (!srst_asserted)
      adapter_assert_reset();
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
int aurix_deassert_reset(struct target *target) {
  int ret;

  /* be certain SRST is off */
  adapter_deassert_reset();

  if (!target_was_examined(target))
    return ERROR_OK;

  ret = aurix_poll(target);
  if (ret != ERROR_OK)
    return ret;

  if (target->reset_halt) {
    /* Breakpoints clear */
    tricore_event_clear_all(target);

    if (target->state != TARGET_HALTED) {
      LOG_TARGET_WARNING(target, "ran after reset and before halt ...");
      if (target_was_examined(target)) {
        ret = aurix_halt(target);
        if (ret != ERROR_OK)
          return ret;
      } else {
        target->state = TARGET_UNKNOWN;
      }
    }
  }

  return ERROR_OK;
}
int aurix_soft_reset_halt(struct target *target) { return ERROR_FAIL; }

/**
 * Target architecture for GDB.
 *
 * The string returned by this function will not be automatically freed;
 * if dynamic allocation is used for this value, it must be managed by
 * the target, ideally by caching the result for subsequent calls.
 */
const char *aurix_get_gdb_arch(const struct target *target) {
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
int aurix_get_gdb_reg_list(struct target *target, struct reg **reg_list[],
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
int aurix_get_gdb_reg_list_noread(struct target *target,
                                  struct reg **reg_list[], int *reg_list_size,
                                  enum target_register_class reg_class) {
  return ERROR_FAIL;
}

/* target memory access
 * size: 1 = byte (8bit), 2 = half-word (16bit), 4 = word (32bit)
 * count: number of items of <size>
 */

/**
 * Target memory read callback.  Do @b not call this function
 * directly, use target_read_memory() instead.
 */
int aurix_read_memory(struct target *target, target_addr_t address,
                      uint32_t size, uint32_t count, uint8_t *buffer) {

  struct aurix_core *aurix = target_to_aurix(target);
  int ret;

  ret = aurix_ocds_queue_soc_read(aurix->ocds, address, size, count, buffer);
  if (ret) {
    LOG_ERROR("Failed to enque read");
    goto exit;
  }

  ret = aurix_ocds_run(aurix->ocds);
  if (ret) {
    LOG_ERROR("Failed to run ocds sequence");
    goto exit;
  }

exit:
  return ret;
}
/**
 * Target memory write callback.  Do @b not call this function
 * directly, use target_write_memory() instead.
 */
int aurix_write_memory(struct target *target, target_addr_t address,
                       uint32_t size, uint32_t count, const uint8_t *buffer) {
  struct aurix_core *aurix = target_to_aurix(target);
  int ret;

  ret = aurix_ocds_queue_soc_write(aurix->ocds, address, size, count, buffer);
  if (ret) {
    LOG_ERROR("Failed to queue write request");
    goto exit;
  }

  ret = aurix_ocds_run(aurix->ocds);
  if (ret) {
    LOG_ERROR("Failed to run OCDS sequence");
    goto exit;
  }

exit:
  return ret;
}

int aurix_checksum_memory(struct target *target, target_addr_t address,
                          uint32_t count, uint32_t *checksum) {
  return ERROR_FAIL;
}
int aurix_blank_check_memory(struct target *target,
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
int aurix_add_breakpoint(struct target *target, struct breakpoint *breakpoint) {
  return ERROR_FAIL;
}

/* remove breakpoint. hw will only be updated if the target
 * is currently halted.
 * However, this method can be invoked on unresponsive targets.
 */
int aurix_remove_breakpoint(struct target *target,
                            struct breakpoint *breakpoint) {
  return ERROR_FAIL;
}

/* add watchpoint ... see add_breakpoint() comment above. */
int aurix_add_watchpoint(struct target *target, struct watchpoint *watchpoint) {
  return ERROR_FAIL;
}

/* remove watchpoint. hw will only be updated if the target
 * is currently halted.
 * However, this method can be invoked on unresponsive targets.
 */
int aurix_remove_watchpoint(struct target *target,
                            struct watchpoint *watchpoint) {
  return ERROR_FAIL;
}

/* Find out just hit watchpoint. After the target hits a watchpoint, the
 * information could assist gdb to locate where the modified/accessed memory is.
 */
int aurix_hit_watchpoint(struct target *target,
                         struct watchpoint **hit_watchpoint) {
  return ERROR_FAIL;
}

/**
 * Target algorithm support.  Do @b not call this method directly,
 * use target_run_algorithm() instead.
 */
int aurix_run_algorithm(struct target *target, int num_mem_params,
                        struct mem_param *mem_params, int num_reg_params,
                        struct reg_param *reg_param, target_addr_t entry_point,
                        target_addr_t exit_point, unsigned int timeout_ms,
                        void *arch_info) {
  return ERROR_FAIL;
}
int aurix_start_algorithm(struct target *target, int num_mem_params,
                          struct mem_param *mem_params, int num_reg_params,
                          struct reg_param *reg_param,
                          target_addr_t entry_point, target_addr_t exit_point,
                          void *arch_info) {
  return ERROR_FAIL;
}
int aurix_wait_algorithm(struct target *target, int num_mem_params,
                         struct mem_param *mem_params, int num_reg_params,
                         struct reg_param *reg_param, target_addr_t exit_point,
                         unsigned int timeout_ms, void *arch_info) {
  return ERROR_FAIL;
}

static int aurix_tc_read_reg_u32(struct target *target, uint16_t addr,
                                 uint32_t *value) {
  struct aurix_core *aurix = target_to_aurix(target);
  return aurix_ocds_atomic_read_u32(aurix->ocds, aurix->base + 0x10000 + addr,
                                    value);
}

static int aurix_tc_write_reg_u32(struct target *target, uint16_t addr,
                                  uint32_t value) {
  struct aurix_core *aurix = target_to_aurix(target);
  return aurix_ocds_atomic_write_u32(aurix->ocds, aurix->base + 0x10000 + addr,
                                     value);
}

static int aurix_arch_info_init(struct target *target, struct aurix_core *aurix,
                                struct aurix_ocds *ocds) {
  aurix->type = target->tap->expected_ids[0] & AURIX_DT_VERSION_MASK_OUT;
  switch (aurix->family = aurix_get_device_family(aurix->type)) {
  case AURIX_DF_TC3X:
    aurix->base = 0xF8800000 + 0x20000 * target->coreid;
    aurix->tricore.version = TRICORE_1_6_2;
    break;
  case AURIX_DF_TC4X:
    aurix->base = 0xF8800000 + 0x40000 * target->coreid;
    aurix->tricore.version = TRICORE_1_8;
    break;
  default:
    return ERROR_TARGET_INVALID;
  }
  aurix->ocds = ocds;
  aurix->tricore.read_reg_u32 = aurix_tc_read_reg_u32;
  aurix->tricore.write_reg_u32 = aurix_tc_write_reg_u32;

  return tricore_arch_info_init(target, &aurix->tricore);
}

/* called when target is created */
int aurix_target_create(struct target *target, Jim_Interp *interp) {
  struct aurix_core *aurix = calloc(1, sizeof(struct aurix_core));
  struct aurix_private_config *aurix_cfg = target->private_config;
  if (!aurix) {
    LOG_TARGET_ERROR(target, "Failed to allocate target memory");
    return ERROR_FAIL;
  }

  return aurix_arch_info_init(target, aurix, aurix_cfg->ocds);
}

const struct command_registration *commands;
static const struct jim_nvp nvp_config_opts[] = {{.name = "-ocds", .value = 0},
                                                 {.name = NULL, .value = -1}};
/* called for various config parameters */
/* returns JIM_CONTINUE - if option not understood */
/* otherwise: JIM_OK, or JIM_ERR, */
int aurix_target_jim_configure(struct target *target,
                               struct jim_getopt_info *goi) {

  int e;
  struct jim_nvp *n;
  struct aurix_private_config *aurix_cfg =
      (struct aurix_private_config *)target->private_config;

  if (!goi->argc)
    return JIM_OK;

  if (aurix_cfg == NULL) {
    aurix_cfg = calloc(1, sizeof(struct aurix_private_config));
    if (!aurix_cfg) {
      LOG_ERROR("Out of memory");
      return JIM_ERR;
    }
    target->private_config = aurix_cfg;
  }

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
      struct aurix_ocds *ocds;
      e = jim_getopt_obj(goi, &o_t);
      if (e != JIM_OK)
        return e;
      ocds = aurix_ocds_by_jim_obj(goi->interp, o_t);
      if (!ocds) {
        Jim_SetResultString(goi->interp, "OCDS name invalid!", -1);
        return JIM_ERR;
      }
      if (aurix_cfg->ocds && aurix_cfg->ocds != ocds) {
        Jim_SetResultString(goi->interp, "OCDS assignment cannot be changed!",
                            -1);
        return JIM_ERR;
      }
      aurix_cfg->ocds = ocds;
    } else {
      if (goi->argc)
        goto err_no_param;
      if (!aurix_cfg->ocds) {
        Jim_SetResultString(goi->interp, "OCDS not configured", -1);
        return JIM_ERR;
      }
      Jim_SetResultString(goi->interp, aurix_cfg->ocds->name, -1);
    }
    break;
  }

  if (aurix_cfg->ocds) {
    if (target->tap_configured) {
      aurix_cfg->ocds = NULL;
      Jim_SetResultString(
          goi->interp,
          "-chain-position and -ocds configparams are mutually exclusive!", -1);
      return JIM_ERR;
    }
    target->tap = aurix_cfg->ocds->tap;
    target->dap_configured = true;
    target->has_dap = true;
  }

  return JIM_OK;

err_no_param:
  Jim_WrongNumArgs(goi->interp, goi->argc, goi->argv, "No parameters");
  return JIM_ERR;
}

/* target commands specifically handled by the target */
/* returns JIM_OK, or JIM_ERR, or JIM_CONTINUE - if option not understood */
int aurix_target_jim_commands(struct target *target,
                              struct jim_getopt_info *goi) {
  return JIM_OK;
}

/**
 * This method is used to perform target setup that requires
 * JTAG access.
 *
 * This may be called multiple times.  It is called after the
 * scan chain is initially validated, or later after the target
 * is enabled by a JRC.  It may also be called during some
 * parts of the reset sequence.
 *
 * For one-time initialization tasks, use target_was_examined()
 * and target_set_examined().  For example, probe the hardware
 * before setting up chip-specific state, and then set that
 * flag so you don't do that again.
 */
int aurix_examine(struct target *target) {
  int ret;
  if (!target_was_examined(target)) {
    ret = tricore_examine(target);
    if (ret) {
      LOG_TARGET_ERROR(target, "failed to examin tricore");
    }

    target_set_examined(target);
  }

  ret = tricore_event_clear_all(target);
  if (ret)
    return ret;

  ret = aurix_poll(target);
  if (ret)
    return ret;

  return ERROR_OK;
}

/* Set up structures for target.
 *
 * It is illegal to talk to the target at this stage as this fn is invoked
 * before the JTAG chain has been examined/verified
 * */
int aurix_init_target(struct command_context *cmd_ctx, struct target *target) {

  return tricore_build_reg_cache(target, &aurix_reg_type);
}

/**
 * Free all the resources allocated by the target.
 * @param target The target to deinit
 */
void aurix_deinit_target(struct target *target) {
  tricore_free_reg_cache(target);
  free(target->private_config);
  free(target_to_aurix(target));
}

/* after reset is complete, the target can check if things are properly set up.
 *
 * This can be used to check if e.g. DCC memory writes have been enabled for
 * arm7/9 targets, which they really should except in the most contrived
 * circumstances.
 */
int aurix_check_reset(struct target *target) { return ERROR_OK; }

/* Parse target-specific GDB query commands.
 * The string pointer "response_p" is always assigned by the called function
 * to a pointer to a NULL-terminated string, even when the function returns
 * an error. The string memory is not freed by the caller, so this function
 * must pay attention for possible memory leaks if the string memory is
 * dynamically allocated.
 */
int aurix_gdb_query_custom(struct target *target, const char *packet,
                           char **response_p) {
  return ERROR_FAIL;
}

unsigned int aurix_address_bits(struct target *target) { return 32; }

unsigned int aurix_data_bits(struct target *target) { return 32; }

static const struct command_registration aurix_commands[] = {
    COMMAND_REGISTRATION_DONE};

struct target_type aurix_target = {
    .name = "aurix",

    .poll = aurix_poll,
    .arch_state = aurix_arch_state,

    .halt = aurix_halt,
    .resume = aurix_resume,
    .step = aurix_step,

    .assert_reset = aurix_assert_reset,
    .deassert_reset = aurix_deassert_reset,
    .soft_reset_halt = aurix_soft_reset_halt,

    .read_memory = aurix_read_memory,
    .write_memory = aurix_write_memory,

    .checksum_memory = aurix_checksum_memory,

    .get_gdb_arch = aurix_get_gdb_arch,
    .get_gdb_reg_list = aurix_get_gdb_reg_list,

    .run_algorithm = aurix_run_algorithm,
    .start_algorithm = aurix_start_algorithm,
    .wait_algorithm = aurix_wait_algorithm,

    .add_breakpoint = aurix_add_breakpoint,
    .remove_breakpoint = aurix_remove_breakpoint,

    .add_watchpoint = aurix_add_watchpoint,
    .remove_watchpoint = aurix_remove_watchpoint,

    .target_create = aurix_target_create,

    .target_jim_configure = aurix_target_jim_configure,
    .target_jim_commands = aurix_target_jim_commands,

    .init_target = aurix_init_target,
    .examine = aurix_examine,
    .deinit_target = aurix_deinit_target,

    .commands = aurix_commands,
};
