// SPDX-License-Identifier: GPL-2.0-or-later

/***************************************************************************
 *   AURIX TC4x RRAM Driver for Infineon AURIX                              *
 *   Copyright (C) 2026 Infineon Technologies AG                            *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "flash/common.h"
#include "flash/nor/core.h"
#include "flash/nor/driver.h"
#include "helper/binarybuffer.h"
#include "helper/log.h"
#include "helper/time_support.h"
#include "target/algorithm.h"
#include "target/aurix/ocmts.h"
#include "target/aurix/tricore.h"
#include "target/target.h"

#define UCB_CHIPID 0xAE607808
#define UCB_CHIPID_PROD 0x3C000000

#define MODULE_DMUR0_BASE 0xF8080000u
#define MODULE_DMUR1_BASE 0xF80C0000u
#define MODULE_PMURCS_BASE 0xF8590000u

#define MODULE_PMUR0A_BASE 0xF8410000u
#define MODULE_PMUR0B_BASE 0xF8420000u
#define MODULE_PMUR1A_BASE 0xF8450000u
#define MODULE_PMUR1B_BASE 0xF8460000u
#define MODULE_PMUR2A_BASE 0xF8490000u
#define MODULE_PMUR2B_BASE 0xF84A0000u
#define MODULE_PMUR3A_BASE 0xF84D0000u
#define MODULE_PMUR3B_BASE 0xF84E0000u
#define MODULE_PMUR4A_BASE 0xF8510000u
#define MODULE_PMUR4B_BASE 0xF8520000u

#define CPU_CFI0_BASE   0xF8400000u
#define CPU_CFI1_BASE   0xF8440000u
#define CPU_CFI2_BASE   0xF8480000u
#define CPU_CFI3_BASE   0xF84C0000u
#define CPU_CFI4_BASE   0xF8500000u
#define CPU_CFICS_BASE  0xF8580000u

#define CPU_CFI_PFIBUFDIS_OFFSET 0x320u

#define UR_NVMADDR_OFFSET 0x3Cu
#define UR_WDATA0_OFFSET  0x40u
#define UR_REQUEST_OFFSET 0x74u
#define UR_REQSTAT_OFFSET 0x78u
#define UR_CLRSTAT_OFFSET 0x7Cu

#define UR_REQUEST_INITWR (1u << 0)
#define UR_REQUEST_STWR   (1u << 1)
#define UR_REQUEST_MAR_RD (1u << 2)

#define UR_REQSTAT_BUSY   (1u << 0)
#define UR_REQSTAT_DONE   (1u << 1)

#define UR_REQSTAT_ERROR_MASK 0x0018FFFCu
#define UR_CLRSTAT_CLEAR_ALL ((1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7) | (1u << 9) | (1u << 10))

#define RRAM_ALGO_ERROR_BUSY (1u << 31)
#define RRAM_ALGO_ERROR_TIMEOUT (1u << 30)
#define RRAM_ALGO_ERROR_PAGE (1u << 29)

enum tc4x_rram_type {
	TC4X_RRAM_PRRAM,
	TC4X_RRAM_DNVM,
	TC4X_RRAM_UCB,
};

enum tc4x_rram_device {
	TC4X_RRAM_TC48X,
	TC4X_RRAM_TC49X,
	TC4X_RRAM_TC4DX,
};

struct tc4x_rram_bank {
	bool probed;
	bool fallback_mode;
	bool ucb_unlocked;
	enum tc4x_rram_type type;
	uint32_t page_size;
	uint32_t sector_size;
	uint32_t write_timeout_ms;
	uint32_t margin_timeout_ms;
	enum tc4x_rram_device device;
};

struct tc4x_rram_bank_info {
	target_addr_t base;
	uint32_t size;
	enum tc4x_rram_type type;
	uint32_t page_size;
	uint32_t sector_size;
};

static const uint8_t tc4x_rram_pnvm_write_code[] = {
#include "../../../contrib/loaders/flash/aurix/tc4x-rram-pnvm-program.inc"
};

static const uint8_t tc4x_rram_dnvm_write_code[] = {
#include "../../../contrib/loaders/flash/aurix/tc4x-rram-dnvm-program.inc"
};

static inline uint32_t tc4x_rram_to_nvm_segment(uint32_t address)
{
	return (address & 0x0FFFFFFFu) | 0xA0000000u;
}

static inline bool tc4x_rram_is_dnvm_addr(uint32_t nvm_addr)
{
	return (nvm_addr & 0xFF000000u) == 0xAE000000u;
}

static target_addr_t tc4x_rram_get_pmur_base(uint32_t nvm_addr)
{
	uint32_t index;

	if (nvm_addr < 0xA0000000u || nvm_addr > 0xAFFFFFFFu)
		return MODULE_PMURCS_BASE;

	index = (nvm_addr - 0xA0000000u) >> 21;

	switch (index) {
	case 0:
		return MODULE_PMUR0A_BASE;
	case 1:
		return MODULE_PMUR0B_BASE;
	case 2:
		return MODULE_PMUR1A_BASE;
	case 3:
		return MODULE_PMUR1B_BASE;
	case 4:
		return MODULE_PMUR2A_BASE;
	case 5:
		return MODULE_PMUR2B_BASE;
	case 6:
		return MODULE_PMUR3A_BASE;
	case 7:
		return MODULE_PMUR3B_BASE;
	case 8:
		return MODULE_PMUR4A_BASE;
	case 9:
		return MODULE_PMUR4B_BASE;
	default:
		return MODULE_PMURCS_BASE;
	}
}

static target_addr_t tc4x_rram_get_module_base(uint32_t nvm_addr)
{
	if (tc4x_rram_is_dnvm_addr(nvm_addr)) {
		if (nvm_addr >= 0xAE800000u)
			return MODULE_DMUR1_BASE;
		return MODULE_DMUR0_BASE;
	}

	return tc4x_rram_get_pmur_base(nvm_addr);
}

static target_addr_t tc4x_rram_get_cfi_base(uint32_t nvm_addr)
{
	target_addr_t module_base = tc4x_rram_get_module_base(nvm_addr);

	switch (module_base) {
	case MODULE_DMUR0_BASE:
	case MODULE_PMUR0A_BASE:
	case MODULE_PMUR0B_BASE:
		return CPU_CFI0_BASE;
	case MODULE_DMUR1_BASE:
	case MODULE_PMUR1A_BASE:
	case MODULE_PMUR1B_BASE:
		return CPU_CFI1_BASE;
	case MODULE_PMUR2A_BASE:
	case MODULE_PMUR2B_BASE:
		return CPU_CFI2_BASE;
	case MODULE_PMUR3A_BASE:
	case MODULE_PMUR3B_BASE:
		return CPU_CFI3_BASE;
	case MODULE_PMUR4A_BASE:
	case MODULE_PMUR4B_BASE:
		return CPU_CFI4_BASE;
	case MODULE_PMURCS_BASE:
		return CPU_CFICS_BASE;
	default:
		return CPU_CFI0_BASE;
	}
}

static void tc4x_rram_error_append(char *err_str, size_t err_str_size, const char *msg)
{
	size_t len;

	if (!err_str || !msg || err_str_size == 0)
		return;

	len = strlen(err_str);
	if (len >= err_str_size - 1)
		return;

	snprintf(err_str + len, err_str_size - len, "%s%s", len ? ", " : "", msg);
}

static void tc4x_rram_decode_sfr_status(uint32_t reqstat, char *err_str, size_t err_str_size)
{
	uint32_t known_mask = 0;

	if (reqstat & (1u << 2)) {
		known_mask |= (1u << 2);
		tc4x_rram_error_append(err_str, err_str_size, "protection error");
	}
	if (reqstat & (1u << 3)) {
		known_mask |= (1u << 3);
		tc4x_rram_error_append(err_str, err_str_size, "command sequence error");
	}
	if (reqstat & (1u << 4)) {
		known_mask |= (1u << 4);
		tc4x_rram_error_append(err_str, err_str_size, "abort error");
	}
	if (reqstat & (1u << 5)) {
		known_mask |= (1u << 5);
		tc4x_rram_error_append(err_str, err_str_size, "clear error");
	}
	if (reqstat & (1u << 6)) {
		known_mask |= (1u << 6);
		tc4x_rram_error_append(err_str, err_str_size, "program verify error");
	}
	if (reqstat & (1u << 7)) {
		known_mask |= (1u << 7);
		tc4x_rram_error_append(err_str, err_str_size, "erase verify error");
	}
	if (reqstat & (1u << 8)) {
		known_mask |= (1u << 8);
		tc4x_rram_error_append(err_str, err_str_size, "flash operation error");
	}
	if (reqstat & (1u << 19)) {
		known_mask |= (1u << 19);
		tc4x_rram_error_append(err_str, err_str_size, "no page mode entry");
	}
	if (reqstat & (1u << 20)) {
		known_mask |= (1u << 20);
		tc4x_rram_error_append(err_str, err_str_size, "flash operation timeout");
	}

	uint32_t unknown_mask = (reqstat & UR_REQSTAT_ERROR_MASK) & ~known_mask;
	for (unsigned int bit = 0; bit < 32; bit++) {
		if (unknown_mask & (1u << bit)) {
			char bit_desc[32];
			snprintf(bit_desc, sizeof(bit_desc), "unknown SFR error bit %u", bit);
			tc4x_rram_error_append(err_str, err_str_size, bit_desc);
		}
	}
}

static void tc4x_rram_decode_algo_status(uint32_t status, char *err_str, size_t err_str_size)
{
	if (status & RRAM_ALGO_ERROR_BUSY)
		tc4x_rram_error_append(err_str, err_str_size, "algorithm busy");
	if (status & RRAM_ALGO_ERROR_TIMEOUT)
		tc4x_rram_error_append(err_str, err_str_size, "algorithm timeout");
	if (status & RRAM_ALGO_ERROR_PAGE)
		tc4x_rram_error_append(err_str, err_str_size, "algorithm page mode failure");

	tc4x_rram_decode_sfr_status(status, err_str, err_str_size);
}

static int tc4x_rram_queue_clear_status(struct ocmts *ocmts, target_addr_t module_base)
{
	uint32_t clr = UR_CLRSTAT_CLEAR_ALL;

	return ocmts_queue_write_u32(ocmts, module_base + UR_CLRSTAT_OFFSET, &clr);
}

static int tc4x_rram_wait_request_done(struct flash_bank *bank, target_addr_t module_base,
		uint32_t timeout_ms, uint32_t *reqstat_out)
{
	struct ocmts *ocmts = target_to_tricore(bank->target)->ocmts;
	int64_t start_time = timeval_ms();
	uint32_t reqstat = 0;
	int ret;

	while (timeval_ms() - start_time < timeout_ms) {
		ret = ocmts_queue_read_u32(ocmts, module_base + UR_REQSTAT_OFFSET, &reqstat);
		if (ret != ERROR_OK)
			return ret;

		ret = ocmts_run(ocmts);
		if (ret != ERROR_OK)
			return ret;

		if ((reqstat & UR_REQSTAT_BUSY) == 0 && (reqstat & UR_REQSTAT_DONE) != 0) {
			*reqstat_out = reqstat;
			return ERROR_OK;
		}

		alive_sleep(1);
	}

	if (reqstat_out)
		*reqstat_out = reqstat;

	char err_str[256] = {0};
	tc4x_rram_decode_sfr_status(reqstat, err_str, sizeof(err_str));
	if (err_str[0] != '\0')
		LOG_ERROR("RRAM request timed out (module=0x%08" PRIx64 ", reqstat=0x%08" PRIx32 ", %s)",
				module_base, reqstat, err_str);
	else
		LOG_ERROR("RRAM request timed out (module=0x%08" PRIx64 ", reqstat=0x%08" PRIx32 ")",
				module_base, reqstat);
	return ERROR_FLASH_OPERATION_FAILED;
}

static int tc4x_rram_write_page_register(struct flash_bank *bank, uint32_t page_addr,
		const uint8_t *buffer, uint32_t page_size, uint32_t timeout_ms)
{
	struct ocmts *ocmts = target_to_tricore(bank->target)->ocmts;
	target_addr_t module_base = tc4x_rram_get_module_base(page_addr);
	uint32_t reqstat;
	uint32_t initwr = UR_REQUEST_INITWR;
	uint32_t stwr = UR_REQUEST_STWR;
	int ret;

	ret = tc4x_rram_queue_clear_status(ocmts, module_base);
	if (ret != ERROR_OK)
		return ret;

	ret = ocmts_queue_write_u32(ocmts, module_base + UR_REQUEST_OFFSET, &initwr);
	if (ret != ERROR_OK)
		return ret;

	ret = ocmts_queue_write_u32(ocmts, module_base + UR_NVMADDR_OFFSET, &page_addr);
	if (ret != ERROR_OK)
		return ret;

	for (uint32_t i = 0; i < page_size; i += 4) {
		ret = ocmts_queue_write_u32(ocmts, module_base + UR_WDATA0_OFFSET + i, (void *)(buffer + i));
		if (ret != ERROR_OK)
			return ret;
	}

	ret = ocmts_queue_write_u32(ocmts, module_base + UR_REQUEST_OFFSET, &stwr);
	if (ret != ERROR_OK)
		return ret;

	ret = ocmts_run(ocmts);
	if (ret != ERROR_OK)
		return ret;

	ret = tc4x_rram_wait_request_done(bank, module_base, timeout_ms, &reqstat);
	if (ret != ERROR_OK)
		return ret;

	if (reqstat & UR_REQSTAT_ERROR_MASK) {
		char err_str[256] = {0};
		tc4x_rram_decode_sfr_status(reqstat, err_str, sizeof(err_str));
		if (err_str[0] != '\0')
			LOG_ERROR("RRAM write failed (addr=0x%08" PRIx32 ", reqstat=0x%08" PRIx32 ", %s)",
					page_addr, reqstat, err_str);
		else
			LOG_ERROR("RRAM write failed (addr=0x%08" PRIx32 ", reqstat=0x%08" PRIx32 ")",
					page_addr, reqstat);
		return ERROR_FLASH_OPERATION_FAILED;
	}

	return ERROR_OK;
}

static int tc4x_rram_set_bank(struct flash_bank *bank, struct tc4x_rram_bank *rram_bank,
		uint32_t chipid)
{
	static const struct tc4x_rram_bank_info tc4x_rram_tc48x_layout[] = {
		{0x80000000, 0x200000, TC4X_RRAM_PRRAM, 32, 0x80000},
		{0x80200000, 0x200000, TC4X_RRAM_PRRAM, 32, 0x80000},
		{0x80400000, 0x200000, TC4X_RRAM_PRRAM, 32, 0x80000},
		{0x80600000, 0x200000, TC4X_RRAM_PRRAM, 32, 0x80000},
		{0x80800000, 0x200000, TC4X_RRAM_PRRAM, 32, 0x80000},
		{0x80900000, 0x200000, TC4X_RRAM_PRRAM, 32, 0x80000},
		{0x80A00000, 0x200000, TC4X_RRAM_PRRAM, 32, 0x80000},
		{0x80C00000, 0x200000, TC4X_RRAM_PRRAM, 32, 0x80000},
		{0x84000000, 0x080000, TC4X_RRAM_PRRAM, 32, 0x80000},
		{0xAE000000, 0x040000, TC4X_RRAM_DNVM, 8, 0x40000},
		{0xAE400000, 0x008000, TC4X_RRAM_UCB, 8, 0x8000},
		{0xAE800000, 0x020000, TC4X_RRAM_DNVM, 8, 0x20000},
		{0xAEC00000, 0x008000, TC4X_RRAM_UCB, 8, 0x8000},
	};
	uint32_t prod_family;
	const struct tc4x_rram_bank_info *layout;
	size_t layout_count;

	prod_family = (chipid & UCB_CHIPID_PROD) >> 26;
	switch (prod_family) {
	case 8:
		layout = tc4x_rram_tc48x_layout;
		layout_count = ARRAY_SIZE(tc4x_rram_tc48x_layout);
		rram_bank->device = TC4X_RRAM_TC48X;
		break;
	default:
		LOG_ERROR("CHIPID product family %" PRIu32 " is not supported by tc4x_rram driver.",
				prod_family);
		return ERROR_FLASH_BANK_INVALID;
	}

	for (size_t i = 0; i < layout_count; i++) {
		if (layout[i].base == bank->base && layout[i].size == bank->size) {
			rram_bank->type = layout[i].type;
			rram_bank->page_size = layout[i].page_size;
			rram_bank->sector_size = layout[i].sector_size;
			rram_bank->write_timeout_ms = 10;
			rram_bank->margin_timeout_ms = 10;
			return ERROR_OK;
		}
	}

	LOG_ERROR("Failed to find TC4x RRAM bank at 0x%08" PRIx64 " size 0x%08" PRIx32,
			bank->base, bank->size);
	return ERROR_FLASH_BANK_INVALID;
}

static int tc4x_rram_probe(struct flash_bank *bank)
{
	struct tc4x_rram_bank *rram_bank = bank->driver_priv;
	uint32_t chipid;
	int retval;
	uint32_t flash_addr = bank->base;

	if (rram_bank->probed)
		return ERROR_OK;

	retval = target_read_u32(bank->target, UCB_CHIPID, &chipid);
	if (retval != ERROR_OK) {
		LOG_ERROR("Cannot read CHIPID register.");
		return retval;
	}

	retval = tc4x_rram_set_bank(bank, rram_bank, chipid);
	if (retval != ERROR_OK)
		return retval;

	bank->minimal_write_gap = FLASH_WRITE_GAP_SECTOR;
	bank->write_start_alignment = rram_bank->page_size;
	bank->write_end_alignment = rram_bank->page_size;
	bank->num_sectors = bank->size / rram_bank->sector_size;
	bank->sectors = calloc(bank->num_sectors, sizeof(struct flash_sector));
	if (!bank->sectors)
		return ERROR_FLASH_OPERATION_FAILED;

	for (unsigned int i = 0; i < bank->num_sectors; i++) {
		bank->sectors[i].size = rram_bank->sector_size;
		bank->sectors[i].offset = flash_addr - bank->base;
		flash_addr += rram_bank->sector_size;
		bank->sectors[i].is_erased = 1;
		bank->sectors[i].is_protected = -1;
	}

	rram_bank->probed = true;
	return ERROR_OK;
}

static int tc4x_rram_auto_probe(struct flash_bank *bank)
{
	struct tc4x_rram_bank *rram_bank = bank->driver_priv;

	if (rram_bank->probed)
		return ERROR_OK;

	return tc4x_rram_probe(bank);
}

static int tc4x_rram_erase(struct flash_bank *bank, unsigned int first, unsigned int last)
{
	struct tc4x_rram_bank *rram_bank = bank->driver_priv;

	if (rram_bank->type == TC4X_RRAM_UCB && !rram_bank->ucb_unlocked) {
		LOG_WARNING("UCB bank has not been unlocked. Skipping operation.");
		return ERROR_OK;
	}

	if (first > last || last >= bank->num_sectors)
		return ERROR_FLASH_SECTOR_INVALID;

	for (unsigned int i = first; i <= last; i++)
		bank->sectors[i].is_erased = 1;

	LOG_INFO("TC4x RRAM does not require erase; operation treated as success.");
	return ERROR_OK;
}

static int tc4x_rram_erase_check(struct flash_bank *bank)
{
	for (unsigned int i = 0; i < bank->num_sectors; i++)
		bank->sectors[i].is_erased = 1;

	return ERROR_OK;
}

static int tc4x_rram_write_algo(struct flash_bank *bank, uint32_t address,
		const uint8_t *buffer, uint32_t bytes)
{
	struct tc4x_rram_bank *rram_bank = bank->driver_priv;
	struct target *target = bank->target;
	struct reg_param reg_params[6];
	const uint8_t *write_code;
	size_t write_code_size;
	target_addr_t module_base;
	uint32_t buffer_size = 0x8000;
	const uint32_t nvmaddr = tc4x_rram_to_nvm_segment(address);
	int ret;
	struct working_area *source;

	if (rram_bank->type == TC4X_RRAM_PRRAM) {
		write_code = tc4x_rram_pnvm_write_code;
		write_code_size = sizeof(tc4x_rram_pnvm_write_code);
	} else {
		write_code = tc4x_rram_dnvm_write_code;
		write_code_size = sizeof(tc4x_rram_dnvm_write_code);
	}

	ret = target_write_buffer(target, 0x70100000, write_code_size, write_code);
	if (ret != ERROR_OK)
		return ret;

	module_base = tc4x_rram_get_module_base(nvmaddr);

	while (target_alloc_working_area(target, buffer_size, &source) != ERROR_OK) {
		buffer_size /= 2;
		buffer_size &= ~3UL;
		if (buffer_size <= 256) {
			LOG_WARNING("No large enough working area available, can't do block memory writes");
			return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		}
	}

	init_reg_param(&reg_params[0], "a4", 32, PARAM_OUT);
	init_reg_param(&reg_params[1], "d4", 32, PARAM_OUT);
	init_reg_param(&reg_params[2], "d5", 32, PARAM_OUT);
	init_reg_param(&reg_params[3], "d6", 32, PARAM_OUT);
	init_reg_param(&reg_params[4], "a5", 32, PARAM_OUT);
	init_reg_param(&reg_params[5], "d2", 32, PARAM_IN);

	buf_set_u32(reg_params[0].value, 0, 32, (uint32_t)source->address);
	buf_set_u32(reg_params[1].value, 0, 32, source->size);
	buf_set_u32(reg_params[2].value, 0, 32, nvmaddr);
	buf_set_u32(reg_params[3].value, 0, 32, bytes);
	buf_set_u32(reg_params[4].value, 0, 32, (uint32_t)module_base);

	ret = target_run_flash_async_algorithm(target, buffer, bytes / 4, 4,
			0, NULL, ARRAY_SIZE(reg_params), reg_params,
			source->address, source->size, 0x70100000, 0, NULL);

	if (ret == ERROR_FLASH_OPERATION_FAILED) {
		uint32_t status = buf_get_u32(reg_params[5].value, 0, 32);
		char err_str[256] = {0};
		tc4x_rram_decode_algo_status(status, err_str, sizeof(err_str));
		if (err_str[0] != '\0')
			LOG_ERROR("RRAM write algorithm failed with status 0x%08" PRIx32 " (%s)", status, err_str);
		else
			LOG_ERROR("RRAM write algorithm failed with status 0x%08" PRIx32, status);
	}

	target_free_working_area(target, source);
	destroy_reg_param(&reg_params[0]);
	destroy_reg_param(&reg_params[1]);
	destroy_reg_param(&reg_params[2]);
	destroy_reg_param(&reg_params[3]);
	destroy_reg_param(&reg_params[4]);
	destroy_reg_param(&reg_params[5]);

	return ret;
}

static int tc4x_rram_write(struct flash_bank *bank, const uint8_t *buffer,
		uint32_t offset, uint32_t count)
{
	struct tc4x_rram_bank *rram_bank = bank->driver_priv;
	int ret;

	if (rram_bank->type == TC4X_RRAM_UCB && !rram_bank->ucb_unlocked) {
		LOG_WARNING("UCB bank has not been unlocked. Skipping operation.");
		return ERROR_OK;
	}

	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	ret = tc4x_rram_auto_probe(bank);
	if (ret != ERROR_OK)
		return ret;

	if ((offset & (rram_bank->page_size - 1)) || (count % rram_bank->page_size) != 0)
		return ERROR_FLASH_DST_BREAKS_ALIGNMENT;

	if (!rram_bank->fallback_mode) {
		ret = tc4x_rram_write_algo(bank, bank->base + offset, buffer, count);
		if (ret == ERROR_OK)
			return ERROR_OK;
		if (ret != ERROR_TARGET_RESOURCE_NOT_AVAILABLE)
			return ret;

		LOG_WARNING("No enough resources to run RRAM write algorithm, fallback to slow register sequence.");
	}

	for (uint32_t page_offset = 0; page_offset < count; page_offset += rram_bank->page_size) {
		uint32_t page_addr = tc4x_rram_to_nvm_segment(bank->base + offset + page_offset);
		ret = tc4x_rram_write_page_register(bank, page_addr,
				buffer + page_offset, rram_bank->page_size, rram_bank->write_timeout_ms);
		if (ret != ERROR_OK)
			return ret;
	}

	return ERROR_OK;
}

static int tc4x_rram_read(struct flash_bank *bank, uint8_t *buffer,
		uint32_t offset, uint32_t count)
{
	uint32_t mapped_addr = ((bank->base + offset) & ~0xF0000000u) + 0xA0000000u;
	uint32_t nvm_addr = tc4x_rram_to_nvm_segment(bank->base + offset);
	target_addr_t cfi_pfibufdis = tc4x_rram_get_cfi_base(nvm_addr) + CPU_CFI_PFIBUFDIS_OFFSET;
	uint32_t disable = 1;
	uint32_t enable = 0;
	int ret;
	int restore_ret;

	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	ret = tc4x_rram_auto_probe(bank);
	if (ret != ERROR_OK)
		return ret;

	ret = target_write_u32(bank->target, cfi_pfibufdis, disable);
	if (ret != ERROR_OK) {
		LOG_ERROR("Failed to disable prefetch buffer at 0x%08" PRIx64 " (ret=%d)",
				cfi_pfibufdis, ret);
		return ret;
	}

	/* Read through the mapped memory window, same model as eflash. */
	ret = target_read_buffer(bank->target, mapped_addr, count, buffer);

	restore_ret = target_write_u32(bank->target, cfi_pfibufdis, enable);
	if (restore_ret != ERROR_OK) {
		LOG_ERROR("Failed to re-enable prefetch buffer at 0x%08" PRIx64 " (ret=%d)",
				cfi_pfibufdis, restore_ret);
		if (ret == ERROR_OK)
			ret = restore_ret;
	}

	return ret;
}

static void tc4x_rram_free_driver_priv(struct flash_bank *bank)
{
	free(bank->driver_priv);
}

FLASH_BANK_COMMAND_HANDLER(tc4x_rram_flash_bank_command)
{
	struct tc4x_rram_bank *rram_bank;

	rram_bank = malloc(sizeof(struct tc4x_rram_bank));
	if (!rram_bank)
		return ERROR_FLASH_OPERATION_FAILED;

	rram_bank->probed = false;
	rram_bank->fallback_mode = false;
	rram_bank->ucb_unlocked = false;
	rram_bank->type = TC4X_RRAM_PRRAM;
	rram_bank->device = TC4X_RRAM_TC48X;
	rram_bank->page_size = 0;
	rram_bank->sector_size = 0;
	rram_bank->write_timeout_ms = 10;
	rram_bank->margin_timeout_ms = 10;
	bank->driver_priv = rram_bank;

	return ERROR_OK;
}

COMMAND_HANDLER(tc4x_rram_set_fallback_mode_command)
{
	struct flash_bank *bank;
	struct tc4x_rram_bank *rram_bank;
	int retval;

	if (CMD_ARGC < 1 || CMD_ARGC > 2)
		return ERROR_COMMAND_ARGUMENT_INVALID;

	retval = CALL_COMMAND_HANDLER(flash_command_get_bank, 0, &bank);
	if (retval != ERROR_OK)
		return retval;

	rram_bank = bank->driver_priv;

	if (CMD_ARGC == 2) {
		if (strcmp(CMD_ARGV[1], "on") == 0)
			rram_bank->fallback_mode = true;
		else if (strcmp(CMD_ARGV[1], "off") == 0)
			rram_bank->fallback_mode = false;
		else
			return ERROR_COMMAND_ARGUMENT_INVALID;
	} else {
		rram_bank->fallback_mode = !rram_bank->fallback_mode;
	}

	return ERROR_OK;
}

static const struct command_registration tc4x_rram_subcommand_handlers[] = {
	{
		.name = "fallback",
		.handler = tc4x_rram_set_fallback_mode_command,
		.mode = COMMAND_EXEC,
		.usage = "<bank> [on|off]",
		.help = "Set or toggle fallback mode. In fallback mode, driver uses register access instead of loader algorithm.",
	},
	COMMAND_REGISTRATION_DONE
};

static const struct command_registration tc4x_rram_command_handlers[] = {
	{
		.name = "tc4x_rram",
		.mode = COMMAND_ANY,
		.usage = "",
		.help = "Commands for TC4x RRAM driver",
		.chain = tc4x_rram_subcommand_handlers,
	},
	COMMAND_REGISTRATION_DONE
};

const struct flash_driver tc4x_rram = {
	.name = "tc4x_rram",
	.flash_bank_command = tc4x_rram_flash_bank_command,
	.commands = tc4x_rram_command_handlers,
	.probe = tc4x_rram_probe,
	.auto_probe = tc4x_rram_auto_probe,
	.erase = tc4x_rram_erase,
	.write = tc4x_rram_write,
	.read = tc4x_rram_read,
	.erase_check = tc4x_rram_erase_check,
	.free_driver_priv = tc4x_rram_free_driver_priv,
};
