// SPDX-License-Identifier: GPL-2.0-or-later

/***************************************************************************
 *   TAS Client Adapter Interface for Infineon AURIX                       *
 *   Copyright (C) 2026 Infineon Technologies AG                           *
 ***************************************************************************/

#include <stdbool.h>
#include <string.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include "helper/command.h"
#include "helper/log.h"
#include "jtag/adapter.h"
#include "jtag/ifxdap.h"
#include "jtag/interface.h"
#include "jtag/jtag.h"
#include "server/server.h"
#include "target/aurix/ocmts.h"
#include "transport/transport.h"

#include "tas_am15_am14.h"
#include "tas_pkt.h"
#include "tas_protocol.h"

static struct tas_client tas_client = {.max_pl2rq_pkt_size = 1024, .max_pl2rsp_pkt_size = 1024, .pl0_max_num_rw = 128};
static const char *server_host = "localhost";
static const char *server_port = "24817";
static uint32_t current_address;
static struct tas_client_mem_req mem_reqs[256];
static size_t mem_req_num;
static tas_target_info_st target;
static bool harr;
static bool porst;

static int tas_client_init(void)
{
	int err = ERROR_OK, sock = -1;
	struct addrinfo hints;
	struct addrinfo *result, *rp;
	const char *serial = adapter_get_required_serial();
	tas_target_info_st *targets;
	size_t target_num;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	hints.ai_protocol = IPPROTO_TCP;

	LOG_INFO("Connecting to TAS server %s:%s", server_host, server_port);

	err = getaddrinfo(server_host, server_port, &hints, &result);
	if (err != 0) {
		LOG_ERROR("Failed to get address for host %s", server_host);
		return ERROR_FAIL;
	}

	for (rp = result; rp; rp = rp->ai_next) {
		sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (sock == -1)
			continue;

		err = connect(sock, rp->ai_addr, rp->ai_addrlen);
		if (err == 0)
			break;
	}

	freeaddrinfo(result);

	if (err != 0) {
		LOG_ERROR("Failed to connect to %s:%s", server_host, server_port);
		return ERROR_CONNECTION_REJECTED;
	}

	if (tas_client_connect(&tas_client, sock) != 0) {
		LOG_ERROR("Failed to connect to TAS server");
		return ERROR_CONNECTION_REJECTED;
	}

	if (tas_client_get_targets(&tas_client, &targets, &target_num) != 0) {
		LOG_ERROR("Failed to receive targets");
		return ERROR_FAIL;
	}

	if (target_num == 0) {
		LOG_ERROR("No targets to connect");
		return ERROR_FAIL;
	}

	if (!serial) {
		target = targets[0];
	} else {
		bool found = false;
		for (size_t i = 0; i < target_num; i++) {
			if (strcmp(serial, targets[i].identifier) == 0) {
				found = true;
				target = targets[i];
				break;
			}
		}
		if (!found) {
			LOG_ERROR("No target with serial %s found", serial);
			free(targets);
			return ERROR_FAIL;
		}
	}
	free(targets);

	LOG_INFO("Starting session for target %s", target.identifier);

	err = tas_client_session_start(&tas_client, target.identifier);
	if (err) {
		LOG_ERROR("Failed to start session for target %s", target.identifier);
		return ERROR_FAIL;
	}

	return ERROR_OK;
}

static int tas_client_quit(void)
{
	close(tas_client.sock);
	return 0;
}

static int tas_client_reset(int trst, int srst)
{
	int err;
	if (trst != 0)
		return ERROR_NOT_IMPLEMENTED;

	if (srst && !porst) {
		struct tas_client_mem_req ctrl_mem_req = {
			.addr = TAS_AM15_RW_USERPINS_CNTRL,
			.buffer = (uint32_t[]){TAS_UPC_ADD_SFP_RESET},
			.length = 4,
			.is_read = false,
		};
		err = tas_client_execute_mem_req(&tas_client, 15, &ctrl_mem_req);
		if (err) {
			LOG_ERROR("Failed to set user pins for reset");
			return ERROR_FAIL;
		}

		struct tas_client_mem_req upc_mem_req = {
			.addr = TAS_AM15_RW_USERPINS,
			.buffer = (uint32_t[]){0, TAS_UP_SFP_RESET},
			.length = 8,
			.is_read = false,
		};
		err = tas_client_execute_mem_req(&tas_client, 15, &upc_mem_req);
		if (err) {
			LOG_ERROR("Failed to set user pins for reset");
			return ERROR_FAIL;
		}
		porst = true;
	} else {
		if (porst) {
			if (harr)
				err = tas_client_device_connect(&tas_client, TAS_DEV_CON_FEAT_RESET_AND_HALT);
			else
				err = tas_client_device_connect(&tas_client, TAS_DEV_CON_FEAT_RESET);

			harr = false;
			porst = false;
			return err;
		}
	}

	return 0;
}

static int tas_client_op_run(struct ocmts *ocds)
{
	int err = ERROR_OK;
	if (mem_req_num > 0)
		err = tas_client_execute_mem_reqs(&tas_client, 0, mem_reqs, mem_req_num);
	mem_req_num = 0;
	if (err)
		return ERROR_FAIL;

	return ERROR_OK;
}

static int tas_client_op_connect(struct ocmts *ocmts)
{
	uint32_t tap;
	bool found = false;

	for (tap = 0; tap < ocmts->tap->expected_ids_cnt; tap++) {
		if (target.device_type == ocmts->tap->expected_ids[tap]) {
			found = true;
			break;
		}
	}

	if (!found) {
		LOG_ERROR("No matching target for OCDS %s found", ocmts->name);
		return ERROR_COMMAND_ARGUMENT_INVALID;
	}
	ocmts->tap->idcode = target.device_type;
	ocmts->tap->has_idcode = true;

	int err;
	enum reset_types jtag_reset_config = jtag_get_reset_config();

	if (jtag_reset_config & RESET_CNCT_UNDER_SRST)
		err = tas_client_device_connect(&tas_client, TAS_DEV_CON_FEAT_RESET_AND_HALT);
	else
		err = tas_client_device_connect(&tas_client, TAS_DEV_CON_FEAT_NONE);

	if (err) {
		LOG_ERROR("Failed to connect to device %s", target.identifier);
		return ERROR_FAIL;
	}

	return ERROR_OK;
}

static int tas_client_set_address(struct ocmts *ocds, uint32_t addr)
{
	current_address = addr;
	return ERROR_OK;
}

static int tas_client_read_byte(struct ocmts *ocds, uint8_t *data)
{
	if (mem_req_num >= 256) {
		int ret = tas_client_execute_mem_reqs(&tas_client, 0, mem_reqs, mem_req_num);
		if (ret)
			return ERROR_FAIL;
		mem_req_num = 0;
	}
	mem_reqs[mem_req_num].addr = current_address;
	mem_reqs[mem_req_num].buffer = (void *)data;
	mem_reqs[mem_req_num].length = 1;
	mem_reqs[mem_req_num].is_read = true;
	mem_req_num++;

	return ERROR_OK;
}

static int tas_client_read_hword(struct ocmts *ocds, uint16_t *data)
{
	if (current_address & 0x1) {
		LOG_ERROR("Half-Word write address must be 2-byte aligned");
		return ERROR_FAIL;
	}
	if (mem_req_num >= 256) {
		int ret = tas_client_execute_mem_reqs(&tas_client, 0, mem_reqs, mem_req_num);
		if (ret)
			return ERROR_FAIL;
		mem_req_num = 0;
	}
	mem_reqs[mem_req_num].addr = current_address;
	mem_reqs[mem_req_num].buffer = (void *)data;
	mem_reqs[mem_req_num].length = 2;
	mem_reqs[mem_req_num].is_read = true;
	mem_req_num++;

	return ERROR_OK;
}

static int tas_client_read_word(struct ocmts *ocds, uint32_t *data)
{
	if (current_address & 0x3) {
		LOG_ERROR("Word write address must be 4-byte aligned");
		return ERROR_FAIL;
	}
	if (mem_req_num >= 256) {
		int ret = tas_client_execute_mem_reqs(&tas_client, 0, mem_reqs, mem_req_num);
		if (ret)
			return ERROR_FAIL;
		mem_req_num = 0;
	}
	mem_reqs[mem_req_num].addr = current_address;
	mem_reqs[mem_req_num].buffer = (void *)data;
	mem_reqs[mem_req_num].length = 4;
	mem_reqs[mem_req_num].is_read = true;
	mem_req_num++;

	return ERROR_OK;
}

static int tas_client_read_block(struct ocmts *ocds, void *data, size_t length)
{
	if (length > 256) {
		LOG_ERROR("Block read length must up to 256 words");
		return ERROR_FAIL;
	}
	if (current_address & 0x3) {
		LOG_ERROR("Block read address must be 4-byte aligned");
		return ERROR_FAIL;
	}
	if (mem_req_num >= 256) {
		int ret = tas_client_execute_mem_reqs(&tas_client, 0, mem_reqs, mem_req_num);
		if (ret)
			return ERROR_FAIL;
		mem_req_num = 0;
	}
	mem_reqs[mem_req_num].addr = current_address;
	mem_reqs[mem_req_num].buffer = data;
	mem_reqs[mem_req_num].length = length * 4;
	mem_reqs[mem_req_num].is_read = true;
	mem_req_num++;

	return ERROR_OK;
}

static int tas_client_write_byte(struct ocmts *ocds, const void *data)
{
	if (mem_req_num >= 256) {
		int ret = tas_client_execute_mem_reqs(&tas_client, 0, mem_reqs, mem_req_num);
		if (ret)
			return ERROR_FAIL;
		mem_req_num = 0;
	}
	mem_reqs[mem_req_num].addr = current_address;
	mem_reqs[mem_req_num].buffer = (void *)data;
	mem_reqs[mem_req_num].length = 1;
	mem_reqs[mem_req_num].is_read = false;
	mem_req_num++;

	return ERROR_OK;
}

static int tas_client_write_hword(struct ocmts *ocds, const void *data)
{
	if (current_address & 0x1) {
		LOG_ERROR("Half-Word write address must be 2-byte aligned");
		return ERROR_FAIL;
	}
	if (mem_req_num >= 256) {
		int ret = tas_client_execute_mem_reqs(&tas_client, 0, mem_reqs, mem_req_num);
		if (ret)
			return ERROR_FAIL;
		mem_req_num = 0;
	}
	mem_reqs[mem_req_num].addr = current_address;
	mem_reqs[mem_req_num].buffer = (void *)data;
	mem_reqs[mem_req_num].length = 2;
	mem_reqs[mem_req_num].is_read = false;
	mem_req_num++;

	return ERROR_OK;
}

static int tas_client_write_word(struct ocmts *ocds, const void *data)
{
	if (current_address & 0x3) {
		LOG_ERROR("Word write address must be 4-byte aligned");
		return ERROR_FAIL;
	}
	if (mem_req_num >= 256) {
		int ret = tas_client_execute_mem_reqs(&tas_client, 0, mem_reqs, mem_req_num);
		if (ret)
			return ERROR_FAIL;
		mem_req_num = 0;
	}
	mem_reqs[mem_req_num].addr = current_address;
	mem_reqs[mem_req_num].buffer = (void *)data;
	mem_reqs[mem_req_num].length = 4;
	mem_reqs[mem_req_num].is_read = false;
	mem_req_num++;

	return ERROR_OK;
}

static int tas_client_write_block(struct ocmts *ocds, const void *data, size_t length)
{
	if (length > 256) {
		LOG_ERROR("Block write length must be up to 256 words");
		return ERROR_FAIL;
	}
	if (current_address & 0x3) {
		LOG_ERROR("Block write address must be 4-byte aligned");
		return ERROR_FAIL;
	}
	if (mem_req_num >= 256) {
		int ret = tas_client_execute_mem_reqs(&tas_client, 0, mem_reqs, mem_req_num);
		if (ret)
			return ERROR_FAIL;
		mem_req_num = 0;
	}
	mem_reqs[mem_req_num].addr = current_address;
	mem_reqs[mem_req_num].buffer = (void *)data;
	mem_reqs[mem_req_num].length = length * 4;
	mem_reqs[mem_req_num].is_read = false;
	mem_req_num++;

	return ERROR_OK;
}

static int tas_client_set_ojconf(struct ocmts *ocds, uint16_t ojconf)
{
	if (ojconf & 0x1)
		harr = (ojconf & 0x2) != 0;

	return ERROR_OK;
}

static const struct ocmts_ops ocmts_ops_interface = {
	.connect = tas_client_op_connect,
	.queue_io_set_address = tas_client_set_address,
	.queue_io_read_byte = tas_client_read_byte,
	.queue_io_read_hword = tas_client_read_hword,
	.queue_io_read_word = tas_client_read_word,
	.queue_io_read_block = tas_client_read_block,
	.queue_io_write_byte = tas_client_write_byte,
	.queue_io_write_hword = tas_client_write_hword,
	.queue_io_write_word = tas_client_write_word,
	.queue_io_write_block = tas_client_write_block,
	.run = tas_client_op_run,
	.queue_io_set_ojconf = tas_client_set_ojconf,
};

static int tas_client_speed(int speed)
{
	struct tas_client_mem_req mem_req = {
		.addr = TAS_AM15_RW_ACC_HW_FREQUENCY,
		.buffer = (uint32_t[]){speed * 1000},
		.length = 4,
		.is_read = false,
	};
	int err = tas_client_execute_mem_req(&tas_client, 15, &mem_req);
	if (err) {
		LOG_ERROR("Failed to set adapter speed.");
		return ERROR_FAIL;
	}

	return ERROR_OK;
}

static int tas_client_speed_div(int speed, int *khz)
{
	*khz = speed;
	return ERROR_OK;
}

static int tas_client_khz(int khz, int *speed)
{
	*speed = khz;
	return ERROR_OK;
}

static int tas_client_dap_init(void)
{
	return ERROR_OK;
}

static struct ifxdap_driver ifxdap_ops = {
	.init = tas_client_dap_init,
};

COMMAND_HANDLER(tas_client_cmd_host)
{
	if (CMD_ARGC < 1 || CMD_ARGC > 2)
		return ERROR_COMMAND_SYNTAX_ERROR;

	server_host = strdup(CMD_ARGV[0]);
	if (CMD_ARGC == 2)
		server_port = strdup(CMD_ARGV[1]);

	return ERROR_OK;
}

static const struct command_registration tas_client_subcommand_handlers[] = {{
																				 .name = "host",
																				 .handler = tas_client_cmd_host,
																				 .mode = COMMAND_CONFIG,
																				 .usage = "host [port]",
																				 .help = "set tas client host and port",
																			 },
																			 COMMAND_REGISTRATION_DONE};

static const struct command_registration tas_client_command_handlers[] = {{
																			  .name = "tas-client",
																			  .mode = COMMAND_ANY,
																			  .help = "perform tas client commands",
																			  .usage = "<cmd>",
																			  .chain = tas_client_subcommand_handlers,
																		  },
																		  COMMAND_REGISTRATION_DONE};

struct adapter_driver tas_client_adapter_driver = {
	.name = "tas_client",
	.commands = tas_client_command_handlers,
	.transport_ids = TRANSPORT_JTAG | TRANSPORT_IFXDAP,
	.transport_preferred_id = TRANSPORT_IFXDAP,
	.ocmts_ops = &ocmts_ops_interface,
	.ifxdap_ops = &ifxdap_ops,
	.init = tas_client_init,
	.quit = tas_client_quit,
	.reset = tas_client_reset,
	/* .srst_asserted = tas_client_srst_asserted, */
	.speed = tas_client_speed,
	.speed_div = tas_client_speed_div,
	.khz = tas_client_khz,
};
