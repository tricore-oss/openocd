#ifndef OPENOCD_JTAG_DRIVERS_TAS_CLIENT_TAS_PROTOCOL_H
#define OPENOCD_JTAG_DRIVERS_TAS_CLIENT_TAS_PROTOCOL_H

#include <stdio.h>

#include "tas_pkt.h"

int tas_client_connect(int sock);
int tas_client_device_connect(int sock, tas_dev_con_feat_et dev_con_feat);
int tas_client_get_targets(int sock, tas_target_info_st **targets,
                           size_t *target_num);

int tas_client_ping(int sock, tas_pl1rsp_ping_st *rsp_ping);
int tas_client_session_start(int sock, const char *device,
                             tas_con_info_st *con_info);
int tas_client_send_pl0(int sock, uint8_t con_id, uint8_t *tx_buffer,
                        uint32_t tx_len, uint8_t *rx_buffer, size_t *rx_len,
                        tas_pl0rsp_st **pl0_resp, size_t pl0_elements);
#endif // !OPENOCD_JTAG_DRIVERS_TAS_CLIENT_TAS_PROTOCOL_H
