// SPDX-License-Identifier: GPL-2.0-or-later

/***************************************************************************
 *   TAS Client Protocol API for Infineon AURIX                            *
 *   Copyright (C) 2026 Infineon Technologies AG                           *
 ***************************************************************************/

#ifndef OPENOCD_JTAG_DRIVERS_TAS_CLIENT_TAS_PROTOCOL_H
#define OPENOCD_JTAG_DRIVERS_TAS_CLIENT_TAS_PROTOCOL_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "tas_pkt.h"

struct tas_client {
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

struct tas_client_mem_req {
  uint32_t addr;
  void *buffer;
  size_t length;
  bool is_read;
};

int tas_client_connect(struct tas_client *client, int sock);
int tas_client_device_connect(struct tas_client *client,
                              tas_dev_con_feat_et dev_con_feat);
int tas_client_get_targets(struct tas_client *client,
                           tas_target_info_st **targets, size_t *target_num);

int tas_client_ping(struct tas_client *client, tas_pl1rsp_ping_st *rsp_ping);
int tas_client_session_start(struct tas_client *client, const char *device);
int tas_client_execute_mem_req(struct tas_client *client, uint8_t addr_map,
                               struct tas_client_mem_req *mem_req);
int tas_client_execute_mem_reqs(struct tas_client *client, uint8_t addr_map,
                                struct tas_client_mem_req *mem_reqs,
                                size_t mem_req_num);
#endif // !OPENOCD_JTAG_DRIVERS_TAS_CLIENT_TAS_PROTOCOL_H
