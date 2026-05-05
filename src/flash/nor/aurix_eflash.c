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

#include <flash/common.h>
#include <flash/nor/core.h>
#include <flash/nor/driver.h>
#include <helper/log.h>
#include <helper/time_support.h>
#include <helper/types.h>
#include <jtag/jtag.h>
#include <target/aurix/aurix_device_family.h>
#include <target/aurix/ocmts.h>
#include <target/aurix/tricore.h>
#include <target/target.h>

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
	/** Write timeout in milliseconds */
	uint64_t write_timeout_ms;
	/** Erase timeout in milliseconds */
	uint64_t erase_timeout_ms;
};

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

static inline void aurix_eflash_reset_to_read(struct flash_bank *bank)
{
	struct ocmts *ocmts = target_to_tricore(bank->target)->ocmts;
	struct aurix_eflash_bank *aurix_bank = bank->driver_priv;

	int ret = ocmts_io_write_u32(ocmts, aurix_bank->cmd_addr + 0x5554, 0xF0);
	if (ret) {
		LOG_ERROR("Failed to execute reset to read. Please reset "
				  "device to continue");
	}
}

static inline int aurix_eflash_handle_error(struct flash_bank *bank, uint32_t sts_bit, int64_t timeout_ms)
{
	struct aurix_eflash_bank *tc3x_bank = bank->driver_priv;
	struct ocmts *ocmts = target_to_tricore(bank->target)->ocmts;
	uint32_t flash_err = 0;
	uint32_t flash_busy = 0xFFFFFFFF;
	int ret = ERROR_OK;
	int64_t start_time = timeval_ms();

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
			aurix_eflash_reset_to_read(bank);
			return ERROR_FLASH_OPERATION_FAILED;
		}
		if (flash_err || !(flash_busy & (1 << sts_bit)))
			break;
		usleep(100);
	}

	if (flash_busy & (1 << sts_bit)) {
		LOG_ERROR("Flash operation timed out.");
		return ERROR_FLASH_BUSY;
	}

	if (flash_err) {
		char err_str[256];
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
		LOG_ERROR("Flash operation failed with error: %s", err_str);
		if (flash_err & ((1 << 5) | (1 << 8)))
			LOG_ERROR("Critical flash error. Please reset device to continue");
		else
			aurix_eflash_reset_to_read(bank);
		return ERROR_FLASH_OPERATION_FAILED;
	}

	return ERROR_OK;
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

static inline int aurix_eflash_enter_page_mode(struct flash_bank *bank)
{
	struct aurix_eflash_bank *aurix_bank = bank->driver_priv;
	struct ocmts *ocmts = target_to_tricore(bank->target)->ocmts;
	uint32_t flash_sts;

	int ret = ocmts_io_write_u32(ocmts, aurix_bank->cmd_addr + 0x5554, 0x50);
	if (ret) {
		LOG_ERROR("Failed to enter page mode");
		return ERROR_FLASH_OPERATION_FAILED;
	}

	ret = ocmts_io_read_u32(ocmts, aurix_bank->reg_addr + aurix_bank->sts_offset, &flash_sts);
	if (ret) {
		LOG_ERROR("Failed to read flash status");
		return ERROR_FLASH_OPERATION_FAILED;
	}
	if (!(flash_sts & (1 << aurix_bank->page_bit))) {
		LOG_ERROR("Failed to enter page mode.");
		return ERROR_FLASH_OPERATION_FAILED;
	}
	return ERROR_OK;
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
	if (ret)
		return ret;

	while (first <= last) {
		/* Align sector count to physical sector boundary */
		uint32_t sectors_to_boundary =
			MIN(last - first + 1, aurix_bank->phys_sector_size / aurix_bank->sector_size -
									  (first % (aurix_bank->phys_sector_size / aurix_bank->sector_size)));
		/* Limit to logical sector erase count. */
		uint32_t sector_count = MIN(aurix_bank->num_erase_sectors, MIN(last - first + 1, sectors_to_boundary));

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
sequence_err:
		if (ret) {
			LOG_ERROR("Failed to execute flash erase sequence address: 0x%08x, "
					  "sector count: %u",
					  addr, sector_count);
			aurix_eflash_reset_to_read(bank);
			return ERROR_FLASH_OPERATION_FAILED;
		}
		first += sector_count;

		ret = aurix_eflash_handle_error(bank, aurix_bank->busy_bit, aurix_bank->erase_timeout_ms);
		if (ret)
			return ret;
	}

	return ERROR_OK;
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

	ret = aurix_eflash_check_busy(bank);
	if (ret)
		return ret;

	while (page_offset < count) {
		const bool burst_mode = count - page_offset >= aurix_bank->burst_size;
		const uint32_t copy_size = burst_mode ? aurix_bank->burst_size : aurix_bank->page_size;
		uint32_t i;

		ret = aurix_eflash_enter_page_mode(bank);
		if (ret)
			return ret;

		if (aurix_df_check_if_tc4x(bank->target->tap->idcode)) {
			for (i = 0; i < copy_size && page_offset + i < count; i += 4) {
				ret = ocmts_queue_write_u32(ocmts, aurix_bank->cmd_addr + 0x55F4, (void *)(buffer + page_offset + i));
				if (ret)
					goto err;
			}
		} else {
			for (i = 0; i < copy_size && page_offset + i < count; i += 4) {
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
			LOG_ERROR("Failed to execute flash program sequence at address: "
					  "0x%08x, data size: %u",
					  addr, copy_size);
			aurix_eflash_reset_to_read(bank);
			return ERROR_FLASH_OPERATION_FAILED;
		}

		if (aurix_eflash_handle_error(bank, aurix_bank->busy_bit, aurix_bank->write_timeout_ms) != ERROR_OK)
			return ERROR_FLASH_OPERATION_FAILED;
	}

	return ERROR_OK;
}

static int aurix_eflash_read(struct flash_bank *bank, uint8_t *buffer, uint32_t offset, uint32_t count)
{
	return target_read_buffer(bank->target, bank->base + offset, count, buffer);
}

static void aurix_free_driver_priv(struct flash_bank *bank)
{
	free(bank->driver_priv);
}

FLASH_BANK_COMMAND_HANDLER(tc3x_flash_bank_command)
{
	struct aurix_eflash_bank *tc3x_bank;
	bool is_pflash = bank->base >= 0x80000000 && bank->base < 0x81000000;
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

	tc3x_bank->probed = false;
	bank->driver_priv = tc3x_bank;

	return ERROR_OK;
}

FLASH_BANK_COMMAND_HANDLER(tc4x_flash_bank_command)
{
	struct aurix_eflash_bank *tc4x_bank;
	bank->base = (bank->base & ~0xF0000000) | 0xA0000000;

	bool is_pflash0 = bank->base >= 0xA0000000 && bank->base < 0xA1800000;
	bool is_pflash1 = bank->base == 0xA8000000;
	bool is_ucb0 = bank->base == 0xAE400000;
	bool is_ucb1 = bank->base == 0xAEC00000;
	bool is_dflash0 = bank->base == 0xAE000000 || bank->base == 0xAE400000;
	bool is_dflash1 = bank->base == 0xAE800000 || bank->base == 0xAEC00000;
	bool is_dflash = is_dflash0 || is_dflash1;
	bool is_ucb = is_ucb0 || is_ucb1;

	if (is_pflash0) {
		if (bank->size % 0x100000 != 0 || bank->size > 2 * 1024 * 1024) {
			LOG_ERROR("Invalid pflash0 bank size. Size should be 1MB or 2MB.");
			return ERROR_FLASH_BANK_INVALID;
		}
		if (bank->base % (2 * 1024 * 1024) != 0) {
			LOG_ERROR("Invalid pflash0 bank base address. Base should be "
					  "aligned to 2MB.");
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

	tc4x_bank->probed = false;
	bank->driver_priv = tc4x_bank;

	return ERROR_OK;
}

const struct flash_driver tc3x_eflash = {
	.name = "tc3x_eflash",
	.flash_bank_command = tc3x_flash_bank_command,
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
	.probe = tc4x_eflash_probe,
	.auto_probe = tc4x_eflash_auto_probe,
	.erase = aurix_eflash_erase,
	.write = aurix_eflash_write,
	.read = aurix_eflash_read,
	.free_driver_priv = aurix_free_driver_priv,
};
