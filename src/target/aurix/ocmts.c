// SPDX-License-Identifier: GPL-2.0-or-later

/***************************************************************************
 *   OCMTS Interface Module for Infineon AURIX                             *
 *   Copyright (C) 2026 Infineon Technologies AG                           *
 ***************************************************************************/

#include <string.h>

#include <helper/command.h>
#include <helper/jim-nvp.h>
#include <helper/list.h>
#include <helper/log.h>
#include <jim.h>
#include <jtag/adapter.h>
#include <jtag/ifxdap.h>
#include <jtag/interface.h>
#include <jtag/jtag.h>
#include <target/aurix/aurix_device_family.h>
#include <transport/transport.h>

#include "ocmts.h"

static OOCD_LIST_HEAD(all_ocmts);

struct ocmts *ocmts_by_jim_obj(Jim_Interp *interp, Jim_Obj *o) {
  struct ocmts *ocmts;
  const char *name = Jim_GetString(o, NULL);

  list_for_each_entry(ocmts, &all_ocmts, lh) {
    if (strcmp(name, ocmts->name) == 0) {
      return ocmts;
    }
  }

  return NULL;
}

enum dap_cfg_param {
  CFG_CHAIN_POSITION,
};

static const struct jim_nvp nvp_config_opts[] = {
    {.name = "-chain-position", .value = CFG_CHAIN_POSITION},
    {.name = NULL, .value = -1}};

static int ocmts_configure(struct jim_getopt_info *goi, struct ocmts *ocmts) {
  struct jim_nvp *n;
  int e;
  const char *name;

  jim_getopt_string(goi, &name, NULL);
  ocmts->name = strdup(name);

  /* parse config ... */
  while (goi->argc > 0) {
    Jim_SetEmptyResult(goi->interp);

    e = jim_getopt_nvp(goi, nvp_config_opts, &n);
    if (e != JIM_OK) {
      jim_getopt_nvp_unknown(goi, nvp_config_opts, 0);
      return e;
    }
    switch (n->value) {
    case CFG_CHAIN_POSITION: {
      Jim_Obj *o_t;
      e = jim_getopt_obj(goi, &o_t);
      if (e != JIM_OK)
        return e;

      struct jtag_tap *tap;
      tap = jtag_tap_by_jim_obj(goi->interp, o_t);
      if (!tap) {
        Jim_SetResultString(goi->interp, "-chain-position is invalid", -1);
        return JIM_ERR;
      }
      ocmts->tap = tap;
      /* loop for more */
      break;
    }
    default:
      break;
    }
  }

  return JIM_OK;
}

COMMAND_HANDLER(ocmts_create_cmd) {
  struct jim_getopt_info goi;
  jim_getopt_setup(&goi, CMD_CTX->interp, CMD_ARGC - 1, CMD_JIMTCL_ARGV + 1);
  if (goi.argc < 2) {
    Jim_WrongNumArgs(goi.interp, goi.argc, goi.argv,
                     "<name> [<ocmts_options> ...]");
    return ERROR_COMMAND_SYNTAX_ERROR;
  }
  struct ocmts *ocmts = calloc(1, sizeof(struct ocmts));
  if (!ocmts) {
    return ERROR_FAIL;
  }

  int e = ocmts_configure(&goi, ocmts);
  if (e != JIM_OK) {
    free(ocmts);
    return ERROR_FAIL;
  }

  list_add_tail(&ocmts->lh, &all_ocmts);

  return ERROR_OK;
}

static int ocmts_init(struct ocmts *ocmts) {
  int err;
  uint32_t pat0 = 0xA1, pat1 = 0x5E, ostate;
  uint32_t oec_addr = 0xF0000478;
  uint32_t ostate_addr = 0xF0000480;
  
  if (aurix_df_check_if_tc4x(ocmts->tap->idcode)) {
    oec_addr = 0xFA180068;
    ostate_addr = 0xFA18006C;
  }

  ocmts_queue_io_set_address(ocmts, ostate_addr);
  ocmts_queue_io_read_word(ocmts, &ostate);
  err = ocmts->ops->run(ocmts);
  if (err) {
    LOG_WARNING(
        "[OCMTS] was not accassible via IO client. Sending adapter reset");
    adapter_assert_reset();
    ocmts->ops->queue_io_set_ojconf(ocmts, 0x3);
    adapter_deassert_reset();
    ocmts_queue_io_set_address(ocmts, ostate_addr);
    ocmts_queue_io_read_word(ocmts, &ostate);
    err = ocmts->ops->run(ocmts);
    if (err) {
      LOG_ERROR("[OCMTS] is not accessible via IO client after adapter reset");
      return err;
    }
  } else {
    if (ostate & 0x1) {
      /* already initialized */
      return ERROR_OK;
    }
  }
  if (ostate & 0x10000) {
    LOG_ERROR("[OCMTS] Device is still locked");
    return ERROR_FAIL;
  }

  ocmts_queue_io_set_address(ocmts, oec_addr);
  ocmts_queue_io_write_word(ocmts, &pat0);
  ocmts_queue_io_write_word(ocmts, &pat1);
  ocmts_queue_io_write_word(ocmts, &pat0);
  ocmts_queue_io_write_word(ocmts, &pat1);
  ocmts_queue_io_set_address(ocmts, ostate_addr);
  ocmts_queue_io_read_word(ocmts, &ostate);
  err = ocmts->ops->run(ocmts);
  if (err) {
    return err;
  }
  if (ostate & 0x1) {
    /* already initialized */
    return ERROR_OK;
  } else {
    LOG_ERROR("[OCMTS] Failed to enable debug");
    return ERROR_FAIL;
  }
}

COMMAND_HANDLER(ocmts_init_cmd) {
  struct ocmts *ocmts;

  list_for_each_entry(ocmts, &all_ocmts, lh) {
    /* skip taps that are disabled */
    if (!ocmts->tap->enabled)
      continue;

    if (transport_is_ifxdap()) {
      ocmts->ops = adapter_driver->ocmts_ops;
      int err = ocmts->ops->connect(ocmts);
      if (err) {
        return err;
      }
    }
    ocmts_init(ocmts);
  }

  return ERROR_OK;
}

static const struct command_registration ocmts_subcommand_handlers[] = {
    {
        .name = "init",
        .mode = COMMAND_ANY,
        .handler = ocmts_init_cmd,
        .usage = "",
        .help = "Initialize all OCMTS systems",
    },
    {
        .name = "create",
        .mode = COMMAND_ANY,
        .handler = ocmts_create_cmd,
        .usage = "name '-chain-position' name",
        .help = "Creates a new DAP instance",
    },
    COMMAND_REGISTRATION_DONE};

static const struct command_registration ocmts_commands[] = {
    {
        .name = "ocmts",
        .mode = COMMAND_CONFIG,
        .help = "OCMTS commands",
        .chain = ocmts_subcommand_handlers,
        .usage = "",
    },
    COMMAND_REGISTRATION_DONE};

int ocmts_register_commands(struct command_context *cmd_ctx) {
  return register_commands(cmd_ctx, NULL, ocmts_commands);
}
