#include <stdio.h>
#include <string.h>

#include <helper/command.h>
#include <helper/jim-nvp.h>
#include <helper/list.h>
#include <jim.h>
#include <jtag/adapter.h>
#include <jtag/interface.h>
#include <jtag/ifxdap.h>
#include <transport/transport.h>

#include "ocmts.h"

static LIST_HEAD(all_ocmts);

extern struct adapter_driver *adapter_driver;

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

static int ocmts_create(Jim_Interp *interp, int argc, Jim_Obj *const *argv) {
  struct jim_getopt_info goi;
  jim_getopt_setup(&goi, interp, argc - 1, argv + 1);
  if (goi.argc < 2) {
    Jim_WrongNumArgs(goi.interp, goi.argc, goi.argv,
                     "<name> [<ocmts_options> ...]");
    return JIM_ERR;
  }
  struct ocmts *ocmts = calloc(1, sizeof(struct ocmts));
  if (!ocmts) {
    return JIM_ERR;
  }

  ocmts_configure(&goi, ocmts);

  list_add_tail(&ocmts->lh, &all_ocmts);

  return 0;
}

COMMAND_HANDLER(ocmts_init) {
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
  }

  return JIM_OK;
}

static const struct command_registration ocmts_subcommand_handlers[] = {
    {
        .name = "init",
        .mode = COMMAND_ANY,
        .handler = ocmts_init,
        .usage = "",
        .help = "Initialize all OCMTS systems",
    },
    {
        .name = "create",
        .mode = COMMAND_ANY,
        .jim_handler = ocmts_create,
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