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
#include <jtag/jtag.h>
#include <target/target.h>

#include <target/tricore/aurix_device_family.h>

struct tc3xx_flash_bank {
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
  /** Number of bytes for a burst program operation */
  uint16_t burst_size;
  /** Number of bytes for a page program operation. (Smallest programmable unit)
   */
  uint16_t page_size;
  /** Offset of the busy bit for the bank in the status register */
  uint8_t busy_bit;
  /** Bank probed */
  bool probed;
};

#define UCB_CHIPID 0xAE400008
#define UCB_CHIPID_PROD 0x3C000000
#define SCU_CHIPID 0xF0036140
#define SCU_CHIPID_CHREV 0x3F
#define SCU_CHIPID_CHTEC 0xC0
#define SCU_CHIPID_FSIZE 0x0F000000

static int tc3xx_probe(struct flash_bank *bank) {
  struct tc3xx_flash_bank *tc3xx_bank = bank->driver_priv;
  uint32_t flash_addr = bank->base;
  uint32_t chipid;
  int retval;

  if (tc3xx_bank->probed)
    return ERROR_OK;

  if (aurix_df_check_if_tc4x(bank->target->tap->idcode)) {
    retval = target_read_u32(bank->target, UCB_CHIPID, &chipid);
    if (retval != ERROR_OK) {
      LOG_ERROR("Cannot read CHIPID register.");
      return retval;
    }

    if (((chipid & UCB_CHIPID_PROD) >> 26) != 13) {
      LOG_ERROR("CHIPID register does not match tc4xx with eFLASH.");
      return ERROR_FAIL;
    }
    /* TODO: Check size */
  } else {
    retval = target_read_u32(bank->target, SCU_CHIPID, &chipid);
    if (retval != ERROR_OK) {
      LOG_ERROR("Cannot read tc3xx CHIPID register.");
      return retval;
    }

    if ((chipid & SCU_CHIPID_CHTEC) != 0x80) {
      LOG_ERROR("CHIPID register does not match tc3xx.");
      return ERROR_FAIL;
    }
    /* TODO: Check size  */
  }

  bank->minimal_write_gap = 0;
  bank->write_start_alignment = bank->write_end_alignment =
      tc3xx_bank->page_size;
  bank->num_sectors = bank->size / tc3xx_bank->sector_size;
  bank->sectors = calloc(bank->num_sectors, sizeof(struct flash_sector));
  for (unsigned int i = 0; i < bank->num_sectors; i++) {
    bank->sectors[i].size = tc3xx_bank->sector_size;
    bank->sectors[i].offset = flash_addr - bank->base;
    flash_addr += tc3xx_bank->sector_size;
    /* TOOD: Check erased */
    bank->sectors[i].is_erased = -1;
    /* TODO: Check UCB for protection*/
    bank->sectors[i].is_protected = -1;
  }

  tc3xx_bank->probed = true;

  return ERROR_OK;
}

static int tc3xx_auto_probe(struct flash_bank *bank) {
  struct tc3xx_flash_bank *tc3xx_bank = bank->driver_priv;

  if (tc3xx_bank->probed)
    return ERROR_OK;

  return tc3xx_probe(bank);
}

int tc3xx_erase(struct flash_bank *bank, unsigned int first,
                unsigned int last) {

  struct tc3xx_flash_bank *tc3xx_bank = bank->driver_priv;
  int ret;

  if (bank->target->state != TARGET_HALTED) {
    LOG_ERROR("Target not halted");
    return ERROR_TARGET_NOT_HALTED;
  }

  while (first <= last) {
    /* Align sector count to physical sector boundary */
    uint32_t sector_count =
        MIN(last - first + 1,
            tc3xx_bank->phys_sector_size -
                (first % (bank->size / tc3xx_bank->phys_sector_size)));
    /* Put address always in segment 0xA */
    uint32_t addr =
        (~0xF0000000 & (bank->base + bank->sectors[first].offset)) + 0xA0000000;

    ret = target_write_u32(bank->target, tc3xx_bank->cmd_addr + 0xAA50, addr);
    if (ret) {
      goto err;
    }
    ret = target_write_u32(bank->target, tc3xx_bank->cmd_addr + 0xAA58,
                           sector_count);
    if (ret) {
      goto err;
    }
    ret = target_write_u32(bank->target, tc3xx_bank->cmd_addr + 0xAAA8, 0x80);
    if (ret) {
      goto err;
    }
    ret = target_write_u32(bank->target, tc3xx_bank->cmd_addr + 0xAAA8, 0x50);
    if (ret) {
      goto err;
    }
    first += sector_count;

    uint32_t flash_err = 0;
    uint32_t flash_busy = 0xFFFFFFFF;
    while (flash_err == 0 && (flash_busy & (1 << tc3xx_bank->busy_bit))) {
      ret = target_read_u32(bank->target,
                            tc3xx_bank->reg_addr + tc3xx_bank->err_offset,
                            &flash_err);
      if (ret) {
        goto err;
      }
      ret = target_read_u32(bank->target,
                            tc3xx_bank->reg_addr + tc3xx_bank->sts_offset,
                            &flash_busy);
      if (ret) {
        goto err;
      }
    }

    if (flash_err) {
      LOG_ERROR("Flash operation failed: %x", flash_err);
      ret = target_write_u32(bank->target, tc3xx_bank->cmd_addr + 0x5554, 0xF0);
      if (ret) {
        LOG_ERROR("Failed to execute reset to read");
      }
      return ERROR_FLASH_OPERATION_FAILED;
    }
  }

  return ERROR_OK;

err:
  LOG_ERROR("Failed to execute flash erase sequence");
  return ERROR_FLASH_OPERATION_FAILED;
}

static int tc3xx_write(struct flash_bank *bank, const uint8_t *buffer,
                       uint32_t offset, uint32_t count) {
  struct tc3xx_flash_bank *tc3xx_bank = bank->driver_priv;
  uint32_t page_offset = 0;
  int ret;

  if (bank->target->state != TARGET_HALTED) {
    LOG_ERROR("Target not halted");
    return ERROR_TARGET_NOT_HALTED;
  }

  if (offset & ~(tc3xx_bank->page_size - 1) ||
      count % tc3xx_bank->page_size != 0) {
    return ERROR_FLASH_DST_BREAKS_ALIGNMENT;
  }

  while (page_offset < count) {
    const bool burst_mode = count - page_offset >= tc3xx_bank->burst_size;
    const uint32_t copy_size =
        burst_mode ? tc3xx_bank->burst_size : tc3xx_bank->page_size;
    uint32_t i;

    /* Enter page mode*/
    ret = target_write_u32(bank->target, tc3xx_bank->cmd_addr + 0x5554, 0x50);
    if (ret) {
      goto err;
    }

    if (aurix_df_check_if_tc4x(bank->target->tap->idcode)) {
      for (i = 0; i < copy_size && page_offset + i < count; i += 4) {
        uint32_t data;
        memcpy(&data, buffer + page_offset + i, 4);
        ret = target_write_u32(
            bank->target,
            tc3xx_bank->cmd_addr + 0x55F0 + ((i % 8) == 0 ? 0 : 4), data);
        if (ret) {
          goto err;
        }
      }
    } else {
      for (i = 0; i < copy_size && page_offset + i < count; i += 4) {
        uint32_t data;
        memcpy(&data, buffer + page_offset + i, 4);
        ret =
            target_write_u32(bank->target, tc3xx_bank->cmd_addr + 0x55F4, data);
        if (ret) {
          goto err;
        }
      }
    }

    /* Put address always in segment 0xA */
    uint32_t addr =
        (~0xF0000000 & (bank->base + offset + page_offset)) + 0xA0000000;
    page_offset += copy_size;
    /* Execute page write sequence */
    ret = target_write_u32(bank->target, tc3xx_bank->cmd_addr + 0xAA50, addr);
    if (ret) {
      goto err;
    }
    ret = target_write_u32(bank->target, tc3xx_bank->cmd_addr + 0xAA58, 0);
    if (ret) {
      goto err;
    }
    ret = target_write_u32(bank->target, tc3xx_bank->cmd_addr + 0xAAA8, 0xA0);
    if (ret) {
      goto err;
    }

    /* Check for burst sequence*/
    if (burst_mode) {
      ret = target_write_u32(bank->target, tc3xx_bank->cmd_addr + 0xAAA8, 0xA6);
      if (ret) {
        goto err;
      }
      page_offset += tc3xx_bank->burst_size;
    } else {
      ret = target_write_u32(bank->target, tc3xx_bank->cmd_addr + 0xAAA8, 0xAA);
      if (ret) {
        goto err;
      }
    }

    uint32_t flash_err = 0;
    uint32_t flash_busy = 0xFFFFFFFF;
    while (flash_err == 0 && (flash_busy & (1 << tc3xx_bank->busy_bit))) {
      ret = target_read_u32(bank->target,
                            tc3xx_bank->reg_addr + tc3xx_bank->err_offset,
                            &flash_err);
      if (ret) {
        goto err;
      }
      ret = target_read_u32(bank->target,
                            tc3xx_bank->reg_addr + tc3xx_bank->sts_offset,
                            &flash_busy);
      if (ret) {
        goto err;
      }
    }

    if (flash_err) {
      LOG_ERROR("Flash operation failed: %x", flash_err);
      ret = target_write_u32(bank->target, tc3xx_bank->cmd_addr + 0x5554, 0xF0);
      if (ret) {
        LOG_ERROR("Failed to execute reset to read");
      }
      return ERROR_FLASH_OPERATION_FAILED;
    }
  }

  return ERROR_OK;

err:
  LOG_ERROR("Failed to execute flash erase sequence");
  return ERROR_FLASH_OPERATION_FAILED;
}

static int tc3xx_read(struct flash_bank *bank, uint8_t *buffer, uint32_t offset,
                      uint32_t count) {

  return target_read_buffer(bank->target, bank->base + offset, count, buffer);
}

static void tc3xx_free_driver_priv(struct flash_bank *bank) {
  free(bank->driver_priv);
}

static inline bool tc3xx_is_dflash(struct flash_bank *bank) {
  return (bank->base == 0xAF000000 || bank->base == 0xAFC00000);
}

static inline bool tc3xx_is_ucb(struct flash_bank *bank) {
  return (bank->base == 0xAF400000);
}

static inline bool tc4xx_is_dflash(struct flash_bank *bank) {
  return (bank->base == 0xAE000000 || bank->base == 0xAE800000);
}

static inline bool tc4xx_is_ucb(struct flash_bank *bank) {
  /* Check for dflash0 eeprom+ucb and dflash1 eeprom+ucb */
  return (bank->base == 0xAE400000 || bank->base == 0xAEC00000);
}

FLASH_BANK_COMMAND_HANDLER(tc3xx_flash_bank_command) {
  struct tc3xx_flash_bank *tc3xx_bank;

  tc3xx_bank = malloc(sizeof(struct tc3xx_flash_bank));
  if (!tc3xx_bank)
    return ERROR_FLASH_OPERATION_FAILED;

  if (aurix_df_check_if_tc4x(bank->target->tap->idcode)) {
    bool use_cs_pfls = false;
    /* Select the command sequence interface based on bank addresses */
    if (bank->base == 0x84000000 || bank->base == 0xAE800000 ||
        bank->base == 0xAEC00000 ||
        (use_cs_pfls &&
         !(bank->base == 0xAE000000 || bank->base == 0xAE400000))) {
      tc3xx_bank->cmd_addr = 0xF80C0000;
      tc3xx_bank->sts_offset = 0x84;
      tc3xx_bank->err_offset = 0x90;
    } else {
      tc3xx_bank->cmd_addr = 0xF8080000;
      tc3xx_bank->sts_offset = 0x4;
      tc3xx_bank->err_offset = 0x10;
    }
    tc3xx_bank->reg_addr = 0xF8040000;
    tc3xx_bank->busy_bit = bank->base < 0xAE000000   ? bank->bank_number
                           : bank->base < 0xAE800000 ? 16
                                                     : 17;
    tc3xx_bank->page_size =
        tc4xx_is_dflash(bank) || tc4xx_is_dflash(bank) ? 8 : 32;
    tc3xx_bank->burst_size =
        tc4xx_is_dflash(bank) || tc4xx_is_dflash(bank) ? 32 : 512;
    tc3xx_bank->sector_size = tc4xx_is_ucb(bank)      ? 512
                              : tc4xx_is_dflash(bank) ? 0x800
                                                      : 0x4000;
    tc3xx_bank->phys_sector_size = tc4xx_is_ucb(bank)      ? 512
                                   : tc4xx_is_dflash(bank) ? 0x20000
                                                           : 0x100000;
  } else {
    /* cmd_reg and offsets might be changed during probe, if HSM sequencer is
     * required */
    tc3xx_bank->cmd_addr = 0xAF000000;
    tc3xx_bank->reg_addr = 0xF8040000;
    tc3xx_bank->sts_offset = 0x10;
    tc3xx_bank->err_offset = 0x34;
    tc3xx_bank->busy_bit = bank->base < 0xAF000000   ? bank->bank_number + 2
                           : bank->base < 0xAFC00000 ? 0
                                                     : 1;
    tc3xx_bank->page_size =
        tc3xx_is_dflash(bank) || tc3xx_is_ucb(bank) ? 8 : 32;
    tc3xx_bank->burst_size =
        tc3xx_is_dflash(bank) || tc3xx_is_ucb(bank) ? 32 : 256;
    tc3xx_bank->sector_size = tc3xx_is_ucb(bank)      ? 512
                              : tc3xx_is_dflash(bank) ? 0x1000
                                                      : 0x4000;
    tc3xx_bank->phys_sector_size = tc3xx_is_ucb(bank)      ? 512
                                   : tc3xx_is_dflash(bank) ? bank->size
                                                           : 0x100000;
  }

  tc3xx_bank->probed = false;

  bank->driver_priv = tc3xx_bank;

  return ERROR_OK;
}

const struct flash_driver tc3xx_flash = {
    .name = "tc3xx",
    .flash_bank_command = tc3xx_flash_bank_command,
    .probe = tc3xx_probe,
    .auto_probe = tc3xx_auto_probe,
    .erase = tc3xx_erase,
    .write = tc3xx_write,
    .read = tc3xx_read,
    .free_driver_priv = tc3xx_free_driver_priv,
};