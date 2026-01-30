#include <string.h>

#include "helper/binarybuffer.h"
#include "helper/command.h"
#include "helper/list.h"
#include "helper/log.h"
#include "jtag/adapter.h"
#include "jtag/drivers/tas_client/tas_am15_am14.h"
#include "jtag/drivers/tas_client/tas_pkt.h"
#include "jtag/ifxdap.h"
#include "jtag/jtag.h"
#include "server/server.h"
#include <jtag/interface.h>

#include "target/tricore/ocmts.h"
#include "tas_protocol.h"

#ifdef __WIN32__
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

struct tas_client_state {
  int sock;
  bool connected;
  uint8_t con_id;
  const char *ip_addr;
  tas_target_info_st *targets;
  size_t target_num;

  uint8_t *tx_buffer;
  void **pl0_resp_buffers;
  uint32_t pl0_rsps;
  uint32_t tx_offset;
  uint32_t rx_offset;

  uint32_t max_pl2rq_pkt_size;
  uint32_t max_pl2rsp_pkt_size;
  uint32_t pl0_max_num_rw;

  uint32_t current_address;
};

static struct tas_client_state client_state;

static inline int check_pl0_limits(size_t tx_size, size_t rx_size) {
  if (client_state.pl0_rsps >= client_state.pl0_max_num_rw) {
    LOG_ERROR("Exceeded maximum number of PL0 requests per OCMTS run");
    return ERROR_FAIL;
  }
  if (client_state.tx_offset + tx_size > client_state.max_pl2rq_pkt_size) {
    LOG_ERROR("Exceeded maximum PL2 request packet size");
    return ERROR_FAIL;
  }
  if (client_state.rx_offset + rx_size > client_state.max_pl2rsp_pkt_size) {
    LOG_ERROR("Exceeded maximum PL2 response packet size");
    return ERROR_FAIL;
  }
  return ERROR_OK;
}

static int tas_client_init(void) {
  struct sockaddr_in ipv4_sock_addr;

  client_state.con_id = 1;

  if (client_state.ip_addr == NULL) {
    client_state.ip_addr = "127.0.0.1";
  }
#ifdef __WIN32__
  ipv4_sock_addr.sin_addr.S_un.S_addr = inet_addr(client_state.ip_addr);
  if (ipv4_sock_addr.sin_addr.S_un.S_addr == INADDR_NONE) {
    LOG_ERROR("Invalid ip addr: %s", client_state.ip_addr);
    return ERROR_INVALID_NUMBER;
  }
#else
  if (inet_aton(client_state.ip_addr, &ipv4_sock_addr.sin_addr) == 0) {
    LOG_ERROR("Invalid ip addr: %s", client_state.ip_addr);
    return ERROR_INVALID_NUMBER;
  }
#endif
  ipv4_sock_addr.sin_family = AF_INET;
  ipv4_sock_addr.sin_port = htons(24817);

  LOG_INFO("Connecting to TAS server %s:%u", client_state.ip_addr, 24817);
  client_state.sock = socket(AF_INET, SOCK_STREAM, 0);
  if (client_state.sock == -1) {
    return ERROR_FAIL;
  }

  if (connect(client_state.sock, (struct sockaddr *)&ipv4_sock_addr,
              sizeof(struct sockaddr_in))) {
    LOG_ERROR("Failed to connect to tas server %s: %s", client_state.ip_addr,
              strerror(errno));
    return ERROR_CONNECTION_REJECTED;
  }

  if (tas_client_connect(client_state.sock) != 0) {
    LOG_ERROR("Failed to connect to TAS server");
    return ERROR_CONNECTION_REJECTED;
  }
  client_state.connected = true;

  if (tas_client_get_targets(client_state.sock, &client_state.targets,
                             &client_state.target_num) != 0) {
    LOG_ERROR("Failed to receive targets");
    return ERROR_FAIL;
  }

  if (client_state.target_num == 0) {
    LOG_ERROR("No targets to connect");
    return ERROR_FAIL;
  }

  const char *serial = adapter_get_required_serial();
  if (serial == NULL) {
    serial = client_state.targets[0].identifier;
  } else {
    bool found = false;
    for (size_t i = 0; i < client_state.target_num; i++) {
      if (strcmp(serial, client_state.targets[i].identifier) == 0) {
        found = true;
        break;
      }
    }
    if (!found) {
      LOG_ERROR("No target with serial %s found", serial);
      return ERROR_FAIL;
    }
  }

  LOG_INFO("Starting session for target %s", serial);

  tas_con_info_st con_info;
  int err = tas_client_session_start(client_state.sock, serial, &con_info);
  if (err) {
    LOG_ERROR("Failed to start session for target %s", serial);
    return ERROR_FAIL;
  }

  client_state.max_pl2rq_pkt_size = MIN(2048, con_info.max_pl2rq_pkt_size);
  client_state.max_pl2rsp_pkt_size = MIN(2048, con_info.max_pl2rsp_pkt_size);
  client_state.pl0_max_num_rw = MIN(128, con_info.pl0_max_num_rw);

  client_state.pl0_resp_buffers =
      malloc(sizeof(void *) * client_state.pl0_max_num_rw);
  client_state.tx_buffer = malloc(client_state.max_pl2rq_pkt_size);
  client_state.tx_offset = 16;
  client_state.rx_offset = 0;
  client_state.pl0_rsps = 0;

  if (!client_state.tx_buffer) {
    return ERROR_FAIL;
  }

  return ERROR_OK;
}

static int tas_client_quit(void) {

  close(client_state.sock);
  return 0;
}

static int tas_client_reset(int trst, int srst) {
  if (trst != 0) {
    return ERROR_NOT_IMPLEMENTED;
  }
  if (!client_state.connected) {
    return ERROR_FAIL;
  }
  if (srst) {
    tas_pl0rq_addr_map_st pl0rq_addr_map = {
        .cmd = TAS_PL0_CMD_ADDR_MAP,
        .wl = 0,
        .addr_map = TAS_AM15,
    };
    tas_pl0rq_base_addr32_st pl0rq_base_addr = {
        .cmd = TAS_PL0_CMD_BASE_ADDR32,
        .wl = 0,
        .ba31to16 = 0x000A,
    };
    tas_pl0rq_wr_st pl0wr_userctl = {
        .cmd = TAS_PL0_CMD_WR32,
        .wl = 1,
        .a15to0 = 0x8400,
        .data = TAS_UPC_ADD_SFP_RESET,
    };
    tas_pl0rq_wr64_st pl0rq_setreset = {
        .cmd = TAS_PL0_CMD_WR64,
        .wl = 1,
        .a15to0 = 0x8000,
        .data = {0, TAS_UP_SFP_RESET},
    };
    size_t tx_size = sizeof(pl0rq_addr_map) + sizeof(pl0rq_base_addr) +
                     sizeof(pl0wr_userctl) + sizeof(pl0rq_setreset);
    size_t rx_size = sizeof(tas_pl0rsp_st) * 3 + sizeof(tas_pl0rsp_wr_st);
    uint8_t tx_buffer[tx_size];
    uint8_t rx_buffer[rx_size];
    size_t offset = 0;
    tas_pl0rsp_st *pl0_resp[4];
    int err = check_pl0_limits(tx_size, rx_size);
    if (err != ERROR_OK)
      return err;
    memcpy(tx_buffer + offset, &pl0rq_addr_map, sizeof(pl0rq_addr_map));
    offset += sizeof(pl0rq_addr_map);
    memcpy(tx_buffer + offset, &pl0rq_base_addr, sizeof(pl0rq_base_addr));
    offset += sizeof(pl0rq_base_addr);
    memcpy(tx_buffer + offset, &pl0wr_userctl, sizeof(pl0wr_userctl));
    offset += sizeof(pl0wr_userctl);
    memcpy(tx_buffer + offset, &pl0rq_setreset, sizeof(pl0rq_setreset));
    offset += sizeof(pl0rq_setreset);
    return tas_client_send_pl0(client_state.sock, 0, tx_buffer, tx_size,
                               rx_buffer, &rx_size, pl0_resp, 4);
  } else {
    return tas_client_device_connect(client_state.sock,
                                     TAS_DEV_CON_FEAT_RESET_AND_HALT);
  }

  return 0;
}

static int tas_client_op_run(struct ocmts *ocds) {
  tas_pl0rsp_st *pl0rsps[client_state.pl0_rsps];
  uint8_t rx_buffer[client_state.max_pl2rsp_pkt_size];
  size_t rx_len = client_state.max_pl2rsp_pkt_size;
  int err;
  size_t i;

  client_state.tx_offset += sizeof(tas_pl1rq_pl0_end_st);
  err = tas_client_send_pl0(client_state.sock, ocds->con_id,
                            client_state.tx_buffer, client_state.tx_offset,
                            rx_buffer, &rx_len, pl0rsps, client_state.pl0_rsps);
  if (err) {
    LOG_ERROR("Failed to run ocmts sequence.");
    client_state.tx_offset = 16;
    client_state.rx_offset = 0;
    client_state.pl0_rsps = 0;
    return ERROR_FAIL;
  }

  for (i = 0; i < client_state.pl0_rsps; i++) {
    switch (pl0rsps[i]->cmd) {
    case TAS_PL0_CMD_RDBLK:
      memcpy(client_state.pl0_resp_buffers[i], pl0rsps[i] + 1,
             pl0rsps[i]->wl * 4);
      break;
    case TAS_PL0_CMD_RD8:
      memcpy(client_state.pl0_resp_buffers[i], pl0rsps[i] + 1, 1);
      break;
    case TAS_PL0_CMD_RD16:
      memcpy(client_state.pl0_resp_buffers[i], pl0rsps[i] + 1, 2);
      break;
    case TAS_PL0_CMD_RD32:
      memcpy(client_state.pl0_resp_buffers[i], pl0rsps[i] + 1, 4);
      break;
    case TAS_PL0_CMD_RDBLK1KB:
      memcpy(client_state.pl0_resp_buffers[i], pl0rsps[i] + 1, 1024);
      break;
    }
  }

  client_state.tx_offset = 16;
  client_state.rx_offset = 0;
  client_state.pl0_rsps = 0;

  return ERROR_OK;
}

static int tas_client_op_connect(struct ocmts *ocmts) {
  uint32_t i, j;
  tas_target_info_st *target = NULL;

  for (i = 0; i < client_state.target_num; i++) {
    for (j = 0; j < ocmts->tap->expected_ids_cnt; j++) {
      if (client_state.targets[i].device_type == ocmts->tap->expected_ids[j]) {
        target = &client_state.targets[i];
        break;
      }
    }
    if (target != NULL) {
      break;
    }
  }

  if (target == NULL) {
    LOG_ERROR("No matching target for OCDS %s found", ocmts->name);
    return ERROR_COMMAND_ARGUMENT_INVALID;
  }

  int err;

  enum reset_types jtag_reset_config = jtag_get_reset_config();

  if (jtag_reset_config & RESET_CNCT_UNDER_SRST) {
    err = tas_client_device_connect(client_state.sock,
                                    TAS_DEV_CON_FEAT_RESET_AND_HALT);
  } else {
    err = tas_client_device_connect(client_state.sock, TAS_DEV_CON_FEAT_NONE);
  }

  if (err) {
    LOG_ERROR("Failed to connect to device %s", target->identifier);
    return ERROR_FAIL;
  }

  return ERROR_OK;
}

static int tas_client_set_address(struct ocmts *ocds, uint32_t addr) {
  if ((client_state.current_address & 0xFFFF0000) != (addr & 0xFFFF0000)) {
    int err = check_pl0_limits(sizeof(tas_pl0rq_base_addr32_st),
                               sizeof(tas_pl0rsp_st));
    if (err != ERROR_OK)
      return err;

    tas_pl0rq_base_addr32_st pl0rq_base_addr = {
        .cmd = TAS_PL0_CMD_BASE_ADDR32,
        .wl = 0,
        .ba31to16 = (addr >> 16) & 0xFFFF,
    };

    memcpy(client_state.tx_buffer + client_state.tx_offset, &pl0rq_base_addr,
           sizeof(tas_pl0rq_base_addr32_st));
    client_state.tx_offset += sizeof(tas_pl0rq_base_addr32_st);
  }

  client_state.current_address = addr;

  return ERROR_OK;
}

static int tas_client_read_byte(struct ocmts *ocds, uint8_t *data) {
  int err = check_pl0_limits(sizeof(tas_pl0rq_rd_st),
                             sizeof(tas_pl0rsp_rd_st) + 1);
  if (err != ERROR_OK)
    return err;

  tas_pl0rq_rd_st pl0rq_read8 = {
      .cmd = TAS_PL0_CMD_RD8,
      .wl = 0,
      .a15to0 = client_state.current_address & 0xFFFF,
  };

  memcpy(client_state.tx_buffer + client_state.tx_offset, &pl0rq_read8,
         sizeof(tas_pl0rq_rd_st));
  client_state.tx_offset += sizeof(tas_pl0rq_rd_st);
  client_state.pl0_resp_buffers[client_state.pl0_rsps] = data;
  client_state.pl0_rsps++;

  return ERROR_OK;
}

static int tas_client_read_hword(struct ocmts *ocds, uint16_t *data) {
  int err = check_pl0_limits(sizeof(tas_pl0rq_rd_st),
                             sizeof(tas_pl0rsp_rd_st) + 2);
  if (err != ERROR_OK)
    return err;

  tas_pl0rq_rd_st pl0rq_read16 = {
      .cmd = TAS_PL0_CMD_RD16,
      .wl = 0,
      .a15to0 = client_state.current_address & 0xFFFF,
  };

  memcpy(client_state.tx_buffer + client_state.tx_offset, &pl0rq_read16,
         sizeof(tas_pl0rq_rd_st));
  client_state.tx_offset += sizeof(tas_pl0rq_rd_st);
  client_state.pl0_resp_buffers[client_state.pl0_rsps] = data;
  client_state.pl0_rsps++;

  return ERROR_OK;
}

static int tas_client_read_word(struct ocmts *ocds, uint32_t *data) {
  int err = check_pl0_limits(sizeof(tas_pl0rq_rd_st),
                             sizeof(tas_pl0rsp_rd_st) + 1);
  if (err != ERROR_OK)
    return err;

  tas_pl0rq_rd_st pl0rq_read32 = {
      .cmd = TAS_PL0_CMD_RD32,
      .wl = 0,
      .a15to0 = client_state.current_address & 0xFFFF,
  };

  memcpy(client_state.tx_buffer + client_state.tx_offset, &pl0rq_read32,
         sizeof(tas_pl0rq_rd_st));
  client_state.tx_offset += sizeof(tas_pl0rq_rd_st);
  client_state.pl0_resp_buffers[client_state.pl0_rsps] = data;
  client_state.pl0_rsps++;

  return ERROR_OK;
}

static int tas_client_write_byte(struct ocmts *ocds, uint8_t data) {
  int err = check_pl0_limits(sizeof(tas_pl0rq_wr_st), sizeof(tas_pl0rsp_wr_st));
  if (err != ERROR_OK)
    return err;

  tas_pl0rq_wr_st pl0rq_write8 = {
      .cmd = TAS_PL0_CMD_WR8,
      .wl = 1,
      .a15to0 = client_state.current_address & 0xFFFF,
      .data = data,
  };

  memcpy(client_state.tx_buffer + client_state.tx_offset, &pl0rq_write8,
         sizeof(tas_pl0rq_wr_st));
  client_state.tx_offset += sizeof(tas_pl0rq_wr_st);
  client_state.pl0_rsps++;

  return ERROR_OK;
}

static int tas_client_write_hword(struct ocmts *ocds, uint16_t data) {
  int err = check_pl0_limits(sizeof(tas_pl0rq_wr_st), sizeof(tas_pl0rsp_wr_st));
  if (err != ERROR_OK)
    return err;

  tas_pl0rq_wr_st pl0rq_write16 = {
      .cmd = TAS_PL0_CMD_WR16,
      .wl = 1,
      .a15to0 = client_state.current_address & 0xFFFF,
      .data = data,
  };

  memcpy(client_state.tx_buffer + client_state.tx_offset, &pl0rq_write16,
         sizeof(tas_pl0rq_wr_st));
  client_state.tx_offset += sizeof(tas_pl0rq_wr_st);
  client_state.pl0_rsps++;

  return ERROR_OK;
}

static int tas_client_write_word(struct ocmts *ocds, uint32_t data) {
  int err = check_pl0_limits(sizeof(tas_pl0rq_wr_st), sizeof(tas_pl0rsp_wr_st));
  if (err != ERROR_OK)
    return err;

  tas_pl0rq_wr_st pl0rq_write32 = {
      .cmd = TAS_PL0_CMD_WR32,
      .wl = 1,
      .a15to0 = client_state.current_address & 0xFFFF,
      .data = data,
  };

  memcpy(client_state.tx_buffer + client_state.tx_offset, &pl0rq_write32,
         sizeof(tas_pl0rq_wr_st));
  client_state.tx_offset += sizeof(tas_pl0rq_wr_st);
  client_state.pl0_rsps++;

  return ERROR_OK;
}

static int tas_client_write_block(struct ocmts *ocds, const void *data,
                                  size_t length) {
  if (length > 1024 || (length % 4) != 0) {
    LOG_ERROR("Block write length must be multiple of 4 and up to 1024 bytes");
    return ERROR_INVALID_NUMBER;
  }
  int err = check_pl0_limits(sizeof(tas_pl0rq_wrblk_st) + length,
                             sizeof(tas_pl0rsp_wr_st));
  if (err != ERROR_OK)
    return err;

  tas_pl0rq_wrblk_st pl0rq_write_block = {
      .cmd = TAS_PL0_CMD_WRBLK,
      .wl = (length == 1024) ? 0 : (length / 4) - 1,
      .a15to0 = client_state.current_address & 0xFFFF,
  };

  memcpy(client_state.tx_buffer + client_state.tx_offset, &pl0rq_write_block,
         sizeof(tas_pl0rq_wrblk_st));
  client_state.tx_offset += sizeof(tas_pl0rq_wrblk_st);
  client_state.pl0_rsps++;
  memcpy(client_state.tx_buffer + client_state.tx_offset, data, length);
  client_state.tx_offset += length;

  return ERROR_OK;
}

static int tas_client_read_block(struct ocmts *ocds, void *data,
                                 size_t length) {
  if (length > 1024 || (length % 4) != 0) {
    LOG_ERROR("Block read length must be multiple of 4 and up to 1024 bytes");
    return ERROR_INVALID_NUMBER;
  }
  int err = check_pl0_limits(sizeof(tas_pl0rq_rdblk_st),
                             sizeof(tas_pl0rsp_rd_st) + length);
  if (err != ERROR_OK)
    return err;

  tas_pl0rq_rdblk_st pl0rq_read_block = {
      .cmd = (length == 1024) ? TAS_PL0_CMD_RDBLK1KB : TAS_PL0_CMD_RDBLK,
      .wl = (length == 1024) ? 0 : (length / 4) - 1,
      .a15to0 = client_state.current_address & 0xFFFF,
  };

  memcpy(client_state.tx_buffer + client_state.tx_offset, &pl0rq_read_block,
         sizeof(tas_pl0rq_rdblk_st));
  client_state.tx_offset += sizeof(tas_pl0rq_rdblk_st);
  client_state.pl0_resp_buffers[client_state.pl0_rsps] = data;
  client_state.pl0_rsps++;

  return ERROR_OK;
}

static const struct ocmts_ops ocmts_ops_interface = {
    .connect = tas_client_op_connect,
    .queue_io_set_address = tas_client_set_address,
    .queue_io_read_byte = tas_client_read_byte,
    .queue_io_read_hword = tas_client_read_hword,
    .queue_io_read_word = tas_client_read_word,
    .queue_io_write_byte = tas_client_write_byte,
    .queue_io_write_hword = tas_client_write_hword,
    .queue_io_write_word = tas_client_write_word,
    .queue_io_write_block = tas_client_write_block,
    .queue_io_read_block = tas_client_read_block,
    .run = tas_client_op_run,
};

static int tas_client_speed(int speed) {
  tas_pl0rq_addr_map_st pl0rq_addr_map = {
      .cmd = TAS_PL0_CMD_ADDR_MAP,
      .wl = 0,
      .addr_map = TAS_AM15,
  };
  tas_pl0rq_base_addr32_st pl0rq_base_addr = {
      .cmd = TAS_PL0_CMD_BASE_ADDR32,
      .wl = 0,
      .ba31to16 = (TAS_AM15_RW_ACC_HW_FREQUENCY >> 16) & 0xFFFF,
  };
  tas_pl0rq_wr_st pl0wr_userctl = {
      .cmd = TAS_PL0_CMD_WR32,
      .wl = 1,
      .a15to0 = (TAS_AM15_RW_ACC_HW_FREQUENCY & 0xFFFF),
      .data = speed * 1000,
  };
  size_t tx_len =
      sizeof(pl0rq_addr_map) + sizeof(pl0rq_base_addr) + sizeof(pl0wr_userctl);
  size_t rx_len = sizeof(tas_pl0rsp_st) * 3;
  uint8_t tx_buffer[tx_len];
  uint8_t rx_buffer[rx_len];
  size_t offset = 0;
  tas_pl0rsp_st *pl0_resp[3];
  int err = ERROR_OK;
  memcpy(tx_buffer + offset, &pl0rq_addr_map, sizeof(pl0rq_addr_map));
  offset += sizeof(pl0rq_addr_map);
  memcpy(tx_buffer + offset, &pl0rq_base_addr, sizeof(pl0rq_base_addr));
  offset += sizeof(pl0rq_base_addr);
  memcpy(tx_buffer + offset, &pl0wr_userctl, sizeof(pl0wr_userctl));
  offset += sizeof(pl0wr_userctl);

  err = tas_client_send_pl0(client_state.sock, client_state.con_id, tx_buffer,
                            tx_len, rx_buffer, &rx_len, pl0_resp, 3);

  if (err) {
    LOG_ERROR("Failed to set adapter speed.");
    return ERROR_FAIL;
  }
  return ERROR_OK;
}

static int tas_client_speed_div(int speed, int *khz) {
  *khz = speed;
  return ERROR_OK;
}

static int tas_client_khz(int khz, int *speed) {
  *speed = khz;
  return ERROR_OK;
}

static int tas_client_dap_init(void) { return ERROR_OK; }

static struct ifxdap_driver ifxdap_ops = {
    .init = tas_client_dap_init,
};

static const char *const tas_client_transports[] = {"ifxdap", "jtag", NULL};

struct adapter_driver tas_client_adapter_driver = {
    .name = "tas_client",
    .transports = tas_client_transports,
    .ocmts_ops = &ocmts_ops_interface,
    .ifxdap_ops = &ifxdap_ops,
    .init = tas_client_init,
    .quit = tas_client_quit,
    .reset = tas_client_reset,
    .speed = tas_client_speed,
    .speed_div = tas_client_speed_div,
    .khz = tas_client_khz,
};
