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
#include "jtag/jtag.h"
#include "target/algorithm.h"
#include "target/aurix/aurix_device_family.h"
#include "target/aurix/ocmts.h"
#include "target/aurix/tricore.h"
#include "target/target.h"

struct aurix_eflash_bank {
	/** Address of the command sequencer */
	target_addr_t cmd_addr;
	/** Address of the DMU registers */
	target_addr_t reg_addr;
	/** Offset of the error register for a given command sequencer */
	target_addr_t err_offset;
	/** Offset of the status register for a ginven command sequencer */
	target_addr_t sts_offset;
	/** Size of a physical sector, where earase commands cannot cross */
	uint32_t phys_sector_size;
	/** Size of a (logical) sector. (Smallest earasable unit) */
	uint16_t sector_size;
	/** Maximum number of erasable sectors */
	uint16_t num_erase_sectors;
	/** Number of bytes for a burst program operation */
	uint16_t burst_size;
	/** Number of bytes for a page program operation. (Smallest programmable
	 * unit)
	 */
	uint16_t page_size;
	/** Offset of the busy bit for the bank in the status register */
	uint8_t busy_bit;
	/** Offset of the page bit for the bank in the status register */
	uint8_t page_bit;
	/** Bank probed */
	bool probed;
	/** TC4 eflash */
	bool tc4x;
	/** DFLASH */
	bool dflash;
	/** Fallback mode for flash write */
	bool fallback_mode;
	/** Write timeout in milliseconds */
	uint64_t write_timeout_ms;
	/** Erase timeout in milliseconds */
	uint64_t erase_timeout_ms;
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
#define SCU_CHIPID_FSIZE 0x0F000000

static int tc3x_eflash_probe(struct flash_bank *bank)
{
	struct aurix_eflash_bank *tc3x_bank = bank->driver_priv;
	uint32_t flash_addr = bank->base;
	uint32_t chipid;
	int retval;

	if (tc3x_bank->probed)
		return ERROR_OK;

	if (aurix_df_check_if_tc4x(bank->target->tap->idcode)) {
		retval = target_read_u32(bank->target, UCB_CHIPID, &chipid);
		if (retval != ERROR_OK) {
			LOG_ERROR("Cannot read CHIPID register.");
			return retval;
		}

		if (((chipid & UCB_CHIPID_PROD) >> 26) != 13) {
			LOG_ERROR("CHIPID register does not match tc4x with eFLASH.");
			return ERROR_FAIL;
		}
		/* TODO: Check size */
	} else {
		retval = target_read_u32(bank->target, SCU_CHIPID, &chipid);
		if (retval != ERROR_OK) {
			LOG_ERROR("Cannot read tc3x CHIPID register.");
			return retval;
		}

		if ((chipid & SCU_CHIPID_CHTEC) != 0x80) {
			LOG_ERROR("CHIPID register does not match tc3x.");
			return ERROR_FAIL;
		}
		/* TODO: Check size  */
	}

	bank->minimal_write_gap = FLASH_WRITE_GAP_SECTOR;
	bank->write_start_alignment = tc3x_bank->burst_size;
	bank->write_end_alignment = tc3x_bank->burst_size;
	bank->num_sectors = bank->size / tc3x_bank->sector_size;
	bank->sectors = calloc(bank->num_sectors, sizeof(struct flash_sector));
	for (unsigned int i = 0; i < bank->num_sectors; i++) {
		bank->sectors[i].size = tc3x_bank->sector_size;
		bank->sectors[i].offset = flash_addr - bank->base;
		flash_addr += tc3x_bank->sector_size;
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

	if (((chipid & UCB_CHIPID_PROD) >> 26) != 13) {
		LOG_ERROR("CHIPID register does not match tc4x with eFLASH.");
		return ERROR_FAIL;
	}
	/* TODO: Check size */

	bank->minimal_write_gap = FLASH_WRITE_GAP_SECTOR;
	bank->write_start_alignment = tc4x_bank->burst_size;
	bank->write_end_alignment = tc4x_bank->burst_size;
	bank->num_sectors = bank->size / tc4x_bank->sector_size;
	bank->sectors = calloc(bank->num_sectors, sizeof(struct flash_sector));
	for (unsigned int i = 0; i < bank->num_sectors; i++) {
		bank->sectors[i].size = tc4x_bank->sector_size;
		bank->sectors[i].offset = flash_addr - bank->base;
		flash_addr += tc4x_bank->sector_size;
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

static inline void aurix_eflash_get_error_string(uint32_t flash_err, char *err_str)
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

static inline int aurix_eflash_handle_error(struct flash_bank *bank, uint32_t sts_bit, int64_t timeout_ms)
{
	struct aurix_eflash_bank *tc3x_bank = bank->driver_priv;
	struct ocmts *ocmts = target_to_tricore(bank->target)->ocmts;
	uint32_t flash_err = 0;
	uint32_t flash_busy = 0xFFFFFFFF;
	int ret = ERROR_OK;
	int64_t start_time = timeval_ms();
	bool timeout_occurred = true;

	while (start_time + timeout_ms > timeval_ms()) {
		ret = ocmts_queue_read_u32(ocmts, tc3x_bank->reg_addr + tc3x_bank->err_offset, &flash_err);
		if (ret)
			goto status_err;
		ret = ocmts_queue_read_u32(ocmts, tc3x_bank->reg_addr + tc3x_bank->sts_offset, &flash_busy);
		if (ret)
			goto status_err;
		ret = ocmts_run(ocmts);

status_err:
		if (ret) {
			LOG_ERROR("Failed to read flash operation status");
			return ERROR_FLASH_OPERATION_FAILED;
		}
		if (flash_err) {
			timeout_occurred = false;
			break;
		}
		if (tc3x_bank->tc4x) {
			if (flash_busy & (1 << 31)) {
				timeout_occurred = false;
			}
		} else {
			if (!(flash_busy & (1 << sts_bit))) {
				timeout_occurred = false;
			}
		}
		usleep(100);
	}

	if (timeout_occurred) {
		flash_err |= (1 << 30);
	}

	if (flash_err) {
		char err_str[256] = {0};
		aurix_eflash_get_error_string(flash_err, err_str);
		LOG_ERROR("Flash operation failed with error:%s", err_str);
		if (flash_err & ((1 << 5) | (1 << 8))) {
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
	if (flash_sts & (1 << aurix_bank->busy_bit)) {
		LOG_ERROR("Flash is busy with another operation.");
		return ERROR_FLASH_BUSY;
	}
	return ERROR_OK;
}

static inline int aurix_eflash_enter_page_mode(struct flash_bank *bank, bool dflash)
{
	struct aurix_eflash_bank *aurix_bank = bank->driver_priv;
	struct ocmts *ocmts = target_to_tricore(bank->target)->ocmts;
	uint32_t flash_sts;
	uint32_t flash_err;

	uint32_t page_mode_key = dflash ? page_mode_d_key : page_mode_p_key;
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
	if (!(flash_sts & (1 << aurix_bank->page_bit))) {
		flash_err |= (1 << 29);
	}
	if (flash_err) {
		char err_str[256] = {0};
		aurix_eflash_get_error_string(flash_err, err_str);
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
			sector_count = MIN(aurix_bank->num_erase_sectors, last - first + 1);
		} else {
			/* Align sector count to physical sector boundary */
			uint32_t sectors_to_boundary =
				MIN(last - first + 1, aurix_bank->phys_sector_size / aurix_bank->sector_size -
										  (first % (aurix_bank->phys_sector_size / aurix_bank->sector_size)));
			/* Limit to logical sector erase count. */
			sector_count = MIN(aurix_bank->num_erase_sectors, MIN(last - first + 1, sectors_to_boundary));
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
		first += sector_count;

		ret = aurix_eflash_handle_error(bank, aurix_bank->busy_bit, aurix_bank->erase_timeout_ms);
		if (ret) {
			LOG_ERROR("Flash erase failed: operation failed at address 0x%08" PRIx64,
					  bank->base + bank->sectors[first].offset);
			return ret;
		}
	}

	return ERROR_OK;
}

static const uint8_t tc4x_flash_write_code[] = {
#include "../../../contrib/loaders/flash/aurix/tc4x-program.inc"
};

static const uint8_t tc3x_flash_write_code[] = {
#include "../../../contrib/loaders/flash/aurix/tc3x-program.inc"
};

/* Start a low level flash write for the specified region */
static int aurix_eflash_write_algo(struct flash_bank *bank, uint32_t address, const uint8_t *buffer, uint32_t bytes)
{
	struct aurix_eflash_bank *aurix_bank = bank->driver_priv;
	struct target *target = bank->target;
	struct reg_param reg_params[5];
	uint32_t buffer_size = 0x8000;
	int ret;
	struct working_area *source;

	if (aurix_bank->tc4x) {
		ret = target_write_buffer(target, 0x70100000, sizeof(tc4x_flash_write_code), tc4x_flash_write_code);
	} else {
		ret = target_write_buffer(target, 0x70100000, sizeof(tc3x_flash_write_code), tc3x_flash_write_code);
	}
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
	buf_set_u32(reg_params[2].value, 0, 32, address);
	buf_set_u32(reg_params[3].value, 0, 32, bytes);

	ret = target_run_flash_async_algorithm(target, buffer, bytes / 8, 8, 0, NULL, ARRAY_SIZE(reg_params), reg_params,
										   source->address, source->size, 0x70100000, 0, NULL);

	if (ret == ERROR_FLASH_OPERATION_FAILED) {
		uint32_t status = buf_get_u32(reg_params[4].value, 0, 32);
		char err_str[256] = {0};
		aurix_eflash_get_error_string(status, err_str);
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

	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (offset & ~(aurix_bank->page_size - 1) || count % aurix_bank->page_size != 0)
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
			(page_offset % aurix_bank->burst_size) == 0 && (count - page_offset >= aurix_bank->burst_size);
		const uint32_t copy_size = burst_mode ? aurix_bank->burst_size : aurix_bank->page_size;
		uint32_t i;

		ret = aurix_eflash_clear_status(bank);
		if (ret) {
			LOG_ERROR("Flash program failed: failed to clear flash status");
			return ret;
		}

		ret = aurix_eflash_enter_page_mode(bank, aurix_bank->dflash);
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

		if (aurix_eflash_handle_error(bank, aurix_bank->busy_bit, aurix_bank->write_timeout_ms) != ERROR_OK) {
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
	uint32_t bank_base = bank->base & 0x0FFFFFFF;
	bool is_flash_segment = (bank->base & 0xF0000000) == 0x80000000 || (bank->base & 0xF0000000) == 0xA0000000;
	bool is_pflash = is_flash_segment && bank_base < 0x01000000;
	bool is_ucb = bank->base == 0xAF400000;
	bool is_dflash0 = bank->base == 0xAF000000 || bank->base == 0xAF400000;
	bool is_dflash1 = bank->base == 0xAFC00000;
	bool is_dflash = is_dflash0 || is_dflash1;

	if (is_pflash) {
		if (bank->size != 1 * 1024 * 1024 && bank->size != 2 * 1024 * 1024 && bank->size != 3 * 1024 * 1024) {
			LOG_ERROR("Invalid pflash bank size. Size should be 1MB,"
					  " 2MB or 3MB.");
			return ERROR_FLASH_BANK_INVALID;
		}
		if (bank->base % (1 * 1024 * 1024) != 0) {
			LOG_ERROR("Invalid pflash bank base address. Base should be "
					  "aligned to 1MB.");
			return ERROR_FLASH_BANK_INVALID;
		}
	} else if (is_ucb) {
		if (bank->size != 24 * 1024) {
			LOG_ERROR("Invalid UCB bank size. Size should be 24KB.");
			return ERROR_FLASH_BANK_INVALID;
		}
	} else if (is_dflash0) {
		if (bank->size != 1 * 1024 * 1024) {
			LOG_ERROR("Invalid dflash0 bank size. Size should be 1MB.");
			return ERROR_FLASH_BANK_INVALID;
		}
	} else if (is_dflash1) {
		if (bank->size != 128 * 1024) {
			LOG_ERROR("Invalid dflash1 bank size. Size should be 128KB.");
			return ERROR_FLASH_BANK_INVALID;
		}
	} else {
		LOG_ERROR("Invalid flash bank base address: 0x%08" PRIx64, bank->base);
		return ERROR_FLASH_BANK_INVALID;
	}

	tc3x_bank = malloc(sizeof(struct aurix_eflash_bank));
	if (!tc3x_bank)
		return ERROR_FLASH_OPERATION_FAILED;

	/* cmd_reg and offsets might be changed during probe, if HSM sequencer is
	 * required */
	tc3x_bank->cmd_addr = 0xAF000000;
	tc3x_bank->reg_addr = 0xF8040000;
	tc3x_bank->sts_offset = 0x10;
	tc3x_bank->err_offset = 0x34;
	tc3x_bank->busy_bit = is_pflash ? bank->bank_number + 2 : is_dflash0 ? 0 : 1;
	tc3x_bank->page_bit = is_dflash ? 20 : 21;
	tc3x_bank->page_size = is_dflash ? 8 : 32;
	tc3x_bank->burst_size = is_dflash ? 32 : 256;
	tc3x_bank->sector_size = is_ucb ? 512 : is_dflash ? 4 * 1024 : 16 * 1024;
	tc3x_bank->phys_sector_size = is_ucb	   ? 24 * 1024
								  : is_dflash0 ? 1 * 1024 * 1024
								  : is_dflash1 ? 128 * 1024
											   : 1024 * 1024;
	tc3x_bank->num_erase_sectors = is_ucb	   ? 1
								   : is_dflash ? 256 * 1024 / tc3x_bank->sector_size
											   : 512 * 1024 / tc3x_bank->sector_size;
	/* TODO:  Complement Sensing Mode */

	tc3x_bank->write_timeout_ms = 10;
	tc3x_bank->erase_timeout_ms = is_ucb ? 200 : is_dflash ? 500 : 400;

	tc3x_bank->dflash = is_dflash;
	tc3x_bank->fallback_mode = false;
	tc3x_bank->tc4x = false;
	tc3x_bank->probed = false;
	bank->driver_priv = tc3x_bank;

	return ERROR_OK;
}

FLASH_BANK_COMMAND_HANDLER(tc4x_flash_bank_command)
{
	struct aurix_eflash_bank *tc4x_bank;
	uint32_t bank_base = bank->base & 0x0FFFFFFF;
	bool is_flash_segment = (bank->base & 0xF0000000) == 0x80000000 || (bank->base & 0xF0000000) == 0xA0000000;
	bool is_pflash0 = is_flash_segment && bank_base < 0x01400000;
	bool is_pflash1 = is_flash_segment && bank_base == 0x04000000;
	bool is_ucb0 = bank->base == 0xAE400000;
	bool is_ucb1 = bank->base == 0xAEC00000;
	bool is_dflash0 = bank->base == 0xAE000000 || bank->base == 0xAE400000;
	bool is_dflash1 = bank->base == 0xAE800000 || bank->base == 0xAEC00000;
	bool is_dflash = is_dflash0 || is_dflash1;
	bool is_ucb = is_ucb0 || is_ucb1;

	if (is_pflash0) {
		if (bank->size != 1 * 1024 * 1024 && bank->size != 2 * 1024 * 1024) {
			LOG_ERROR("Invalid pflash0 bank size. Size should be 1MB or 2MB.");
			return ERROR_FLASH_BANK_INVALID;
		}
		if (bank->base % (bank->size) != 0) {
			LOG_ERROR("Invalid pflash0 bank base address. Base should be "
					  "aligned to bank size.");
			return ERROR_FLASH_BANK_INVALID;
		}
	} else if (is_pflash1) {
		if (bank->size != 1 * 1024 * 1024) {
			LOG_ERROR("Invalid pflash1 bank size. Size should be 1MB.");
			return ERROR_FLASH_BANK_INVALID;
		}
	} else if (is_ucb0) {
		if (bank->size != 80 * 1024) {
			LOG_ERROR("Invalid UCB bank size. Size should be 80KB.");
			return ERROR_FLASH_BANK_INVALID;
		}
	} else if (is_ucb1) {
		if (bank->size != 52 * 1024) {
			LOG_ERROR("Invalid UCB bank size. Size should be 52KB.");
			return ERROR_FLASH_BANK_INVALID;
		}
	} else if (is_dflash0) {
		if (bank->size != 1024 * 1024 && bank->size != 512 * 1024 && bank->size != 128 * 1024) {
			LOG_ERROR("Invalid dflash bank size. Size should be 128KB, 512KB "
					  "or 1MB.");
			return ERROR_FLASH_BANK_INVALID;
		}
	} else if (is_dflash1) {
		if (bank->size != 128 * 1024) {
			LOG_ERROR("Invalid dflash1 bank size. Size should be 128KB.");
			return ERROR_FLASH_BANK_INVALID;
		}
	} else {
		LOG_ERROR("Invalid flash bank base address: 0x%08" PRIx64, bank->base);
		return ERROR_FLASH_BANK_INVALID;
	}

	tc4x_bank = malloc(sizeof(struct aurix_eflash_bank));
	if (!tc4x_bank)
		return ERROR_FLASH_OPERATION_FAILED;

	/* Select the command sequence interface based on bank addresses */
	if (is_pflash1 || is_dflash1) {
		tc4x_bank->cmd_addr = 0xF80C0000;
		tc4x_bank->sts_offset = 0x84;
		tc4x_bank->err_offset = 0x90;
	} else {
		tc4x_bank->cmd_addr = 0xF8080000;
		tc4x_bank->sts_offset = 0x4;
		tc4x_bank->err_offset = 0x10;
	}
	tc4x_bank->reg_addr = 0xF8040000;
	tc4x_bank->busy_bit = is_pflash0 ? bank->bank_number : is_pflash1 ? 18 : is_dflash0 ? 16 : 17;
	tc4x_bank->page_bit = is_dflash ? 24 : 25;
	tc4x_bank->page_size = is_dflash ? 8 : 32;
	tc4x_bank->burst_size = is_dflash ? 32 : 512;
	tc4x_bank->sector_size = is_ucb ? 512 : is_dflash ? 2 * 1024 : 16 * 1024;
	tc4x_bank->phys_sector_size = is_ucb0	  ? 80 * 1024
								  : is_ucb1	  ? 52 * 1024
								  : is_dflash ? (bank->size == 128 * 1024 ? 64 : 128 * 1024)
											  : 512 * 1024;
	tc4x_bank->num_erase_sectors = is_ucb	   ? 1
								   : is_dflash ? 256 * 1024 / tc4x_bank->sector_size
											   : 512 * 1024 / tc4x_bank->sector_size;

	tc4x_bank->write_timeout_ms = 10;
	tc4x_bank->erase_timeout_ms = is_ucb ? 200 : is_dflash ? 500 : 400;

	tc4x_bank->dflash = is_dflash;
	tc4x_bank->fallback_mode = false;
	tc4x_bank->tc4x = true;
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
	.free_driver_priv = aurix_free_driver_priv,
};
