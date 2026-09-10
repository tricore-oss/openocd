// SPDX-License-Identifier: GPL-2.0-or-later

/***************************************************************************
 *   AURIX eFlash Driver for Infineon AURIX                                *
 *   Copyright (C) 2026 Infineon Technologies AG                           *
 ***************************************************************************/

#include <unistd.h>
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "flash/common.h"
#include "flash/nor/core.h"
#include "flash/nor/driver.h"
#include "helper/log.h"
#include "helper/time_support.h"
#include "helper/types.h"
#include "target/algorithm.h"
#include "target/aurix/ocmts.h"
#include "target/aurix/tricore.h"
#include "target/target.h"

enum aurix_eflash_type {
	AURIX_EFLASH_UNKNOWN = -1,
	AURIX_EFLASH_PFLASH = 0,
	AURIX_EFLASH_DFLASH,
	AURIX_EFLASH_UCB,
};

struct aurix_eflash_params {
	/** Size of a (logical) sector. (Smallest earasable unit) */
	uint16_t sector_size;
	/** Maximum number of erasable sectors */
	uint16_t num_erase_sectors;
	/** Offset of the page bit in the status register */
	uint8_t page_bit;
	/** Size of a physical sector, where earase commands cannot cross */
	uint32_t phys_sector_size;
	/** Offset of the busy bit for the bank in the status register */
	uint8_t busy_bit;
	/** Number of bytes for a burst program operation */
	uint16_t burst_size;
	/** Number of bytes for a page program operation. (Smallest programmable unit) */
	uint16_t page_size;
	/** Write timeout in milliseconds */
	uint64_t write_timeout_ms;
	/** Erase timeout in milliseconds */
	uint64_t erase_timeout_ms;
};

struct aurix_eflash_bank {
	/** Address of the command sequencer */
	target_addr_t cmd_addr;
	/** Address of the DMU registers */
	target_addr_t reg_addr;
	/** Address of the FSI registers */
	target_addr_t fsi_addr;
	/** Offset of the error register for a given command sequencer */
	target_addr_t err_offset;
	/** Offset of the status register for a ginven command sequencer */
	target_addr_t sts_offset;
	/** Flash memory type */
	enum aurix_eflash_type type;
	/** Type-specific flash operation parameters */
	struct aurix_eflash_params params;
	/** Bank probed */
	bool probed;
	/** TC4 eflash */
	bool tc4x;
	/** Seucre sequencer */
	bool secure;
	/** Fallback mode for flash write */
	bool fallback_mode;
	/** UCB access is unlocked */
	bool ucb_unlocked;
};

static uint32_t clear_status_key = 0xFA;
static uint32_t reset_to_read_key = 0xF0;
static uint32_t page_mode_p_key = 0x50;
static uint32_t page_mode_d_key = 0x5D;

#define UCB_CHIPID 0xAE400008
#define UCB_CHIPID_PROD 0x3C000000
#define SCU_CHIPID 0xF0036140
#define SCU_CHIPID_CHREV 0x3F
#define SCU_CHIPID_CHTEC 0xC0
#define SCU_CHIPID_CHID  0xF000
#define SCU_CHIPID_FSIZE 0x0F000000

struct aurix_eflash_bank_info {
	enum aurix_eflash_type type;
	target_addr_t base;
	uint32_t size;
	uint32_t phys_sector_size;
	uint8_t busy_bit;
	bool secure;
};

static const struct aurix_eflash_bank_info *aurix_eflash_lookup_bank_info(const struct aurix_eflash_bank_info *layout,
		size_t layout_len, struct flash_bank *bank)
{
	for (size_t i = 0; i < layout_len; i++) {
		if (layout[i].base == bank->base && layout[i].size == bank->size)
			return &layout[i];
	}

	return NULL;
}

static inline void aurix_load_tc3x_type_params(struct aurix_eflash_bank *aurix_bank)
{
	static const struct aurix_eflash_params tc3x_type_layout[] = {
		[AURIX_EFLASH_PFLASH] = {
			.sector_size = 16 * 1024,
			.num_erase_sectors = 512 * 1024 / (16 * 1024),
			.page_bit = 21,
			.burst_size = 256,
			.page_size = 32,
			.write_timeout_ms = 10,
			.erase_timeout_ms = 400,
		},
		[AURIX_EFLASH_DFLASH] = {
			.sector_size = 4 * 1024,
			.num_erase_sectors = 256 * 1024 / (4 * 1024),
			.page_bit = 20,
			.burst_size = 32,
			.page_size = 8,
			.write_timeout_ms = 10,
			.erase_timeout_ms = 500,
		},
		[AURIX_EFLASH_UCB] = {
			.sector_size = 512,
			.num_erase_sectors = 1,
			.page_bit = 20,
			.burst_size = 32,
			.page_size = 8,
			.write_timeout_ms = 10,
			.erase_timeout_ms = 200,
		},
	};

	assert(aurix_bank->type >= AURIX_EFLASH_PFLASH && aurix_bank->type <= AURIX_EFLASH_UCB);
	aurix_bank->params = tc3x_type_layout[aurix_bank->type];
}

static int tc3x_eflash_set_bank(struct flash_bank *bank, struct aurix_eflash_bank *aurix_bank,
		uint32_t chipid)
{
	static const struct aurix_eflash_bank_info tc36x_layout[] = {
		{AURIX_EFLASH_PFLASH, 0x80000000, 2 * 1024 * 1024, 1024 * 1024, 2, false},
		{AURIX_EFLASH_PFLASH, 0x80300000, 2 * 1024 * 1024, 1024 * 1024, 3, false},
		{AURIX_EFLASH_DFLASH, 0xAF000000, 1 * 1024 * 1024, 1 * 1024 * 1024, 0, false},
		{AURIX_EFLASH_UCB, 0xAF400000, 24 * 1024, 24 * 1024, 0, false},
		{AURIX_EFLASH_DFLASH, 0xAFC00000, 128 * 1024, 128 * 1024, 1, true},
	};
	static const struct aurix_eflash_bank_info tc37x_layout[] = {
		{AURIX_EFLASH_PFLASH, 0x80000000, 3 * 1024 * 1024, 1024 * 1024, 2, false},
		{AURIX_EFLASH_PFLASH, 0x80300000, 3 * 1024 * 1024, 1024 * 1024, 3, false},
		{AURIX_EFLASH_DFLASH, 0xAF000000, 1 * 1024 * 1024, 1 * 1024 * 1024, 0, false},
		{AURIX_EFLASH_UCB, 0xAF400000, 24 * 1024, 24 * 1024, 0, false},
		{AURIX_EFLASH_DFLASH, 0xAFC00000, 128 * 1024, 128 * 1024, 1, true},
	};
	static const struct aurix_eflash_bank_info tc38x_layout[] = {
		{AURIX_EFLASH_PFLASH, 0x80000000, 3 * 1024 * 1024, 1024 * 1024, 2, false},
		{AURIX_EFLASH_PFLASH, 0x80300000, 3 * 1024 * 1024, 1024 * 1024, 3, false},
		{AURIX_EFLASH_PFLASH, 0x80600000, 3 * 1024 * 1024, 1024 * 1024, 4, false},
		{AURIX_EFLASH_PFLASH, 0x80900000, 1 * 1024 * 1024, 1024 * 1024, 5, false},
		{AURIX_EFLASH_DFLASH, 0xAF000000, 1 * 1024 * 1024, 1 * 1024 * 1024, 0, false},
		{AURIX_EFLASH_UCB, 0xAF400000, 24 * 1024, 24 * 1024, 0, false},
		{AURIX_EFLASH_DFLASH, 0xAFC00000, 128 * 1024, 128 * 1024, 1, true},
	};
	static const struct aurix_eflash_bank_info tc39x_layout[] = {
		{AURIX_EFLASH_PFLASH, 0x80000000, 3 * 1024 * 1024, 1024 * 1024, 2, false},
		{AURIX_EFLASH_PFLASH, 0x80300000, 3 * 1024 * 1024, 1024 * 1024, 3, false},
		{AURIX_EFLASH_PFLASH, 0x80600000, 3 * 1024 * 1024, 1024 * 1024, 4, false},
		{AURIX_EFLASH_PFLASH, 0x80900000, 3 * 1024 * 1024, 1024 * 1024, 5, false},
		{AURIX_EFLASH_PFLASH, 0x80C00000, 3 * 1024 * 1024, 1024 * 1024, 6, false},
		{AURIX_EFLASH_PFLASH, 0x80F00000, 1 * 1024 * 1024, 1024 * 1024, 7, false},
		{AURIX_EFLASH_DFLASH, 0xAF000000, 1 * 1024 * 1024, 1 * 1024 * 1024, 0, false},
		{AURIX_EFLASH_UCB, 0xAF400000, 24 * 1024, 24 * 1024, 0, false},
		{AURIX_EFLASH_DFLASH, 0xAFC00000, 128 * 1024, 128 * 1024, 1, true},
	};
	static const struct aurix_eflash_bank_info tc3ex_layout[] = {
		{AURIX_EFLASH_PFLASH, 0x80000000, 3 * 1024 * 1024, 1024 * 1024, 2, false},
		{AURIX_EFLASH_PFLASH, 0x80300000, 3 * 1024 * 1024, 1024 * 1024, 3, false},
		{AURIX_EFLASH_PFLASH, 0x80600000, 3 * 1024 * 1024, 1024 * 1024, 4, false},
		{AURIX_EFLASH_PFLASH, 0x80900000, 3 * 1024 * 1024, 1024 * 1024, 5, false},
		{AURIX_EFLASH_DFLASH, 0xAF000000, 1 * 1024 * 1024, 1 * 1024 * 1024, 0, false},
		{AURIX_EFLASH_UCB, 0xAF400000, 24 * 1024, 24 * 1024, 0, false},
		{AURIX_EFLASH_DFLASH, 0xAFC00000, 128 * 1024, 128 * 1024, 1, true},
	};

	const uint32_t chipid_variant = (chipid & SCU_CHIPID_CHID) >> 12;
	const struct aurix_eflash_bank_info *bank_info = NULL;

	switch (chipid_variant) {
	case 0x6:
		bank_info = aurix_eflash_lookup_bank_info(tc36x_layout, ARRAY_SIZE(tc36x_layout), bank);
		break;
	case 0x7:
		bank_info = aurix_eflash_lookup_bank_info(tc37x_layout, ARRAY_SIZE(tc37x_layout), bank);
		break;
	case 0x8:
		bank_info = aurix_eflash_lookup_bank_info(tc38x_layout, ARRAY_SIZE(tc38x_layout), bank);
		break;
	case 0x9:
		bank_info = aurix_eflash_lookup_bank_info(tc39x_layout, ARRAY_SIZE(tc39x_layout), bank);
		break;
	case 0xE:
		/* TC3Ex uses the same layout envelope as the TC39x family for the eFLASH ranges. */
		bank_info = aurix_eflash_lookup_bank_info(tc3ex_layout, ARRAY_SIZE(tc3ex_layout), bank);
		break;
	default:
		LOG_ERROR("Unsupported TC3x chip variant: TC3%Xx", chipid_variant);
		return ERROR_FAIL;
	}

	if (!bank_info) {
		LOG_ERROR("Failed to find eFLASH bank at 0x%08" PRIx64, bank->base);
		return ERROR_FLASH_BANK_INVALID;
	}

	/* Load the type-specific parameters for the TC3x eFLASH bank. */
	aurix_bank->type = bank_info->type;
	aurix_load_tc3x_type_params(aurix_bank);
	aurix_bank->params.phys_sector_size = bank_info->phys_sector_size;
	aurix_bank->params.busy_bit = bank_info->busy_bit;
	aurix_bank->secure = bank_info->secure;

	if (bank->base == 0xAFC00000) {
		aurix_bank->fsi_addr = 0xF8030000;
		aurix_bank->reg_addr = 0xF8060000;
		aurix_bank->cmd_addr = 0xAFC00000;
		aurix_bank->sts_offset = 0x10;
		aurix_bank->err_offset = 0x34;
	} else {
		aurix_bank->fsi_addr = 0xF8030000;
		aurix_bank->reg_addr = 0xF8040000;
		aurix_bank->cmd_addr = 0xAF000000;
		aurix_bank->sts_offset = 0x10;
		aurix_bank->err_offset = 0x34;
	}

	return ERROR_OK;
}

static inline void aurix_load_tc4x_type_params(struct aurix_eflash_bank *aurix_bank)
{
	static const struct aurix_eflash_params tc4x_type_layout[] = {
		[AURIX_EFLASH_PFLASH] = {
			.sector_size = 16 * 1024,
			.num_erase_sectors = 512 * 1024 / (16 * 1024),
			.page_bit = 25,
			.burst_size = 512,
			.page_size = 32,
			.write_timeout_ms = 10,
			.erase_timeout_ms = 400,
		},
		[AURIX_EFLASH_DFLASH] = {
			.sector_size = 2 * 1024,
			.num_erase_sectors = 256 * 1024 / (2 * 1024),
			.page_bit = 24,
			.burst_size = 32,
			.page_size = 8,
			.write_timeout_ms = 10,
			.erase_timeout_ms = 500,
		},
		[AURIX_EFLASH_UCB] = {
			.sector_size = 512,
			.num_erase_sectors = 1,
			.page_bit = 24,
			.burst_size = 32,
			.page_size = 8,
			.write_timeout_ms = 10,
			.erase_timeout_ms = 200,
		},
	};

	assert(aurix_bank->type >= AURIX_EFLASH_PFLASH && aurix_bank->type <= AURIX_EFLASH_UCB);
	aurix_bank->params = tc4x_type_layout[aurix_bank->type];
}

static int tc4x_eflash_set_bank(struct flash_bank *bank, struct aurix_eflash_bank *aurix_bank,
		uint32_t chipid)
{
	static const struct aurix_eflash_bank_info tc4dx_layout[] = {
		{AURIX_EFLASH_PFLASH, 0x80000000, 2 * 1024 * 1024, 512 * 1024, 0, false},
		{AURIX_EFLASH_PFLASH, 0x80200000, 2 * 1024 * 1024, 512 * 1024, 1, false},
		{AURIX_EFLASH_PFLASH, 0x80400000, 2 * 1024 * 1024, 512 * 1024, 2, false},
		{AURIX_EFLASH_PFLASH, 0x80600000, 2 * 1024 * 1024, 512 * 1024, 3, false},
		{AURIX_EFLASH_PFLASH, 0x80800000, 1 * 1024 * 1024, 512 * 1024, 4, false},
		{AURIX_EFLASH_PFLASH, 0x80900000, 1 * 1024 * 1024, 512 * 1024, 5, false},
		{AURIX_EFLASH_PFLASH, 0x80A00000, 2 * 1024 * 1024, 512 * 1024, 6, false},
		{AURIX_EFLASH_PFLASH, 0x80C00000, 2 * 1024 * 1024, 512 * 1024, 7, false},
		{AURIX_EFLASH_PFLASH, 0x80E00000, 2 * 1024 * 1024, 512 * 1024, 8, false},
		{AURIX_EFLASH_PFLASH, 0x81000000, 2 * 1024 * 1024, 512 * 1024, 9, false},
		{AURIX_EFLASH_PFLASH, 0x81200000, 1 * 1024 * 1024, 512 * 1024, 10, false},
		{AURIX_EFLASH_PFLASH, 0x81300000, 1 * 1024 * 1024, 512 * 1024, 11, false},
		{AURIX_EFLASH_PFLASH, 0x84000000, 1 * 1024 * 1024, 512 * 1024, 18, true},
		{AURIX_EFLASH_DFLASH, 0xAE000000, 1024 * 1024, 128 * 1024, 16, false},
		{AURIX_EFLASH_UCB, 0xAE400000, 80 * 1024, 80 * 1024, 16, false},
		{AURIX_EFLASH_DFLASH, 0xAE800000, 128 * 1024, 64, 17, true},
		{AURIX_EFLASH_UCB, 0xAEC00000, 52 * 1024, 52 * 1024, 17, true},
	};
	static const struct aurix_eflash_bank_info tc49x_layout[] = {
		{AURIX_EFLASH_PFLASH, 0x80000000, 2 * 1024 * 1024, 512 * 1024, 0, false},
		{AURIX_EFLASH_PFLASH, 0x80200000, 2 * 1024 * 1024, 512 * 1024, 1, false},
		{AURIX_EFLASH_PFLASH, 0x80400000, 2 * 1024 * 1024, 512 * 1024, 2, false},
		{AURIX_EFLASH_PFLASH, 0x80600000, 2 * 1024 * 1024, 512 * 1024, 3, false},
		{AURIX_EFLASH_PFLASH, 0x80800000, 2 * 1024 * 1024, 512 * 1024, 4, false},
		{AURIX_EFLASH_PFLASH, 0x80A00000, 2 * 1024 * 1024, 512 * 1024, 5, false},
		{AURIX_EFLASH_PFLASH, 0x80C00000, 2 * 1024 * 1024, 512 * 1024, 6, false},
		{AURIX_EFLASH_PFLASH, 0x80E00000, 2 * 1024 * 1024, 512 * 1024, 7, false},
		{AURIX_EFLASH_PFLASH, 0x81000000, 2 * 1024 * 1024, 512 * 1024, 8, false},
		{AURIX_EFLASH_PFLASH, 0x81200000, 2 * 1024 * 1024, 512 * 1024, 9, false},
		{AURIX_EFLASH_PFLASH, 0x84000000, 1 * 1024 * 1024, 512 * 1024, 10, true},
		{AURIX_EFLASH_DFLASH, 0xAE000000, 1024 * 1024, 128 * 1024, 16, false},
		{AURIX_EFLASH_UCB, 0xAE400000, 80 * 1024, 80 * 1024, 16, false},
		{AURIX_EFLASH_DFLASH, 0xAE800000, 128 * 1024, 64, 17, true},
		{AURIX_EFLASH_UCB, 0xAEC00000, 52 * 1024, 52 * 1024, 17, true},
	};

	const uint32_t prod = (chipid & UCB_CHIPID_PROD) >> 26;
	const struct aurix_eflash_bank_info *bank_info = NULL;

	switch (prod) {
	case 13:
		bank_info = aurix_eflash_lookup_bank_info(tc4dx_layout, ARRAY_SIZE(tc4dx_layout), bank);
		break;
	case 9:
		bank_info = aurix_eflash_lookup_bank_info(tc49x_layout, ARRAY_SIZE(tc49x_layout), bank);
		break;
	default:
		LOG_ERROR("Unsupported TC4x CHIPID product field: 0x%08" PRIx32, prod);
		return ERROR_FAIL;
	}

	if (!bank_info) {
		LOG_ERROR("Failed to find eFLASH bank at 0x%08" PRIx64, bank->base);
		return ERROR_FLASH_BANK_INVALID;
	}

	/* Load the type-specific parameters for the TC3x eFLASH bank. */
	aurix_bank->type = bank_info->type;
	aurix_load_tc4x_type_params(aurix_bank);
	aurix_bank->params.phys_sector_size = bank_info->phys_sector_size;
	aurix_bank->params.busy_bit = bank_info->busy_bit;
	aurix_bank->secure = bank_info->secure;

	/* Select the command sequence interface based on bank addresses */
	if (bank->base == 0x84000000 || bank->base == 0xAE800000 || bank->base == 0xAEC00000) {
		aurix_bank->fsi_addr = 0xF8028000;
		aurix_bank->reg_addr = 0xF8040000;
		aurix_bank->cmd_addr = 0xF80C0000;
		aurix_bank->sts_offset = 0x84;
		aurix_bank->err_offset = 0x90;
	} else {
		aurix_bank->fsi_addr = 0xF8008000;
		aurix_bank->reg_addr = 0xF8040000;
		aurix_bank->cmd_addr = 0xF8080000;
		aurix_bank->sts_offset = 0x4;
		aurix_bank->err_offset = 0x10;
	}

	return ERROR_OK;
}

static int tc3x_eflash_probe(struct flash_bank *bank)
{
	struct aurix_eflash_bank *tc3x_bank = bank->driver_priv;
	uint32_t flash_addr = bank->base;
	uint32_t chipid;
	int retval;

	if (tc3x_bank->probed)
		return ERROR_OK;

	/* Read SCU_CHIPID to detect device. SCU_CHIPID is not valid for TC4 */
	retval = target_read_u32(bank->target, SCU_CHIPID, &chipid);
	if (retval != ERROR_OK) {
		LOG_ERROR("Cannot read tc3x CHIPID register.");
		return retval;
	}

	if ((chipid & SCU_CHIPID_CHTEC) != 0x80) {
		LOG_ERROR("CHIPID register does not match tc3x.");
		return ERROR_FAIL;
	}

	retval = tc3x_eflash_set_bank(bank, tc3x_bank, chipid);
	if (retval != ERROR_OK)
		return retval;

	bank->minimal_write_gap = FLASH_WRITE_GAP_SECTOR;
	bank->write_start_alignment = tc3x_bank->params.burst_size;
	bank->write_end_alignment = tc3x_bank->params.burst_size;
	bank->num_sectors = bank->size / tc3x_bank->params.sector_size;
	bank->sectors = calloc(bank->num_sectors, sizeof(struct flash_sector));
	for (unsigned int i = 0; i < bank->num_sectors; i++) {
		bank->sectors[i].size = tc3x_bank->params.sector_size;
		bank->sectors[i].offset = flash_addr - bank->base;
		flash_addr += tc3x_bank->params.sector_size;
		/* TOOD: Check erased */
		bank->sectors[i].is_erased = -1;
		/* TODO: Check UCB for protection*/
		bank->sectors[i].is_protected = -1;
	}

	tc3x_bank->probed = true;

	return ERROR_OK;
}

static int tc3x_eflash_auto_probe(struct flash_bank *bank)
{
	struct aurix_eflash_bank *tc3x_bank = bank->driver_priv;

	if (tc3x_bank->probed)
		return ERROR_OK;

	return tc3x_eflash_probe(bank);
}

static int tc4x_eflash_probe(struct flash_bank *bank)
{
	struct aurix_eflash_bank *tc4x_bank = bank->driver_priv;
	uint32_t flash_addr = bank->base;
	uint32_t chipid;
	int retval;

	if (tc4x_bank->probed)
		return ERROR_OK;

	retval = target_read_u32(bank->target, UCB_CHIPID, &chipid);
	if (retval != ERROR_OK) {
		LOG_ERROR("Cannot read CHIPID register.");
		return retval;
	}

	retval = tc4x_eflash_set_bank(bank, tc4x_bank, chipid);
	if (retval != ERROR_OK)
		return retval;

	bank->minimal_write_gap = FLASH_WRITE_GAP_SECTOR;
	bank->write_start_alignment = tc4x_bank->params.burst_size;
	bank->write_end_alignment = tc4x_bank->params.burst_size;
	bank->num_sectors = bank->size / tc4x_bank->params.sector_size;
	bank->sectors = calloc(bank->num_sectors, sizeof(struct flash_sector));
	for (unsigned int i = 0; i < bank->num_sectors; i++) {
		bank->sectors[i].size = tc4x_bank->params.sector_size;
		bank->sectors[i].offset = flash_addr - bank->base;
		flash_addr += tc4x_bank->params.sector_size;
		/* TOOD: Check erased */
		bank->sectors[i].is_erased = -1;
		/* TODO: Check UCB for protection*/
		bank->sectors[i].is_protected = -1;
	}

	tc4x_bank->probed = true;

	return ERROR_OK;
}

static int tc4x_eflash_auto_probe(struct flash_bank *bank)
{
	struct aurix_eflash_bank *tc4x_bank = bank->driver_priv;

	if (tc4x_bank->probed)
		return ERROR_OK;

	return tc4x_eflash_probe(bank);
}

static inline int aurix_eflash_reset_to_read(struct flash_bank *bank)
{
	struct ocmts *ocmts = target_to_tricore(bank->target)->ocmts;
	struct aurix_eflash_bank *aurix_bank = bank->driver_priv;

	return ocmts_io_write_u32(ocmts, aurix_bank->cmd_addr + 0x5554, reset_to_read_key);
}

static inline int aurix_eflash_clear_status(struct flash_bank *bank)
{
	struct ocmts *ocmts = target_to_tricore(bank->target)->ocmts;
	struct aurix_eflash_bank *aurix_bank = bank->driver_priv;

	return ocmts_io_write_u32(ocmts, aurix_bank->cmd_addr + 0x5554, clear_status_key);
}

static inline void aurix_eflash_get_error_string_tc4x(uint32_t flash_err, char *err_str)
{
	if (flash_err & (1 << 0))
		strcat(err_str, " SRI bus address error");
	if (flash_err & (1 << 1))
		strcat(err_str, " Command sequence error");
	if (flash_err & (1 << 2))
		strcat(err_str, " Protection error");
	if (flash_err & (1 << 4))
		strcat(err_str, " Abort error");
	if (flash_err & (1 << 5))
		strcat(err_str, " Clear error");
	if (flash_err & (1 << 6))
		strcat(err_str, " Program verify error");
	if (flash_err & (1 << 7))
		strcat(err_str, " Erase verify error");
	if (flash_err & (1 << 8))
		strcat(err_str, " Flash operation error");
	if (flash_err & (1 << 29))
		strcat(err_str, " No page mode entry");
	if (flash_err & (1 << 30))
		strcat(err_str, " Flash operation timeout");
	if (flash_err & (1 << 31))
		strcat(err_str, " Flash busy");
}

static inline void aurix_eflash_get_error_string_tc3x(uint32_t flash_err, char *err_str)
{
	if (flash_err & (1 << 0))
		strcat(err_str, " Flash Operation Error");
	if (flash_err & (1 << 1))
		strcat(err_str, " Command sequence error");
	if (flash_err & (1 << 2))
		strcat(err_str, " Protection error");
	if (flash_err & (1 << 3))
		strcat(err_str, " Program Verify Error");
	if (flash_err & (1 << 4))
		strcat(err_str, " Erase Verify Error");
	if (flash_err & (1 << 5))
		strcat(err_str, " SRI Bus Address ECC Error");
	if (flash_err & (1 << 6))
		strcat(err_str, " Original Error");
	if (flash_err & (1 << 30))
		strcat(err_str, " Flash operation timeout");
	if (flash_err & (1 << 31))
		strcat(err_str, " Flash busy");
}

static inline void aurix_eflash_get_error_string(struct aurix_eflash_bank *aurix_bank, uint32_t flash_err, char *err_str)
{
	if (aurix_bank->tc4x) {
		aurix_eflash_get_error_string_tc4x(flash_err, err_str);
	} else {
		aurix_eflash_get_error_string_tc3x(flash_err, err_str);
	}
}

static inline int aurix_eflash_handle_error(struct flash_bank *bank, int64_t timeout_ms,
											bool *verify_error)
{
	struct aurix_eflash_bank *aurix_bank = bank->driver_priv;
	struct ocmts *ocmts = target_to_tricore(bank->target)->ocmts;
	uint32_t flash_err = 0;
	uint32_t flash_busy = 0xFFFFFFFF;
	int ret = ERROR_OK;
	int64_t start_time = timeval_ms();
	bool timeout_occurred = true;

	while (start_time + timeout_ms > timeval_ms()) {
		ret = ocmts_queue_read_u32(ocmts, aurix_bank->reg_addr + aurix_bank->err_offset, &flash_err);
		if (ret)
			goto status_err;
		ret = ocmts_queue_read_u32(ocmts, aurix_bank->reg_addr + aurix_bank->sts_offset, &flash_busy);
		if (ret)
			goto status_err;
		ret = ocmts_run(ocmts);

status_err:
		if (ret) {
			LOG_ERROR("Failed to read flash operation status");
			return ERROR_FLASH_OPERATION_FAILED;
		}
		if (aurix_bank->tc4x) {
			if ((flash_busy & ((1 << 31) | (1 << 30))) == 0xC0000000 &&
					!(flash_busy & (1 << aurix_bank->params.busy_bit))) {
				timeout_occurred = false;
				break;
			}
		} else {
			if (!(flash_busy & (1 << aurix_bank->params.busy_bit))) {
				timeout_occurred = false;
				break;
			}
		}
		usleep(100);
	}

	if (timeout_occurred) {
		flash_err |= (1 << 30);
	}

	if (flash_err) {
		uint32_t verify_bit = aurix_bank->tc4x ? 7 : 4;
		uint32_t faltal_mask = aurix_bank->tc4x ? ((1 << 8) | (1 << 5)) : (1 << 0);
		if (verify_error) {
			if (flash_err & (1 << verify_bit)) {
				*verify_error = true;
				ret = aurix_eflash_clear_status(bank);
				if (ret) {
					LOG_ERROR("Failed to clear flash status. Please reset device to continue");
				}
				return ERROR_OK;
			}
		}
		char err_str[256] = {0};
		aurix_eflash_get_error_string(aurix_bank, flash_err, err_str);
		LOG_ERROR("Flash operation failed with error:%s", err_str);
		if (flash_err & faltal_mask) {
			LOG_ERROR("Critical flash error. Please reset device to continue");
		} else if (flash_err & ((1 << 1) | (1 << 2))) {
			ret = aurix_eflash_reset_to_read(bank);
		} else {
			ret = aurix_eflash_clear_status(bank);
		}
		if (ret) {
			LOG_ERROR("Failed to clear flash status. Please reset device to continue");
		}
		ret = ERROR_FLASH_OPERATION_FAILED;
	} else {
		if (verify_error)
			*verify_error = false;
	}

	return ret;
}

static inline int aurix_eflash_check_busy(struct flash_bank *bank)
{
	struct aurix_eflash_bank *aurix_bank = bank->driver_priv;
	struct ocmts *ocmts = target_to_tricore(bank->target)->ocmts;
	uint32_t flash_sts;
	int ret = ERROR_OK;

	ret = ocmts_io_read_u32(ocmts, aurix_bank->reg_addr + aurix_bank->sts_offset, &flash_sts);
	if (ret) {
		LOG_ERROR("Failed to read flash status");
		return ERROR_FLASH_OPERATION_FAILED;
	}
	if (flash_sts & (1 << aurix_bank->params.busy_bit)) {
		LOG_ERROR("Flash is busy with another operation.");
		return ERROR_FLASH_BUSY;
	}
	return ERROR_OK;
}

static inline int aurix_eflash_enter_page_mode(struct flash_bank *bank)
{
	struct aurix_eflash_bank *aurix_bank = bank->driver_priv;
	struct ocmts *ocmts = target_to_tricore(bank->target)->ocmts;
	uint32_t flash_sts;
	uint32_t flash_err;

	uint32_t page_mode_key = aurix_bank->type == AURIX_EFLASH_PFLASH ? page_mode_p_key : page_mode_d_key;
	int ret = ocmts_io_write_u32(ocmts, aurix_bank->cmd_addr + 0x5554, page_mode_key);
	if (ret)
		goto err;
	ret = ocmts_queue_read_u32(ocmts, aurix_bank->reg_addr + aurix_bank->err_offset, &flash_err);
	if (ret)
		goto err;
	ret = ocmts_queue_read_u32(ocmts, aurix_bank->reg_addr + aurix_bank->sts_offset, &flash_sts);
	if (ret)
		goto err;
	ret = ocmts_run(ocmts);
	if (ret)
		goto err;
	if (!(flash_sts & (1 << aurix_bank->params.page_bit))) {
		flash_err |= (1 << 29);
	}
	if (flash_err) {
		char err_str[256] = {0};
		aurix_eflash_get_error_string(aurix_bank, flash_err, err_str);
		LOG_ERROR("Failed to enter page mode with error: %s", err_str);
		ret = aurix_eflash_reset_to_read(bank);
		if (ret) {
			LOG_WARNING("Failed to reset flash to read mode. Please reset device to continue");
		}
		return ERROR_FLASH_OPERATION_FAILED;
	}
	return ERROR_OK;
err:
	LOG_ERROR("Failed to execute enter page sequence");
	return ret;
}

int aurix_eflash_erase(struct flash_bank *bank, unsigned int first, unsigned int last)
{
	struct aurix_eflash_bank *aurix_bank = bank->driver_priv;
	struct ocmts *ocmts = target_to_tricore(bank->target)->ocmts;
	int ret;

	if (aurix_bank->type == AURIX_EFLASH_UCB && aurix_bank->ucb_unlocked == false) {
		LOG_WARNING("UCB bank has not been unlocked. Skipping operation.");
		return ERROR_OK;
	}

	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	ret = aurix_eflash_check_busy(bank);
	if (ret) {
		LOG_ERROR("Flash erase failed: flash is busy");
		return ret;
	}

	ret = aurix_eflash_clear_status(bank);
	if (ret) {
		LOG_ERROR("Flash erase failed: failed to clear flash status");
		return ret;
	}

	while (first <= last) {
		uint32_t sector_count;
		if (aurix_bank->tc4x) {
			/* Limit to logical sector erase count. */
			sector_count = MIN(aurix_bank->params.num_erase_sectors, last - first + 1);
		} else {
			/* Align sector count to physical sector boundary */
			uint32_t sectors_to_boundary =
				MIN(last - first + 1, aurix_bank->params.phys_sector_size / aurix_bank->params.sector_size -
									  (first % (aurix_bank->params.phys_sector_size / aurix_bank->params.sector_size)));
			/* Limit to logical sector erase count. */
			sector_count = MIN(aurix_bank->params.num_erase_sectors, MIN(last - first + 1, sectors_to_boundary));
		}

		/* Put address always in segment 0xA */
		uint32_t addr = (~0xF0000000 & (bank->base + bank->sectors[first].offset)) + 0xA0000000;

		ret = ocmts_queue_write_u32(ocmts, aurix_bank->cmd_addr + 0xAA50, &addr);
		if (ret)
			goto sequence_err;
		ret = ocmts_queue_write_u32(ocmts, aurix_bank->cmd_addr + 0xAA58, &sector_count);
		if (ret)
			goto sequence_err;
		ret = ocmts_queue_write_u32(ocmts, aurix_bank->cmd_addr + 0xAAA8, &(uint32_t){0x80});
		if (ret)
			goto sequence_err;
		ret = ocmts_queue_write_u32(ocmts, aurix_bank->cmd_addr + 0xAAA8, &(uint32_t){0x50});
		if (ret)
			goto sequence_err;
		ret = ocmts_run(ocmts);
		/* Hint: User manual requires Wait for 2*1/fFSI ns (DFlash) or 3*1/fFSI + 8*1/fSRI ns (PFlash)
		 * The OCMTS delay for the next instruction is sufficient. */
sequence_err:
		if (ret) {
			ret = aurix_eflash_reset_to_read(bank);
			if (ret) {
				LOG_WARNING("Failed to reset flash to read mode. Please reset device to continue");
			}
			LOG_ERROR("Flash erase failed: failed to execute erase sequence address: 0x%08x, "
					  "sector count: %u",
					  addr, sector_count);
			return ERROR_FLASH_OPERATION_FAILED;
		}

		ret = aurix_eflash_handle_error(bank, aurix_bank->params.erase_timeout_ms, NULL);
		if (ret) {
			LOG_ERROR("Flash erase failed: operation failed at address 0x%08" PRIx64,
				bank->base + bank->sectors[first].offset);
				return ret;
		}

		first += sector_count;
	}

	return ERROR_OK;
}

int aurix_eflash_erase_check(struct flash_bank *bank)
{
	struct aurix_eflash_bank *aurix_bank = bank->driver_priv;
	struct ocmts *ocmts = target_to_tricore(bank->target)->ocmts;
	int ret;
	size_t sector;

	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	ret = aurix_eflash_check_busy(bank);
	if (ret) {
		LOG_ERROR("Flash verify failed: flash is busy");
		return ret;
	}

	ret = aurix_eflash_clear_status(bank);
	if (ret) {
		LOG_ERROR("Flash verify failed: failed to clear flash status");
		return ret;
	}

	for (sector = 0; sector < bank->num_sectors; sector += aurix_bank->params.num_erase_sectors) {

		/* Put address always in segment 0xA */
		uint32_t addr = (~0xF0000000 & (bank->base + bank->sectors[sector].offset)) + 0xA0000000;
		uint32_t verify_sectors = aurix_bank->params.num_erase_sectors;

		ret = ocmts_queue_write_u32(ocmts, aurix_bank->cmd_addr + 0xAA50, &addr);
		if (ret)
			goto sequence_err;
		ret = ocmts_queue_write_u32(ocmts, aurix_bank->cmd_addr + 0xAA58, &verify_sectors);
		if (ret)
			goto sequence_err;
		ret = ocmts_queue_write_u32(ocmts, aurix_bank->cmd_addr + 0xAAA8, &(uint32_t){0x80});
		if (ret)
			goto sequence_err;
		ret = ocmts_queue_write_u32(ocmts, aurix_bank->cmd_addr + 0xAAA8, &(uint32_t){0x5F});
		if (ret)
			goto sequence_err;
		ret = ocmts_run(ocmts);
sequence_err:
		if (ret) {
			ret = aurix_eflash_reset_to_read(bank);
			if (ret) {
				LOG_WARNING("Failed to reset flash to read mode. Please reset device to continue");
			}
			LOG_ERROR("Flash verify failed: failed to execute erase sequence address: 0x%08x, "
					  "sector count: %u",
					  addr, aurix_bank->params.num_erase_sectors);
			return ERROR_FLASH_OPERATION_FAILED;
		}

		bool verify_error = false;
		ret = aurix_eflash_handle_error(bank, aurix_bank->params.erase_timeout_ms, &verify_error);
		if (ret) {
			LOG_ERROR("Flash verify failed: operation failed at address 0x%08" PRIx64,
					  bank->base + bank->sectors[sector].offset);
			return ret;
		}

		uint32_t i;
		if (!verify_error) {
			for (i = sector; i < sector + verify_sectors && i < bank->num_sectors; i++) {
				bank->sectors[i].is_erased = 1;
			}
		} else {
			uint8_t failed_sector = 0;
			if (aurix_bank->tc4x) {
				ret = ocmts_io_read_u8(ocmts, aurix_bank->fsi_addr + (aurix_bank->tc4x ? 0x1 : 0x4), &failed_sector);
				if (ret) {
					LOG_ERROR("Failed to read failed sector address");
					return ERROR_FLASH_OPERATION_FAILED;
				}
			}

			for (i = sector + failed_sector; i < sector + verify_sectors && i < bank->num_sectors; i++) {
				/* Put address always in segment 0xA */
				addr = (~0xF0000000 & (bank->base + bank->sectors[i].offset)) + 0xA0000000;
				uint32_t single_sector = 1;
				ret = ocmts_queue_write_u32(ocmts, aurix_bank->cmd_addr + 0xAA50, &addr);
				if (ret)
					goto sequence_err2;
				ret = ocmts_queue_write_u32(ocmts, aurix_bank->cmd_addr + 0xAA58, &single_sector);
				if (ret)
					goto sequence_err2;
				ret = ocmts_queue_write_u32(ocmts, aurix_bank->cmd_addr + 0xAAA8, &(uint32_t){0x80});
				if (ret)
					goto sequence_err2;
				ret = ocmts_queue_write_u32(ocmts, aurix_bank->cmd_addr + 0xAAA8, &(uint32_t){0x5F});
				if (ret)
					goto sequence_err2;
				ret = ocmts_run(ocmts);
sequence_err2:
				if (ret) {
					ret = aurix_eflash_reset_to_read(bank);
					if (ret) {
						LOG_WARNING("Failed to reset flash to read mode. Please reset device to continue");
					}
					LOG_ERROR("Flash verify failed: failed to execute erase sequence address: 0x%08x, "
							  "sector count: %u",
							  addr, aurix_bank->params.num_erase_sectors);
					return ERROR_FLASH_OPERATION_FAILED;
				}

				ret =
					aurix_eflash_handle_error(bank, aurix_bank->params.erase_timeout_ms, &verify_error);
				if (ret) {
					LOG_ERROR("Flash verify failed: operation failed at address 0x%08" PRIx64,
							  bank->base + bank->sectors[sector].offset);
					return ret;
				}
				bank->sectors[i].is_erased = verify_error ? 0 : 1;
			}
		}
	}
	return ERROR_OK;
}

static int aurix_eflash_get_code(struct aurix_eflash_bank *aurix_bank, const uint8_t **write_code, size_t *write_code_size) {
	static const uint8_t tc4x_pflash_write_code[] = {
	#include "../../../contrib/loaders/flash/aurix/tc4x-pflash-program.inc"
	};

	static const uint8_t tc4x_dflash_write_code[] = {
	#include "../../../contrib/loaders/flash/aurix/tc4x-dflash-program.inc"
	};

	static const uint8_t tc4x_pflashcs_write_code[] = {
	#include "../../../contrib/loaders/flash/aurix/tc4x-pflashcs-program.inc"
	};

	static const uint8_t tc4x_dflashcs_write_code[] = {
	#include "../../../contrib/loaders/flash/aurix/tc4x-dflashcs-program.inc"
	};

	static const uint8_t tc3x_pflash_write_code[] = {
	#include "../../../contrib/loaders/flash/aurix/tc3x-pflash-program.inc"
	};

	static const uint8_t tc3x_dflash_write_code[] = {
	#include "../../../contrib/loaders/flash/aurix/tc3x-dflash-program.inc"
	};

	if (aurix_bank->tc4x) {
		if (aurix_bank->secure) {
			switch (aurix_bank->type) {
				case AURIX_EFLASH_PFLASH:
					*write_code = tc4x_pflashcs_write_code;
					*write_code_size = sizeof(tc4x_pflashcs_write_code);
					break;
				case AURIX_EFLASH_DFLASH:
				case AURIX_EFLASH_UCB:
					*write_code = tc4x_dflashcs_write_code;
					*write_code_size = sizeof(tc4x_dflashcs_write_code);
					break;
				default:
					return ERROR_FLASH_BANK_INVALID;
			}
		} else {
			switch (aurix_bank->type) {
				case AURIX_EFLASH_PFLASH:
					*write_code = tc4x_pflash_write_code;
					*write_code_size = sizeof(tc4x_pflash_write_code);
					break;
				case AURIX_EFLASH_DFLASH:
				case AURIX_EFLASH_UCB:
					*write_code = tc4x_dflash_write_code;
					*write_code_size = sizeof(tc4x_dflash_write_code);
					break;
				default:
					return ERROR_FLASH_BANK_INVALID;
			}
		}

	} else {
		switch (aurix_bank->type) {
			case AURIX_EFLASH_PFLASH:
				*write_code = tc3x_pflash_write_code;
				*write_code_size = sizeof(tc3x_pflash_write_code);
				break;
			case AURIX_EFLASH_DFLASH:
			case AURIX_EFLASH_UCB:
				*write_code = tc3x_dflash_write_code;
				*write_code_size = sizeof(tc3x_dflash_write_code);
				break;
			default:
				return ERROR_FLASH_BANK_INVALID;
		}
	}

	return 0;
}

/* Start a low level flash write for the specified region */
static int aurix_eflash_write_algo(struct flash_bank *bank, uint32_t address, const uint8_t *buffer, uint32_t bytes)
{
	struct aurix_eflash_bank *aurix_bank = bank->driver_priv;
	struct target *target = bank->target;
	struct working_area *source;
	struct reg_param reg_params[5];
	const uint8_t *write_code;
	size_t write_code_size;
	const uint32_t nvmaddr = (address & ~0xF0000000u) | 0xA0000000u;
	uint32_t buffer_size = 0x8000;
	int ret;

	ret = aurix_eflash_get_code(aurix_bank, &write_code, &write_code_size);
	if (ret != ERROR_OK)
		return ret;

	ret = target_write_buffer(target, 0x70100000, write_code_size, write_code);
	if (ret != ERROR_OK)
		return ret;

	/* memory buffer */
	while (target_alloc_working_area(target, buffer_size, &source) != ERROR_OK) {
		buffer_size /= 2;
		buffer_size &= ~3UL; /* Make sure it's 4 byte aligned */
		if (buffer_size <= 256) {
			LOG_WARNING("No large enough working area available, can't do block "
						"memory writes");
			return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		}
	}

	init_reg_param(&reg_params[0], "a4", 32, PARAM_OUT); /* buffer start */
	init_reg_param(&reg_params[1], "d4", 32, PARAM_OUT); /* buffer size */
	init_reg_param(&reg_params[2], "d5", 32, PARAM_OUT); /* addr */
	init_reg_param(&reg_params[3], "d6", 32, PARAM_OUT); /* size */
	init_reg_param(&reg_params[4], "d2", 32, PARAM_IN);	 /* return */

	buf_set_u32(reg_params[0].value, 0, 32, (uint32_t)source->address);
	buf_set_u32(reg_params[1].value, 0, 32, source->size);
	buf_set_u32(reg_params[2].value, 0, 32, nvmaddr);
	buf_set_u32(reg_params[3].value, 0, 32, bytes);

	ret = target_run_flash_async_algorithm(target, buffer, bytes / 8, 8, 0, NULL, ARRAY_SIZE(reg_params), reg_params,
										   source->address, source->size, 0x70100000, 0, NULL);

	if (ret == ERROR_FLASH_OPERATION_FAILED) {
		uint32_t status = buf_get_u32(reg_params[4].value, 0, 32);
		char err_str[256] = {0};
		aurix_eflash_get_error_string(aurix_bank, status, err_str);
		LOG_ERROR("Flash algorithm failed: %s", err_str);
	}
	target_free_working_area(target, source);

	destroy_reg_param(&reg_params[0]);
	destroy_reg_param(&reg_params[1]);
	destroy_reg_param(&reg_params[2]);
	destroy_reg_param(&reg_params[3]);
	destroy_reg_param(&reg_params[4]);

	return ret;
}

static int aurix_eflash_write(struct flash_bank *bank, const uint8_t *buffer, uint32_t offset, uint32_t count)
{
	struct aurix_eflash_bank *aurix_bank = bank->driver_priv;
	struct ocmts *ocmts = target_to_tricore(bank->target)->ocmts;
	uint32_t page_offset = 0;
	int ret;

	if (aurix_bank->type == AURIX_EFLASH_UCB && aurix_bank->ucb_unlocked == false) {
		LOG_WARNING("UCB bank has not been unlocked. Skipping operation.");
		return ERROR_OK;
	}

	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (offset & (aurix_bank->params.page_size - 1) || count % aurix_bank->params.page_size != 0)
		return ERROR_FLASH_DST_BREAKS_ALIGNMENT;

	if (aurix_bank->fallback_mode)
		goto fallback;

	ret = aurix_eflash_write_algo(bank, bank->base + offset, buffer, count);
	if (ret == ERROR_OK) {
		return ERROR_OK;
	} else if (ret != ERROR_TARGET_RESOURCE_NOT_AVAILABLE) {
		return ret;
	} else {
		LOG_WARNING("No enough resources to run flash write algorithm, fallback to "
					"slow flash write sequence.");
	}

fallback:
	ret = aurix_eflash_check_busy(bank);
	if (ret)
		return ret;

	while (page_offset < count) {
		const bool burst_mode =
			(page_offset % aurix_bank->params.burst_size) == 0 &&
			(count - page_offset >= aurix_bank->params.burst_size);
		const uint32_t copy_size = burst_mode ? aurix_bank->params.burst_size : aurix_bank->params.page_size;
		uint32_t i;

		ret = aurix_eflash_clear_status(bank);
		if (ret) {
			LOG_ERROR("Flash program failed: failed to clear flash status");
			return ret;
		}

		ret = aurix_eflash_enter_page_mode(bank);
		if (ret) {
			LOG_ERROR("Flash program failed: failed to enter page mode");
			return ret;
		}

		if (aurix_bank->tc4x) {
			for (i = 0; i < copy_size; i += 4) {
				ret = ocmts_queue_write_u32(ocmts, aurix_bank->cmd_addr + 0x55F4, (void *)(buffer + page_offset + i));
				if (ret)
					goto err;
			}
			/* Clear status to reset request done from load page*/
			ret = ocmts_queue_write_u32(ocmts, aurix_bank->cmd_addr + 0x5554, &clear_status_key);
			if (ret) {
				ret = aurix_eflash_reset_to_read(bank);
				if (ret) {
					LOG_ERROR("Failed to reset flash to read mode. Please reset device to continue");
				}
				LOG_ERROR("Flash program failed: failed to clear flash status");
				return ret;
			}
		} else {
			for (i = 0; i < copy_size; i += 4) {
				ret = ocmts_queue_write_u32(ocmts, aurix_bank->cmd_addr + 0x55F0 + ((i % 8) == 0 ? 0 : 4),
											(void *)(buffer + page_offset + i));
				if (ret)
					goto err;
			}
		}

		/* Put address always in segment 0xA */
		uint32_t addr = (~0xF0000000 & (bank->base + offset + page_offset)) + 0xA0000000;
		page_offset += copy_size;
		/* Execute page write sequence */
		ret = ocmts_queue_write_u32(ocmts, aurix_bank->cmd_addr + 0xAA50, &addr);
		if (ret)
			goto err;
		ret = ocmts_queue_write_u32(ocmts, aurix_bank->cmd_addr + 0xAA58, &(uint32_t){0});
		if (ret)
			goto err;
		ret = ocmts_queue_write_u32(ocmts, aurix_bank->cmd_addr + 0xAAA8, &(uint32_t){0xA0});
		if (ret)
			goto err;

		/* Check for burst sequence*/
		if (burst_mode) {
			ret = ocmts_queue_write_u32(ocmts, aurix_bank->cmd_addr + 0xAAA8, &(uint32_t){0xA6});
			if (ret)
				goto err;
		} else {
			ret = ocmts_queue_write_u32(ocmts, aurix_bank->cmd_addr + 0xAAA8, &(uint32_t){0xAA});
			if (ret)
				goto err;
		}
		ret = ocmts_run(ocmts);
err:
		if (ret) {
			ret = aurix_eflash_reset_to_read(bank);
			if (ret) {
				LOG_ERROR("Failed to reset flash to read mode. Please reset device to continue");
			}
			LOG_ERROR(
				"Flash program failed: failed to execute flash program sequence at address: 0x%08x, data size: %u",
				addr, copy_size);
			return ERROR_FLASH_OPERATION_FAILED;
		}

		if (aurix_eflash_handle_error(bank, aurix_bank->params.write_timeout_ms, NULL) != ERROR_OK) {
			LOG_ERROR("Flash program failed: operation failed at address 0x%08" PRIx64,
					  bank->base + offset + page_offset - copy_size);
			return ERROR_FLASH_OPERATION_FAILED;
		}
	}

	return ERROR_OK;
}

static int aurix_eflash_read(struct flash_bank *bank, uint8_t *buffer, uint32_t offset, uint32_t count)
{
	return target_read_buffer(bank->target, ((bank->base + offset) & ~0xF0000000) + 0xA0000000, count, buffer);
}

static void aurix_free_driver_priv(struct flash_bank *bank)
{
	free(bank->driver_priv);
}

FLASH_BANK_COMMAND_HANDLER(tc3x_flash_bank_command)
{
	struct aurix_eflash_bank *tc3x_bank;

	tc3x_bank = malloc(sizeof(struct aurix_eflash_bank));
	if (!tc3x_bank)
		return ERROR_FLASH_OPERATION_FAILED;

	/* TODO:  Complement Sensing Mode */

	tc3x_bank->type = AURIX_EFLASH_UNKNOWN;
	tc3x_bank->ucb_unlocked = false;
	tc3x_bank->fallback_mode = false;
	tc3x_bank->tc4x = false;
	tc3x_bank->secure = false;
	tc3x_bank->probed = false;
	bank->driver_priv = tc3x_bank;

	return ERROR_OK;
}

FLASH_BANK_COMMAND_HANDLER(tc4x_flash_bank_command)
{
	struct aurix_eflash_bank *tc4x_bank;

	tc4x_bank = malloc(sizeof(struct aurix_eflash_bank));
	if (!tc4x_bank)
		return ERROR_FLASH_OPERATION_FAILED;

	tc4x_bank->type = AURIX_EFLASH_UNKNOWN;
	tc4x_bank->ucb_unlocked = false;
	tc4x_bank->fallback_mode = false;
	tc4x_bank->tc4x = true;
	tc4x_bank->secure = false;
	tc4x_bank->probed = false;
	bank->driver_priv = tc4x_bank;

	return ERROR_OK;
}

COMMAND_HANDLER(aurix_eflash_set_fallback_mode_command)
{
	struct flash_bank *bank;
	struct aurix_eflash_bank *aurix_bank;

	if (CMD_ARGC < 1 || CMD_ARGC > 2) {
		return ERROR_COMMAND_ARGUMENT_INVALID;
	}

	int retval = CALL_COMMAND_HANDLER(flash_command_get_bank, 0, &bank);
	if (retval != ERROR_OK)
		return retval;
	aurix_bank = bank->driver_priv;

	if (CMD_ARGC == 2) {
		if (strcmp(CMD_ARGV[1], "on") == 0) {
			aurix_bank->fallback_mode = true;
		} else if (strcmp(CMD_ARGV[1], "off") == 0) {
			aurix_bank->fallback_mode = false;
		} else {
			LOG_ERROR("Invalid fallback mode: %s. Should be 'on' or 'off'.", CMD_ARGV[1]);
			return ERROR_COMMAND_ARGUMENT_INVALID;
		}
	} else {
		aurix_bank->fallback_mode = !aurix_bank->fallback_mode;
	}

	return ERROR_OK;
}

static const struct command_registration aurix_eflash_subcommand_handlers[] = {
	{
		.name = "fallback",
		.handler = aurix_eflash_set_fallback_mode_command,
		.mode = COMMAND_EXEC,
		.usage = "<bank> [on|off]",
		.help = "Set or toggle fallback mode for flash write. In fallback mode, the driver will use slow flash write "
				"sequence instead of flash write algorithm.",
	},
	COMMAND_REGISTRATION_DONE};

static const struct command_registration aurix_eflash_command_handlers[] = {
	{
		.name = "aurix_eflash",
		.mode = COMMAND_ANY,
		.usage = "",
		.help = "Commands for Aurix eFLASH driver",
		.chain = aurix_eflash_subcommand_handlers,
	},
	COMMAND_REGISTRATION_DONE};

const struct flash_driver tc3x_eflash = {
	.name = "tc3x_eflash",
	.flash_bank_command = tc3x_flash_bank_command,
	.commands = aurix_eflash_command_handlers,
	.probe = tc3x_eflash_probe,
	.auto_probe = tc3x_eflash_auto_probe,
	.erase = aurix_eflash_erase,
	.write = aurix_eflash_write,
	.read = aurix_eflash_read,
	.erase_check = aurix_eflash_erase_check,
	.free_driver_priv = aurix_free_driver_priv,
};

const struct flash_driver tc4x_eflash = {
	.name = "tc4x_eflash",
	.flash_bank_command = tc4x_flash_bank_command,
	.commands = aurix_eflash_command_handlers,
	.probe = tc4x_eflash_probe,
	.auto_probe = tc4x_eflash_auto_probe,
	.erase = aurix_eflash_erase,
	.write = aurix_eflash_write,
	.read = aurix_eflash_read,
	.erase_check = aurix_eflash_erase_check,
	.free_driver_priv = aurix_free_driver_priv,
};
