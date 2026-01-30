
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <jtag/ifxdap.h>
#include <jtag/interface.h>
#include <transport/transport.h>

static const struct command_registration ifxdap_commands[] = {
    {
        .name = "newtap",
        .mode = COMMAND_CONFIG,
        .handler = handle_jtag_newtap,
        .help = "declare a new TAP",
        .usage = "basename tap_type '-irlen' count "
                 "['-enable'|'-disable'] "
                 "['-expected_id' number] "
                 "['-ignore-version'] "
                 "['-ignore-bypass'] "
                 "['-ircapture' number] "
                 "['-ir-bypass' number] "
                 "['-mask' number]",
    },
    COMMAND_REGISTRATION_DONE};

static const struct command_registration ifxdap_handlers[] = {
    {
        .name = "ifxdap",
        .mode = COMMAND_ANY,
        .help = "Infineon DAP commands",
        .chain = ifxdap_commands,
        .usage = "",
    },
    COMMAND_REGISTRATION_DONE};

static int ifxdap_select(struct command_context *ctx) {
  /* FIXME: only place where global 'adapter_driver' is still needed */
  extern struct adapter_driver *adapter_driver;
  const struct ifxdap_driver *ifxdap = adapter_driver->ifxdap_ops;
  int retval;

  retval = register_commands(ctx, NULL, ifxdap_handlers);
  if (retval != ERROR_OK)
    return retval;

  /* be sure driver is in SWD mode; start
   * with hardware default TRN (1), it can be changed later
   */
  if (!ifxdap || !ifxdap->init) {
    LOG_DEBUG("no DAP driver?");
    return ERROR_FAIL;
  }

  retval = ifxdap->init();
  if (retval != ERROR_OK) {
    LOG_DEBUG("can't init DAP driver");
    return retval;
  }

  return retval;
}

static int ifxdap_init(struct command_context *ctx) {
  /* nothing done here, SWD is initialized
   * together with the DAP */
  return ERROR_OK;
}

static struct transport ifxdap_transport = {
    .name = "ifxdap",
    .select = ifxdap_select,
    .init = ifxdap_init,
};

static void ifxdap_constructor(void) __attribute__((constructor));
static void ifxdap_constructor(void) { transport_register(&ifxdap_transport); }

/** Returns true if the current debug session
 * is using SWD as its transport.
 */
bool transport_is_ifxdap(void) {
  return get_current_transport() == &ifxdap_transport;
}
