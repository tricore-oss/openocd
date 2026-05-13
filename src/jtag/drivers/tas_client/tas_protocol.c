// SPDX-License-Identifier: GPL-2.0-or-later

/***************************************************************************
 *   TAS Client Protocol Implementation for Infineon AURIX                 *
 *   Copyright (C) 2026 Infineon Technologies AG                           *
 ***************************************************************************/

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <time.h>
#include <unistd.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#ifdef __WIN32__
#include <winsock2.h>
#define MSG_MORE MSG_PARTIAL
#include <windows.h>
#endif

#include <helper/log.h>

#include "tas_pkt.h"
#include "tas_protocol.h"

#ifdef __WIN32__
#define MIN(a, b)                                                                                                      \
	({                                                                                                                 \
		__typeof__(a) _a = (a);                                                                                        \
		__typeof__(b) _b = (b);                                                                                        \
		_a < _b ? _a : _b;                                                                                             \
	})
#endif

#define TAS_OUTSTANDING_MAX 8

struct tas_outstanding_pkt {
	bool in_use;
	uint16_t pl1_cnt;
	size_t start_mem_req;
	size_t end_mem_req;
	size_t expected_rx_size;
};

int tas_client_connect(struct tas_client *client, int sock)
{
	int err;
	tas_pl1rq_server_connect_st rq_server_connect = {
		.cmd = TAS_PL1_CMD_SERVER_CONNECT,
		.reserved = 0,
		.client_pid = getpid(),
		.wl = sizeof(tas_pl1rq_server_connect_st) / 4 - 1,
	};
	tas_pl1rsp_server_connect_st rsp_server_connect;

	client->sock = sock;
	snprintf(rq_server_connect.client_name, TAS_NAME_LEN32, "openocd");
#ifdef __WIN32__
	DWORD size = TAS_NAME_LEN16;
	GetUserNameA(rq_server_connect.user_name, &size);
#else
	getlogin_r(rq_server_connect.user_name, TAS_NAME_LEN16);
#endif
	rq_server_connect.client_pid = getpid();

	uint32_t packet_size = 4 + sizeof(tas_pl1rq_server_connect_st);
	char tx_buf[packet_size];
	memcpy(tx_buf, &packet_size, 4);
	memcpy(&tx_buf[4], &rq_server_connect, sizeof(rq_server_connect));

	err = send(sock, (const char *)tx_buf, packet_size, 0);
	if (err < 0)
		return ERROR_FAIL;

	err = recv(sock, (char *)&packet_size, 4, 0);
	if (err != 4 || packet_size != sizeof(tas_pl1rsp_server_connect_st) + 4)
		return ERROR_FAIL;
	err = recv(sock, (char *)&rsp_server_connect, sizeof(tas_pl1rsp_server_connect_st), 0);
	if (err < 0)
		return ERROR_FAIL;

	if (rsp_server_connect.cmd != TAS_PL1_CMD_SERVER_CONNECT || rsp_server_connect.err != TAS_PL_ERR_NO_ERROR)
		return ERROR_FAIL;

	return 0;
}

int tas_client_session_start(struct tas_client *client, const char *device)
{
	uint32_t packet_size = 4 + sizeof(tas_pl1rq_session_start_st);
	char tx_buf[packet_size];
	tas_pl1rq_session_start_st rq_session_start = {
		.cmd = TAS_PL1_CMD_SESSION_START,
		.con_id = 0,
		.client_type = TAS_CLIENT_TYPE_RW,
		.wl = sizeof(tas_pl1rq_session_start_st) / 4 - 1,
	};
	tas_pl1rsp_session_start_st rsp_session_start;

	strncpy(rq_session_start.identifier, device, TAS_NAME_LEN64);
	rq_session_start.identifier[TAS_NAME_LEN64 - 1] = '\0';
	snprintf(rq_session_start.session_name, TAS_NAME_LEN16, "openocd%u", getpid());
	rq_session_start.session_pw[0] = 0;

	memcpy(tx_buf, &packet_size, 4);
	memcpy(&tx_buf[4], &rq_session_start, sizeof(rq_session_start));

	if (send(client->sock, (const char *)tx_buf, packet_size, 0) < 0)
		return ERROR_FAIL;

	if (recv(client->sock, (char *)&packet_size, 4, 0) != 4)
		return ERROR_FAIL;
	if (recv(client->sock, (char *)&rsp_session_start, sizeof(tas_pl1rsp_session_start_st), 0) < 0)
		return ERROR_FAIL;

	if (rsp_session_start.cmd != TAS_PL1_CMD_SESSION_START || rsp_session_start.con_id != 0 ||
		rsp_session_start.err != TAS_PL_ERR_NO_ERROR)
		return ERROR_FAIL;

	if (rsp_session_start.num_instances > 0)
		return ERROR_FAIL;

	const uint32_t max_pl2rq_pkt_size = 2048;
	const uint32_t max_pl2rsp_pkt_size = 2048;
	const uint32_t pl0_max_num_rw = 128;

	client->max_pl2rq_pkt_size = MIN(max_pl2rq_pkt_size, rsp_session_start.con_info.max_pl2rq_pkt_size);
	client->max_pl2rsp_pkt_size = MIN(max_pl2rsp_pkt_size, rsp_session_start.con_info.max_pl2rsp_pkt_size);
	client->pl0_max_num_rw = MIN(pl0_max_num_rw, rsp_session_start.con_info.pl0_max_num_rw);

	return 0;
}

int tas_client_device_connect(struct tas_client *client, tas_dev_con_feat_et dev_con_feat)
{
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

	if (send(client->sock, (const char *)buf, packet_size, 0) < 0)
		return ERROR_FAIL;

	if (recv(client->sock, (char *)&packet_size, 4, 0) != 4)
		return ERROR_FAIL;
	if (recv(client->sock, (char *)&rsp_device_connect, sizeof(tas_pl1rsp_device_connect_st), 0) < 0)
		return ERROR_FAIL;

	if (rsp_device_connect.cmd != TAS_PL1_CMD_DEVICE_CONNECT || rsp_device_connect.err != TAS_PL_ERR_NO_ERROR)
		return ERROR_FAIL;

	if (rsp_device_connect.feat_used != dev_con_feat)
		return ERROR_FAIL;

	return 0;
}

int tas_client_get_targets(struct tas_client *client, tas_target_info_st **targets, size_t *target_num)
{
	tas_pl1rq_get_targets_st rq_get_targets;
	tas_pl1rsp_get_targets_st rsp_get_targets;
	uint32_t packet_size;
	if (!targets)
		return ERROR_FAIL;

	packet_size = 4 + sizeof(tas_pl1rq_get_targets_st);
	rq_get_targets.cmd = TAS_PL1_CMD_GET_TARGETS;
	rq_get_targets.wl = 0;
	rq_get_targets.start_index = 0;

	char buf[packet_size];
	memcpy(buf, &packet_size, 4);
	memcpy(&buf[4], &rq_get_targets, sizeof(rq_get_targets));

	if (send(client->sock, (const char *)buf, packet_size, 0) < 0)
		return ERROR_FAIL;

	if (recv(client->sock, (char *)&packet_size, 4, 0) != 4)
		return ERROR_FAIL;
	if (recv(client->sock, (char *)&rsp_get_targets, sizeof(tas_pl1rsp_get_targets_st), 0) < 0)
		return ERROR_FAIL;

	if (rsp_get_targets.cmd != TAS_PL1_CMD_GET_TARGETS || rsp_get_targets.err != TAS_PL_ERR_NO_ERROR)
		return ERROR_FAIL;

	*target_num = rsp_get_targets.num_target;
	/* Limit number of targets supported */
	if (*target_num > 32)
		return ERROR_FAIL;
	if (*target_num > 0) {
		*targets = calloc(*target_num, sizeof(tas_target_info_st));
		if (!*targets)
			return ERROR_FAIL;
		if (recv(client->sock, (char *)*targets, *target_num * sizeof(tas_target_info_st), 0) < 0)
			return ERROR_FAIL;
	}

	return ERROR_OK;
}

int tas_client_ping(struct tas_client *client, tas_pl1rsp_ping_st *rsp_ping)
{
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

	if (send(client->sock, (const char *)buf, packet_size, 0) < 0)
		return ERROR_FAIL;

	if (recv(client->sock, (char *)&packet_size, 4, 0) != 4)
		return ERROR_FAIL;
	if (recv(client->sock, (char *)rsp_ping, sizeof(tas_pl1rsp_ping_st), 0) < 0)
		return ERROR_FAIL;

	if (rsp_ping->cmd != TAS_PL1_CMD_PING || rsp_ping->err != TAS_PL_ERR_NO_ERROR)
		return ERROR_FAIL;

	return ERROR_OK;
}

enum {
	PROTOC_VER = 0 /*!< \brief TasPkt protocol version implemented in this class */
};

static uint16_t pl1_count;

struct tas_client_pl0_req {
	uint32_t addr;
	uint8_t *buffer;
	uint8_t cmd;
};

int tas_client_send_pl0(struct tas_client *client, uint8_t con_id, uint8_t *tx_buffer, uint32_t tx_len,
						uint8_t *rx_buffer, size_t *rx_len, tas_pl0rsp_st **pl0_resp, size_t pl0_elements)
{
	uint32_t packet_size;
	tas_pl1rq_pl0_start_st rq_start = {
		.cmd = TAS_PL1_CMD_PL0_START,
		.wl = 0,
		.con_id = con_id,
		.pl0_addr_map_mask = 1,
		.pl1_cnt = pl1_count++,
		.protoc_ver = PROTOC_VER,

	};
	tas_pl0rq_addr_map_st rq_addr_map = {.addr_map = 0, .cmd = TAS_PL0_CMD_ADDR_MAP, .wl = 0};
	tas_pl1rq_pl0_end_st rq_end = {.wl = 0, .cmd = TAS_PL1_CMD_PL0_END, .num_pl0_rw = pl0_elements + 1};
	tas_pl1rsp_pl0_start_st rsp_start;
	tas_pl1rsp_pl0_end_st rsp_end;

	memcpy(tx_buffer, &tx_len, 4);
	memcpy(tx_buffer + 4, &rq_start, sizeof(tas_pl1rq_pl0_start_st));
	memcpy(tx_buffer + 12, &rq_addr_map, sizeof(tas_pl0rq_addr_map_st));
	memcpy(tx_buffer + tx_len - sizeof(tas_pl1rq_pl0_end_st), &rq_end, sizeof(tas_pl1rq_pl0_end_st));

	if (send(client->sock, (const char *)tx_buffer, tx_len, 0) < 0)
		return ERROR_FAIL;

	if (recv(client->sock, (char *)rx_buffer, 4, 0) != 4)
		return ERROR_FAIL;
	memcpy(&packet_size, rx_buffer, 4);
	if (packet_size > *rx_len)
		return ERROR_FAIL;
	*rx_len = packet_size;

	if (recv(client->sock, (char *)(rx_buffer + 4), packet_size - 4, 0) < 0)
		return ERROR_FAIL;
	memcpy(&rsp_start, rx_buffer + 4, sizeof(tas_pl1rsp_pl0_start_st));
	memcpy(&rsp_end, rx_buffer + packet_size - sizeof(tas_pl1rsp_pl0_start_st), sizeof(tas_pl1rsp_pl0_end_st));

	if (rsp_start.err != TAS_PL_ERR_NO_ERROR || rsp_end.pl1_cnt != rq_start.pl1_cnt)
		return ERROR_FAIL;

	size_t i = 0;
	size_t offset = 4 + sizeof(tas_pl1rsp_pl0_start_st);
	for (i = 0; i < rq_end.num_pl0_rw; i++) {
		pl0_resp[i] = (tas_pl0rsp_st *)(rx_buffer + offset);
		offset += ((pl0_resp[i]->wl + 1) * 4);
	}

	return ERROR_OK;
}

int tas_client_execute_mem_req(struct tas_client *client, uint8_t addr_map, struct tas_client_mem_req *mem_req)
{
	int err;
	size_t tx_offset = 0, rx_offset = 0;
	size_t rx_size = sizeof(tas_pl1rsp_pl0_start_st) + sizeof(tas_pl1rsp_pl0_end_st) + 4;
	char tx_buf[client->max_pl2rq_pkt_size];
	char rx_buf[client->max_pl2rsp_pkt_size];
	uint32_t packet_size;

	if (mem_req->length != 1 && mem_req->length != 2 && mem_req->length != 4 && mem_req->length != 8 &&
		mem_req->length % 4 != 0 && mem_req->length > 1024)
		return ERROR_FAIL;

	tx_offset += 4; /* for packet size */
	tas_pl1rq_pl0_start_st rq_start = {
		.cmd = TAS_PL1_CMD_PL0_START,
		.wl = 1,
		.con_id = 0,
		.pl0_addr_map_mask = (1 << addr_map),
		.pl1_cnt = pl1_count++,
		.protoc_ver = PROTOC_VER,
	};
	memcpy(tx_buf + tx_offset, &rq_start, sizeof(rq_start));
	tx_offset += sizeof(rq_start);

	tas_pl0rq_addr_map_st rq_addr_map = {.addr_map = addr_map, .cmd = TAS_PL0_CMD_ADDR_MAP, .wl = 0};
	memcpy(tx_buf + tx_offset, &rq_addr_map, sizeof(rq_addr_map));
	tx_offset += sizeof(rq_addr_map);

	tas_pl0rq_base_addr32_st rq_base_addr = {
		.cmd = TAS_PL0_CMD_BASE_ADDR32,
		.wl = 0,
		.ba31to16 = (mem_req->addr >> 16) & 0xFFFF,
	};
	memcpy(tx_buf + tx_offset, &rq_base_addr, sizeof(rq_base_addr));
	tx_offset += sizeof(rq_base_addr);

	tas_pl0rq_rdblk_st pl0rq_read;
	tas_pl0rq_wrblk_st pl0rq_write;

	tas_pl1rsp_pl0_start_st rsp_start;
	tas_pl0rsp_rd_st rsp_rd;
	uint32_t words = (mem_req->length + 3) / 4;

	if (mem_req->is_read) {
		pl0rq_read = (tas_pl0rq_rdblk_st){
			.cmd = (mem_req->length == 1)	   ? TAS_PL0_CMD_RD8
				   : (mem_req->length == 2)	   ? TAS_PL0_CMD_RD16
				   : (mem_req->length == 4)	   ? TAS_PL0_CMD_RD32
				   : (mem_req->length == 8)	   ? TAS_PL0_CMD_RDBLK
				   : (mem_req->length == 1024) ? TAS_PL0_CMD_RDBLK1KB
											   : TAS_PL0_CMD_RDBLK,
			.wl = (mem_req->length > 8) ? 1 : 0,
			.a15to0 = mem_req->addr & 0xFFFF,
			.wlrd = words == 256 ? 0 : words,
		};
		if (mem_req->length > 8) {
			memcpy(tx_buf + tx_offset, &pl0rq_read, sizeof(tas_pl0rq_rdblk_st));
			tx_offset += sizeof(tas_pl0rq_rdblk_st);
		} else {
			memcpy(tx_buf + tx_offset, &pl0rq_read, sizeof(tas_pl0rq_rd_st));
			tx_offset += sizeof(tas_pl0rq_rd_st);
		}
		rx_size += sizeof(tas_pl0rsp_rd_st) + words * 4;
	} else {
		pl0rq_write = (tas_pl0rq_wrblk_st){
			.cmd = (mem_req->length == 1)	? TAS_PL0_CMD_WR8
				   : (mem_req->length == 2) ? TAS_PL0_CMD_WR16
				   : (mem_req->length == 4) ? TAS_PL0_CMD_WR32
				   : (mem_req->length == 8) ? TAS_PL0_CMD_WR64
											: TAS_PL0_CMD_WRBLK,
			.wl = (mem_req->length == 1024) ? 0 : ((mem_req->length + 3) / 4),
			.a15to0 = mem_req->addr & 0xFFFF,
		};
		memcpy(tx_buf + tx_offset, &pl0rq_write, sizeof(tas_pl0rq_wrblk_st));
		tx_offset += sizeof(tas_pl0rq_wrblk_st);
		memcpy(tx_buf + tx_offset, mem_req->buffer, mem_req->length);
		tx_offset += mem_req->length;
		rx_size += sizeof(tas_pl0rsp_st);
	}
	tas_pl1rq_pl0_end_st rq_end = {
		.wl = 0,
		.cmd = TAS_PL1_CMD_PL0_END,
		.num_pl0_rw = 1,
	};
	memcpy(tx_buf + tx_offset, &rq_end, sizeof(rq_end));
	tx_offset += sizeof(rq_end);

	packet_size = tx_offset;
	memcpy(tx_buf, &packet_size, 4);

	err = send(client->sock, (const char *)tx_buf, tx_offset, 0);
	if (err < 0)
		return ERROR_FAIL;

	err = recv(client->sock, (char *)&packet_size, 4, 0);
	if (err != 4)
		return ERROR_FAIL;
	if (packet_size > client->max_pl2rsp_pkt_size) {
		LOG_DEBUG("Response packet size %u exceeds maximum PL2 response packet size %u", packet_size,
				  client->max_pl2rsp_pkt_size);
		return ERROR_FAIL;
	}
	err = recv(client->sock, (char *)rx_buf, packet_size - 4, 0);
	if (err != (int)packet_size - 4)
		return ERROR_FAIL;
	if (packet_size > rx_size) {
		LOG_DEBUG("Response packet size %u exceeds expected size %zu", packet_size, rx_size);
		return ERROR_FAIL;
	}

	memcpy(&rsp_start, rx_buf, sizeof(rsp_start));
	rx_offset += sizeof(rsp_start);
	if (rsp_start.err != TAS_PL_ERR_NO_ERROR)
		return ERROR_FAIL;

	if (mem_req->is_read) {
		memcpy(&rsp_rd, rx_buf + rx_offset, sizeof(rsp_rd));
		rx_offset += sizeof(rsp_rd);
		if (rsp_rd.cmd != pl0rq_read.cmd)
			return ERROR_FAIL;
		memcpy(mem_req->buffer, rx_buf + rx_offset, mem_req->length);
	}

	return ERROR_OK;
}

int tas_client_execute_mem_reqs(struct tas_client *client, uint8_t addr_map, struct tas_client_mem_req *mem_reqs,
								size_t mem_req_num)
{
	int err;
	char tx_buffer[client->max_pl2rq_pkt_size];
	char rx_buffer[client->max_pl2rsp_pkt_size];
	size_t send_mem_req = 0;
	size_t recv_mem_req = 0;
	struct tas_outstanding_pkt outstanding[TAS_OUTSTANDING_MAX] = {0};
	size_t outstanding_num = 0;
	uint8_t con_id = 0;

	while (recv_mem_req < mem_req_num) {
		while (send_mem_req < mem_req_num && outstanding_num < TAS_OUTSTANDING_MAX) {
			size_t tx_offset = 4;
			size_t rx_size = 4 + sizeof(tas_pl1rsp_pl0_start_st) + sizeof(tas_pl1rsp_pl0_end_st);
			size_t slot = 0;
			size_t i;
			uint16_t pl1_cnt = pl1_count++;

			for (i = 0; i < TAS_OUTSTANDING_MAX; i++) {
				if (!outstanding[i].in_use) {
					slot = i;
					break;
				}
			}
			if (i == TAS_OUTSTANDING_MAX) {
				alive_sleep(1);
				break;
			}

			tas_pl1rq_pl0_start_st rq_start = {
				.cmd = TAS_PL1_CMD_PL0_START,
				.wl = 1,
				.con_id = con_id,
				.pl0_addr_map_mask = (1 << addr_map),
				.pl1_cnt = pl1_cnt,
				.protoc_ver = PROTOC_VER,
			};
			memcpy(tx_buffer + tx_offset, &rq_start, sizeof(rq_start));
			tx_offset += sizeof(rq_start);

			tas_pl0rq_addr_map_st rq_addr_map = {.addr_map = addr_map, .cmd = TAS_PL0_CMD_ADDR_MAP, .wl = 0};
			memcpy(tx_buffer + tx_offset, &rq_addr_map, sizeof(rq_addr_map));
			tx_offset += sizeof(rq_addr_map);

			uint32_t base_addr = 0;
			tas_pl1rq_pl0_end_st rq_end = {
				.wl = 0,
				.cmd = TAS_PL1_CMD_PL0_END,
				.num_pl0_rw = 0,
			};

			for (i = send_mem_req; i < mem_req_num && rq_end.num_pl0_rw < client->pl0_max_num_rw;
				 i++, rq_end.num_pl0_rw++) {
				if (mem_reqs[i].length != 1 && mem_reqs[i].length != 2 && mem_reqs[i].length != 4 &&
					mem_reqs[i].length != 8 && mem_reqs[i].length % 4 != 0 && mem_reqs[i].length > 1024)
					return ERROR_FAIL;
				uint32_t words = (mem_reqs[i].length + 3) / 4;
				if (tx_offset + sizeof(tas_pl0rq_base_addr32_st) +
							(mem_reqs[i].is_read ? sizeof(tas_pl0rq_rd_st) : sizeof(tas_pl0rq_wrblk_st) + words * 4) >
						client->max_pl2rq_pkt_size ||
					rx_size + (mem_reqs[i].is_read ? sizeof(tas_pl0rsp_rd_st) + words * 4 : sizeof(tas_pl0rsp_st)) >
						client->max_pl2rsp_pkt_size)
					break;

				if (base_addr != (mem_reqs[i].addr & 0xFFFF0000)) {
					tas_pl0rq_base_addr32_st rq_base_addr = {
						.cmd = TAS_PL0_CMD_BASE_ADDR32,
						.wl = 0,
						.ba31to16 = (mem_reqs[i].addr >> 16) & 0xFFFF,
					};
					memcpy(tx_buffer + tx_offset, &rq_base_addr, sizeof(rq_base_addr));
					tx_offset += sizeof(rq_base_addr);
					base_addr = mem_reqs[i].addr & 0xFFFF0000;
				}

				if (mem_reqs[i].is_read) {
					tas_pl0rq_rdblk_st pl0rq_read = {
						.cmd = (mem_reqs[i].length == 1)   ? TAS_PL0_CMD_RD8
							   : (mem_reqs[i].length == 2) ? TAS_PL0_CMD_RD16
							   : (mem_reqs[i].length == 4) ? TAS_PL0_CMD_RD32
							   : (mem_reqs[i].length == 8) ? TAS_PL0_CMD_RD64
														   : TAS_PL0_CMD_RDBLK,
						.wl = (mem_reqs[i].length > 8) ? 1 : 0,
						.a15to0 = mem_reqs[i].addr & 0xFFFF,
						.wlrd = (mem_reqs[i].length == 1024) ? 0 : words,
					};
					if (mem_reqs[i].length > 8) {
						memcpy(tx_buffer + tx_offset, &pl0rq_read, sizeof(tas_pl0rq_rdblk_st));
						tx_offset += sizeof(tas_pl0rq_rdblk_st);
					} else {
						memcpy(tx_buffer + tx_offset, &pl0rq_read, sizeof(tas_pl0rq_rd_st));
						tx_offset += sizeof(tas_pl0rq_rd_st);
					}
					rx_size += sizeof(tas_pl0rsp_rd_st) + words * 4;
				} else {
					tas_pl0rq_wrblk_st pl0rq_write = {
						.cmd = (mem_reqs[i].length == 1)   ? TAS_PL0_CMD_WR8
							   : (mem_reqs[i].length == 2) ? TAS_PL0_CMD_WR16
							   : (mem_reqs[i].length == 4) ? TAS_PL0_CMD_WR32
							   : (mem_reqs[i].length == 8) ? TAS_PL0_CMD_WR64
														   : TAS_PL0_CMD_WRBLK,
						.wl = (mem_reqs[i].length == 1024) ? 0 : words,
						.a15to0 = mem_reqs[i].addr & 0xFFFF,
					};
					memcpy(tx_buffer + tx_offset, &pl0rq_write, sizeof(pl0rq_write));
					tx_offset += sizeof(pl0rq_write);
					memcpy(tx_buffer + tx_offset, mem_reqs[i].buffer, mem_reqs[i].length);
					tx_offset += words * 4;
					rx_size += sizeof(tas_pl0rsp_st);
				}
			}

			if (rq_end.num_pl0_rw == 0)
				return ERROR_FAIL;

			memcpy(tx_buffer + tx_offset, &rq_end, sizeof(rq_end));
			tx_offset += sizeof(rq_end);
			memcpy(tx_buffer, &tx_offset, 4);

			err = send(client->sock, (const char *)tx_buffer, tx_offset, 0);
			if (err < 0)
				return ERROR_FAIL;

			outstanding[slot].in_use = true;
			outstanding[slot].pl1_cnt = pl1_cnt;
			outstanding[slot].start_mem_req = send_mem_req;
			outstanding[slot].end_mem_req = i;
			outstanding[slot].expected_rx_size = rx_size;
			outstanding_num++;
			send_mem_req = i;
			con_id++;
		}

		if (outstanding_num == 0)
			return ERROR_FAIL;

		uint32_t recv_len;
		err = recv(client->sock, (char *)&recv_len, 4, 0);
		if (err != 4)
			return ERROR_FAIL;
		if (recv_len > client->max_pl2rsp_pkt_size) {
			LOG_DEBUG("Response size too long %d", recv_len);
			return ERROR_FAIL;
		}
		err = recv(client->sock, (char *)rx_buffer, recv_len - 4, 0);
		if (err != (int)recv_len - 4)
			return ERROR_FAIL;

		size_t rx_offset = 0;
		tas_pl1rsp_pl0_start_st rsp_start;
		tas_pl1rsp_pl0_end_st rsp_end;
		memcpy(&rsp_start, rx_buffer + rx_offset, sizeof(rsp_start));
		rx_offset += sizeof(rsp_start);
		memcpy(&rsp_end, rx_buffer + recv_len - 4 - sizeof(rsp_end), sizeof(rsp_end));

		size_t slot = TAS_OUTSTANDING_MAX;
		for (size_t i = 0; i < TAS_OUTSTANDING_MAX; i++) {
			if (!outstanding[i].in_use)
				continue;
			if (outstanding[i].pl1_cnt == rsp_end.pl1_cnt) {
				slot = i;
				break;
			}
		}
		if (slot == TAS_OUTSTANDING_MAX)
			return ERROR_FAIL;
		if (recv_len > outstanding[slot].expected_rx_size) {
			LOG_DEBUG("Unxpected response size %zu != %d", outstanding[slot].expected_rx_size, recv_len);
			return ERROR_FAIL;
		}
		if (rsp_start.err != TAS_PL_ERR_NO_ERROR)
			return ERROR_FAIL;
		if (rsp_end.pl1_cnt != outstanding[slot].pl1_cnt)
			return ERROR_FAIL;

		for (size_t i = outstanding[slot].start_mem_req; i < outstanding[slot].end_mem_req; i++) {
			tas_pl0rsp_st rsp_pl0;
			memcpy(&rsp_pl0, rx_buffer + rx_offset, sizeof(rsp_pl0));
			rx_offset += sizeof(rsp_pl0);
			if (rsp_pl0.err != TAS_PL0_ERR_NO_ERROR)
				return ERROR_FAIL;
			if (mem_reqs[i].is_read) {
				memcpy(mem_reqs[i].buffer, rx_buffer + rx_offset, mem_reqs[i].length);
				rx_offset += mem_reqs[i].length;
			}
		}

		outstanding[slot].in_use = false;
		outstanding_num--;
		recv_mem_req += outstanding[slot].end_mem_req - outstanding[slot].start_mem_req;
	}

	return ERROR_OK;
}
