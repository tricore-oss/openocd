#include "jtag/drivers/tas_client/tas_protocol.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <time.h>
#include <unistd.h>

#ifdef __WIN32__
#include <winsock2.h>
#define MSG_MORE MSG_PARTIAL
#include <windows.h>
#else
#include <sys/socket.h>
#endif

#include "helper/log.h"
#include "tas_pkt.h"

#ifdef __WIN32__
#define MIN(a, b)                                                              \
  ({                                                                           \
    __typeof__(a) _a = (a);                                                    \
    __typeof__(b) _b = (b);                                                    \
    _a < _b ? _a : _b;                                                         \
  })
#endif

int tas_client_connect(int sock) {
  tas_pl1rq_server_connect_st rq_server_connect;
  tas_pl1rsp_server_connect_st rsp_server_connect;
  uint32_t packet_size;

  packet_size = 4 + sizeof(tas_pl1rq_server_connect_st);
  rq_server_connect.wl = sizeof(tas_pl1rq_server_connect_st) / 4 - 1;
  rq_server_connect.cmd = TAS_PL1_CMD_SERVER_CONNECT;
  rq_server_connect.reserved = 0;
  snprintf(rq_server_connect.client_name, TAS_NAME_LEN32, "openocd");
#ifdef __WIN32__
  DWORD size = TAS_NAME_LEN16;
  GetUserNameA(rq_server_connect.user_name, &size);
#else
  getlogin_r(rq_server_connect.user_name, TAS_NAME_LEN16);
#endif
  rq_server_connect.client_pid = getpid();

  char buf[packet_size];
  memcpy(buf, &packet_size, 4);
  memcpy(&buf[4], &rq_server_connect, sizeof(rq_server_connect));

  if (send(sock, (const char *)buf, packet_size, 0) < 0) {
    return ERROR_FAIL;
  }

  if (recv(sock, (char *)&packet_size, 4, 0) != 4) {
    return ERROR_FAIL;
  }
  if (recv(sock, (char *)&rsp_server_connect,
           sizeof(tas_pl1rsp_server_connect_st), 0) < 0) {
    return ERROR_FAIL;
  }

  if (rsp_server_connect.cmd != TAS_PL1_CMD_SERVER_CONNECT ||
      rsp_server_connect.err != TAS_PL_ERR_NO_ERROR) {
    return ERROR_FAIL;
  }

  return 0;
}

int tas_client_session_start(int sock, const char *device,
                             tas_con_info_st *con_info) {
  tas_pl1rq_session_start_st rq_session_start;
  tas_pl1rsp_session_start_st rsp_session_start;
  uint32_t packet_size;

  packet_size = 4 + sizeof(tas_pl1rq_session_start_st);
  rq_session_start.wl = sizeof(tas_pl1rq_session_start_st) / 4 - 1;
  rq_session_start.cmd = TAS_PL1_CMD_SESSION_START;
  rq_session_start.con_id = 0;
  rq_session_start.client_type = TAS_CLIENT_TYPE_RW;
  strncpy(rq_session_start.identifier, device, TAS_NAME_LEN64);
  rq_session_start.identifier[TAS_NAME_LEN64 - 1] = '\0';
  snprintf(rq_session_start.session_name, TAS_NAME_LEN16, "openocd%u",
           getpid());
  rq_session_start.session_pw[0] = 0;

  char buf[packet_size];
  memcpy(buf, &packet_size, 4);
  memcpy(&buf[4], &rq_session_start, sizeof(rq_session_start));

  if (send(sock, (const char *)&buf, packet_size, 0) < 0) {
    return ERROR_FAIL;
  }

  if (recv(sock, (char *)&packet_size, 4, 0) != 4) {
    return ERROR_FAIL;
  }
  if (recv(sock, (char *)&rsp_session_start,
           sizeof(tas_pl1rsp_session_start_st), 0) < 0) {
    return ERROR_FAIL;
  }

  if (rsp_session_start.cmd != TAS_PL1_CMD_SESSION_START ||
      rsp_session_start.con_id != 0 ||
      rsp_session_start.err != TAS_PL_ERR_NO_ERROR) {
    return ERROR_FAIL;
  }

  if (rsp_session_start.num_instances > 0) {
    return ERROR_FAIL;
  }

  if (con_info) {
    *con_info = rsp_session_start.con_info;
  }

  return 0;
}

int tas_client_device_connect(int sock, tas_dev_con_feat_et dev_con_feat) {
  tas_pl1rq_device_connect_st rq_device_connect;
  tas_pl1rsp_device_connect_st rsp_device_connect;
  uint32_t packet_size;

  packet_size = 4 + sizeof(tas_pl1rq_device_connect_st);
  rq_device_connect.wl = sizeof(tas_pl1rq_device_connect_st) / 4 - 1;
  rq_device_connect.cmd = TAS_PL1_CMD_DEVICE_CONNECT;
  rq_device_connect.con_id = 0xFF;
  rq_device_connect.reserved = 0;
  rq_device_connect.option = dev_con_feat;
  rq_device_connect.reserved1 = 0;

  char buf[packet_size];
  memcpy(buf, &packet_size, 4);
  memcpy(&buf[4], &rq_device_connect, sizeof(rq_device_connect));

  if (send(sock, (const char *)buf, packet_size, 0) < 0) {
    return ERROR_FAIL;
  }

  if (recv(sock, (char *)&packet_size, 4, 0) != 4) {
    return ERROR_FAIL;
  }
  if (recv(sock, (char *)&rsp_device_connect,
           sizeof(tas_pl1rsp_device_connect_st), 0) < 0) {
    return ERROR_FAIL;
  }

  if (rsp_device_connect.cmd != TAS_PL1_CMD_DEVICE_CONNECT ||
      rsp_device_connect.err != TAS_PL_ERR_NO_ERROR) {
    return ERROR_FAIL;
  }

  if (rsp_device_connect.feat_used != dev_con_feat) {
    return ERROR_FAIL;
  }

  return 0;
}

int tas_client_get_targets(int sock, tas_target_info_st **targets,
                           size_t *target_num) {
  tas_pl1rq_get_targets_st rq_get_targets;
  tas_pl1rsp_get_targets_st rsp_get_targets;
  uint32_t packet_size;
  if (targets == NULL) {
    return ERROR_FAIL;
  }

  packet_size = 4 + sizeof(tas_pl1rq_get_targets_st);
  rq_get_targets.cmd = TAS_PL1_CMD_GET_TARGETS;
  rq_get_targets.wl = 0;
  rq_get_targets.start_index = 0;

  char buf[packet_size];
  memcpy(buf, &packet_size, 4);
  memcpy(&buf[4], &rq_get_targets, sizeof(rq_get_targets));

  if (send(sock, (const char *)buf, packet_size, 0) < 0) {
    return ERROR_FAIL;
  }

  if (recv(sock, (char *)&packet_size, 4, 0) != 4) {
    return ERROR_FAIL;
  }
  if (recv(sock, (char *)&rsp_get_targets, sizeof(tas_pl1rsp_get_targets_st),
           0) < 0) {
    return ERROR_FAIL;
  }

  if (rsp_get_targets.cmd != TAS_PL1_CMD_GET_TARGETS ||
      rsp_get_targets.err != TAS_PL_ERR_NO_ERROR) {
    return ERROR_FAIL;
  }

  *target_num = rsp_get_targets.num_target;
  /* Limit number of targets supported */
  if (*target_num > 32) {
    return ERROR_FAIL;
  }
  if (*target_num > 0) {
    *targets = calloc(*target_num, sizeof(tas_target_info_st));
    if (*targets == NULL) {
      return ERROR_FAIL;
    }
    if (recv(sock, (char *)*targets, *target_num * sizeof(tas_target_info_st),
             0) < 0) {
      return ERROR_FAIL;
    }
  }

  return ERROR_OK;
}

int tas_client_ping(int sock, tas_pl1rsp_ping_st *rsp_ping) {
  tas_pl1rq_ping_st rq_ping;
  uint32_t packet_size;

  packet_size = 4 + sizeof(tas_pl1rq_ping_st);
  rq_ping.wl = sizeof(tas_pl1rq_ping_st) / 4 - 1;
  rq_ping.cmd = TAS_PL1_CMD_PING;
  rq_ping.con_id = 0x0;
  rq_ping.reserved = 0;

  char buf[packet_size];
  memcpy(buf, &packet_size, 4);
  memcpy(&buf[4], &rq_ping, sizeof(rq_ping));

  if (send(sock, (const char *)buf, packet_size, 0) < 0) {
    return ERROR_FAIL;
  }

  if (recv(sock, (char *)&packet_size, 4, 0) != 4) {
    return ERROR_FAIL;
  }
  if (recv(sock, (char *)rsp_ping, sizeof(tas_pl1rsp_ping_st), 0) < 0) {
    return ERROR_FAIL;
  }

  if (rsp_ping->cmd != TAS_PL1_CMD_PING ||
      rsp_ping->err != TAS_PL_ERR_NO_ERROR) {
    return ERROR_FAIL;
  }

  return ERROR_OK;
}

enum {
  PROTOC_VER = 0 //!< \brief TasPkt protocol version implemented in this class
};

static uint16_t pl1_count = 0;

struct tas_client_pl0_req {
  uint32_t addr;
  uint8_t *buffer;
  uint8_t cmd;
};

int tas_client_send_pl0(int sock, uint8_t con_id, uint8_t *tx_buffer,
                        uint32_t tx_len, uint8_t *rx_buffer, size_t *rx_len,
                        tas_pl0rsp_st **pl0_resp, size_t pl0_elements) {
  uint32_t packet_size;
  tas_pl1rq_pl0_start_st rq_start = {
      .cmd = TAS_PL1_CMD_PL0_START,
      .wl = 0,
      .con_id = con_id,
      .pl0_addr_map_mask = 1,
      .pl1_cnt = pl1_count++,
      .protoc_ver = PROTOC_VER,

  };
  tas_pl0rq_addr_map_st rq_addr_map = {
      .addr_map = 0, .cmd = TAS_PL0_CMD_ADDR_MAP, .wl = 0};
  tas_pl1rq_pl0_end_st rq_end = {
      .wl = 0, .cmd = TAS_PL1_CMD_PL0_END, .num_pl0_rw = pl0_elements + 1};
  tas_pl1rsp_pl0_start_st rsp_start;
  tas_pl1rsp_pl0_end_st rsp_end;

  memcpy(tx_buffer, &tx_len, 4);
  memcpy(tx_buffer + 4, &rq_start, sizeof(tas_pl1rq_pl0_start_st));
  memcpy(tx_buffer + 12, &rq_addr_map, sizeof(tas_pl0rq_addr_map_st));
  memcpy(tx_buffer + tx_len - sizeof(tas_pl1rq_pl0_end_st), &rq_end,
         sizeof(tas_pl1rq_pl0_end_st));

  if (send(sock, (const char *)tx_buffer, tx_len, 0) < 0) {
    return ERROR_FAIL;
  }

  if (recv(sock, (char *)rx_buffer, 4, 0) != 4) {
    return ERROR_FAIL;
  }
  memcpy(&packet_size, rx_buffer, 4);
  if (packet_size > *rx_len) {
    return ERROR_FAIL;
  }
  *rx_len = packet_size;

  if (recv(sock, (char *)(rx_buffer + 4), packet_size - 4, 0) < 0) {
    return ERROR_FAIL;
  }
  memcpy(&rsp_start, rx_buffer + 4, sizeof(tas_pl1rsp_pl0_start_st));
  memcpy(&rsp_end, rx_buffer + packet_size - sizeof(tas_pl1rsp_pl0_start_st),
         sizeof(tas_pl1rsp_pl0_end_st));

  if (rsp_start.err != TAS_PL_ERR_NO_ERROR ||
      rsp_end.pl1_cnt != rq_start.pl1_cnt) {
    return ERROR_FAIL;
  }

  size_t i = 0;
  size_t offset = 4 + sizeof(tas_pl1rsp_pl0_start_st);
  for (i = 0; i < rq_end.num_pl0_rw; i++) {
    pl0_resp[i] = (tas_pl0rsp_st *)(rx_buffer + offset);
    offset += ((pl0_resp[i]->wl + 1) * 4);
  }

  return ERROR_OK;
}
