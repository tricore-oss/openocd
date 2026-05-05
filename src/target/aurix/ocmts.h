/* SPDX-License-Identifier: GPL-2.0-or-later */

/***************************************************************************
 *   OCMTS Interface Module for Infineon AURIX                             *
 *   Copyright (C) 2026 Infineon Technologies AG                           *
 ***************************************************************************/

#ifndef OPENOCD_TARGET_AURIX_OCMTS_H
#define OPENOCD_TARGET_AURIX_OCMTS_H

#include <helper/list.h>
#include <jtag/jtag.h>

struct ocmts {
	char *name;
	struct list_head lh;
	struct jtag_tap *tap;
	uint8_t con_id;

	const struct ocmts_ops *ops;
	uint8_t *queue_buffer;
};

/**
 * @brief OCMTS (On-Chip Multi-Core Test System) operations interface for Aurix
 * targets
 *
 * This structure defines the set of function pointers for queuing and executing
 * Cerberus IO instructions on Aurix OCMTS interface. All operations follow a
 * queue-then-run pattern where instructions are queued first and then executed
 * via the run() function.
 */
struct ocmts_ops {
	/**
	 * @brief Connect to the OCMTS interface
	 * @param ocmts Pointer to the OCMTS structure
	 * @return ERROR_OK on success, error code otherwise
	 */
	int (*connect)(struct ocmts *ocmts);

	/**
	 * @brief Queue IO_CONFIG instruction (0x0) to set the IOCONF register
	 * @param ocmts Pointer to the OCMTS structure
	 * @param config 12-bit configuration value
	 * @return ERROR_OK on success, error code otherwise
	 * @note All modes provided OSTATE.IF_LCK is not set
	 */
	int (*queue_io_config)(struct ocmts *ocmts, uint16_t config);

	/**
	 * @brief Queue IO_SET_ADDRESS instruction (0x1) to set the IOADDR register
	 * @param ocmts Pointer to the OCMTS structure
	 * @param addr 16/32-bit address value
	 * @return ERROR_OK on success, error code otherwise
	 * @note All modes provided OSTATE.IF_LCK is not set
	 */
	int (*queue_io_set_address)(struct ocmts *ocmts, uint32_t addr);
	/**
	 * @brief Queue IO_WRITE_BLOCK instruction (0x2) to write a data block
	 * @param ocmts Pointer to the OCMTS structure
	 * @param buffer Pointer to data buffer to write
	 * @param count Number of units (32-bit or 64-bit) to write
	 * @return ERROR_OK on success, error code otherwise
	 * @note RW Mode only. Writes starting from address in IOADDR
	 */

	int (*queue_io_write_block)(struct ocmts *ocmts, const void *buffer, size_t count);
	/**
	 * @brief Queue IO_READ_BLOCK instruction (0x3) to read a data block
	 * @param ocmts Pointer to the OCMTS structure
	 * @param buffer Pointer to buffer for received data
	 * @param count Number of units (32-bit or 64-bit) to read
	 * @return ERROR_OK on success, error code otherwise
	 * @note RW Mode only. Reads starting from address in IOADDR
	 */

	int (*queue_io_read_block)(struct ocmts *ocmts, void *buffer, size_t count);
	/**
	 * @brief Queue IO_WRITE_WORD instruction (0x4) to write a 32-bit word
	 * @param ocmts Pointer to the OCMTS structure
	 * @param data Pointer to 32-bit data to write/send
	 * @return ERROR_OK on success, error code otherwise
	 * @note RW Mode: Write word. COM Mode: Send word
	 */
	int (*queue_io_write_word)(struct ocmts *ocmts, const void *data);
	/**
	 * @brief Queue IO_READ_WORD instruction (0x5) to read a 32-bit word
	 * @param ocmts Pointer to the OCMTS structure
	 * @param data Pointer to store the read 32-bit word
	 * @return ERROR_OK on success, error code otherwise
	 * @note RW Mode: Read word. COM Mode: Request/Receive word
	 */
	int (*queue_io_read_word)(struct ocmts *ocmts, uint32_t *data);
	/**
	 * @brief Queue IO_WRITE_HWORD instruction (0x6) to write a 16-bit half word
	 * @param ocmts Pointer to the OCMTS structure
	 * @param data Pointer to 16-bit data to write
	 * @return ERROR_OK on success, error code otherwise
	 * @note RW Mode only
	 */
	int (*queue_io_write_hword)(struct ocmts *ocmts, const void *data);
	/**
	 * @brief Queue IO_READ_HWORD instruction (0x7) to read a 16-bit half word
	 * @param ocmts Pointer to the OCMTS structure
	 * @param data Pointer to store the read 16-bit half word
	 * @return ERROR_OK on success, error code otherwise
	 * @note RW Mode only
	 */
	int (*queue_io_read_hword)(struct ocmts *ocmts, uint16_t *data);
	/**
	 * @brief Queue IO_WRITE_BYTE instruction (0x8) to write an 8-bit byte
	 * @param ocmts Pointer to the OCMTS structure
	 * @param data Pointer to 8-bit data to write
	 * @return ERROR_OK on success, error code otherwise
	 * @note RW Mode only
	 */

	int (*queue_io_write_byte)(struct ocmts *ocmts, const void *data);
	/**
	 * @brief Queue IO_READ_BYTE instruction (0x9) to read an 8-bit byte
	 * @param ocmts Pointer to the OCMTS structure
	 * @param data Pointer to store the read 8-bit byte
	 * @return ERROR_OK on success, error code otherwise
	 * @note RW Mode only
	 */

	int (*queue_io_read_byte)(struct ocmts *ocmts, uint8_t *data);
	/**
	 * @brief Queue IO_SET_ACR instruction (0xA) to set the IOACR register
	 * @param ocmts Pointer to the OCMTS structure
	 * @param acr 8-bit ACR value
	 * @return ERROR_OK on success, error code otherwise
	 * @note All modes provided
	 */

	int (*queue_io_set_acr)(struct ocmts *ocmts, uint8_t acr);
	/**
	 * @brief Queue IO_SUPERVISOR instruction (0xB) to terminate Cerberus state
	 * and read IOINFO
	 * @param ocmts Pointer to the OCMTS structure
	 * @param ioinfo Pointer to store the IOINFO register value
	 * @return ERROR_OK on success, error code otherwise
	 * @note All modes
	 */
	int (*queue_io_supervisor)(struct ocmts *ocmts, uint32_t *ioinfo);
	/**
	 * @brief Queue IO_READ_AGAIN instruction (0xC) to re-read the internal shift
	 * register
	 * @param ocmts Pointer to the OCMTS structure
	 * @param data Pointer to store the 32-bit data from shift register
	 * @return ERROR_OK on success, error code otherwise
	 * @note Returns same data as previous R-Type instruction. All modes
	 */

	int (*queue_io_read_again)(struct ocmts *ocmts, uint32_t *data);
	/**
	 * @brief Queue IO_READ_TRIG instruction (0xD) to read and clear highest
	 * priority TRIGx register
	 * @param ocmts Pointer to the OCMTS structure
	 * @param trig Pointer to store the trigger register value
	 * @return ERROR_OK on success, error code otherwise
	 * @note Destructive read. Use IO_READ_AGAIN in case of transmission errors.
	 *       All modes provided OSTATE.IF_LCK is not set
	 */
	int (*queue_io_read_trig)(struct ocmts *ocmts, uint32_t *trig);
	/**
	 * @brief Queue IO_SET_OJCONF instruction (0xE) to set the OJCONF register
	 * @param ocmts Pointer to the OCMTS structure
	 * @param ojconf 16-bit OJCONF value
	 * @return ERROR_OK on success, error code otherwise
	 * @note All modes
	 */
	int (*queue_io_set_ojconf)(struct ocmts *ocmts, uint16_t ojconf);
	/**
	 * @brief Queue IO_CLIENT_ID instruction (0xF) to read the CLIENT_ID register
	 * @param ocmts Pointer to the OCMTS structure
	 * @param client_id Pointer to store the CLIENT_ID register value
	 * @return ERROR_OK on success, error code otherwise
	 * @note All modes provided OSTATE.IF_LCK is not set
	 */
	int (*queue_io_client_id)(struct ocmts *ocmts, uint32_t *client_id);

	/** Execute all queued operations */
	int (*run)(struct ocmts *ocmts);
};

struct ocmts *ocmts_by_jim_obj(Jim_Interp *interp, Jim_Obj *o);
int ocmts_register_commands(struct command_context *cmd_ctx);

static inline int ocmts_io_write_block(struct ocmts *ocmts, uint32_t addr, const void *buffer, uint32_t size)
{
	int retval = ocmts->ops->queue_io_set_address(ocmts, addr);
	if (retval != ERROR_OK)
		return retval;
	retval = ocmts->ops->queue_io_write_block(ocmts, buffer, size);
	if (retval != ERROR_OK)
		return retval;
	return ocmts->ops->run(ocmts);
}

static inline int ocmts_io_read_block(struct ocmts *ocmts, uint32_t addr, void *buffer, uint32_t count)
{
	int retval = ocmts->ops->queue_io_set_address(ocmts, addr);
	if (retval != ERROR_OK)
		return retval;
	retval = ocmts->ops->queue_io_read_block(ocmts, buffer, count);
	if (retval != ERROR_OK)
		return retval;
	return ocmts->ops->run(ocmts);
}

static inline int ocmts_io_read_u8(struct ocmts *ocmts, uint32_t addr, uint8_t *data)
{
	int retval = ocmts->ops->queue_io_set_address(ocmts, addr);
	if (retval != ERROR_OK)
		return retval;
	retval = ocmts->ops->queue_io_read_byte(ocmts, data);
	if (retval != ERROR_OK)
		return retval;
	return ocmts->ops->run(ocmts);
}

static inline int ocmts_io_write_u8(struct ocmts *ocmts, uint32_t addr, uint8_t data)
{
	int retval = ocmts->ops->queue_io_set_address(ocmts, addr);
	if (retval != ERROR_OK)
		return retval;
	retval = ocmts->ops->queue_io_write_byte(ocmts, &data);
	if (retval != ERROR_OK)
		return retval;
	return ocmts->ops->run(ocmts);
}

static inline int ocmts_io_read_u16(struct ocmts *ocmts, uint32_t addr, uint16_t *data)
{
	int retval = ocmts->ops->queue_io_set_address(ocmts, addr);
	if (retval != ERROR_OK)
		return retval;
	retval = ocmts->ops->queue_io_read_hword(ocmts, data);
	if (retval != ERROR_OK)
		return retval;
	return ocmts->ops->run(ocmts);
}

static inline int ocmts_io_write_u16(struct ocmts *ocmts, uint32_t addr, uint16_t data)
{
	int retval = ocmts->ops->queue_io_set_address(ocmts, addr);
	if (retval != ERROR_OK)
		return retval;
	retval = ocmts->ops->queue_io_write_hword(ocmts, &data);
	if (retval != ERROR_OK)
		return retval;
	return ocmts->ops->run(ocmts);
}

static inline int ocmts_io_read_u32(struct ocmts *ocmts, uint32_t addr, uint32_t *data)
{
	int retval = ocmts->ops->queue_io_set_address(ocmts, addr);
	if (retval != ERROR_OK)
		return retval;
	retval = ocmts->ops->queue_io_read_word(ocmts, data);
	if (retval != ERROR_OK)
		return retval;
	return ocmts->ops->run(ocmts);
}

static inline int ocmts_io_write_u32(struct ocmts *ocmts, uint32_t addr, uint32_t data)
{
	int retval = ocmts->ops->queue_io_set_address(ocmts, addr);
	if (retval != ERROR_OK)
		return retval;
	retval = ocmts->ops->queue_io_write_word(ocmts, &data);
	if (retval != ERROR_OK)
		return retval;
	return ocmts->ops->run(ocmts);
}

static inline int ocmts_queue_write_block(struct ocmts *ocmts, uint32_t addr, const void *buffer, uint32_t size)
{
	int retval = ocmts->ops->queue_io_set_address(ocmts, addr);
	if (retval != ERROR_OK)
		return retval;
	return ocmts->ops->queue_io_write_block(ocmts, buffer, size);
}

static inline int ocmts_queue_read_block(struct ocmts *ocmts, uint32_t addr, void *buffer, uint32_t count)
{
	int retval = ocmts->ops->queue_io_set_address(ocmts, addr);
	if (retval != ERROR_OK)
		return retval;
	return ocmts->ops->queue_io_read_block(ocmts, buffer, count);
}

static inline int ocmts_queue_read_u8(struct ocmts *ocmts, uint32_t addr, uint8_t *data)
{
	int retval = ocmts->ops->queue_io_set_address(ocmts, addr);
	if (retval != ERROR_OK)
		return retval;
	return ocmts->ops->queue_io_read_byte(ocmts, data);
}

static inline int ocmts_queue_write_u8(struct ocmts *ocmts, uint32_t addr, const uint8_t *data)
{
	int retval = ocmts->ops->queue_io_set_address(ocmts, addr);
	if (retval != ERROR_OK)
		return retval;
	return ocmts->ops->queue_io_write_byte(ocmts, data);
}

static inline int ocmts_queue_read_u16(struct ocmts *ocmts, uint32_t addr, uint16_t *data)
{
	int retval = ocmts->ops->queue_io_set_address(ocmts, addr);
	if (retval != ERROR_OK)
		return retval;
	return ocmts->ops->queue_io_read_hword(ocmts, data);
}

static inline int ocmts_queue_write_u16(struct ocmts *ocmts, uint32_t addr, const uint16_t *data)
{
	int retval = ocmts->ops->queue_io_set_address(ocmts, addr);
	if (retval != ERROR_OK)
		return retval;
	return ocmts->ops->queue_io_write_hword(ocmts, data);
}

static inline int ocmts_queue_read_u32(struct ocmts *ocmts, uint32_t addr, uint32_t *data)
{
	int retval = ocmts->ops->queue_io_set_address(ocmts, addr);
	if (retval != ERROR_OK)
		return retval;
	return ocmts->ops->queue_io_read_word(ocmts, data);
}

static inline int ocmts_queue_write_u32(struct ocmts *ocmts, uint32_t addr, const uint32_t *data)
{
	int retval = ocmts->ops->queue_io_set_address(ocmts, addr);
	if (retval != ERROR_OK)
		return retval;
	return ocmts->ops->queue_io_write_word(ocmts, data);
}

static inline int ocmts_queue_io_write_block(struct ocmts *ocmts, const void *buffer, uint32_t count)
{
	return ocmts->ops->queue_io_write_block(ocmts, buffer, count);
}

static inline int ocmts_queue_io_read_block(struct ocmts *ocmts, void *buffer, uint32_t count)
{
	return ocmts->ops->queue_io_read_block(ocmts, buffer, count);
}

static inline int ocmts_queue_io_write_word(struct ocmts *ocmts, const void *data)
{
	return ocmts->ops->queue_io_write_word(ocmts, data);
}

static inline int ocmts_queue_io_read_word(struct ocmts *ocmts, uint32_t *data)
{
	return ocmts->ops->queue_io_read_word(ocmts, data);
}

static inline int ocmts_queue_io_write_hword(struct ocmts *ocmts, const void *data)
{
	return ocmts->ops->queue_io_write_hword(ocmts, data);
}

static inline int ocmts_queue_io_read_hword(struct ocmts *ocmts, uint16_t *data)
{
	return ocmts->ops->queue_io_read_hword(ocmts, data);
}

static inline int ocmts_queue_io_write_byte(struct ocmts *ocmts, const void *data)
{
	return ocmts->ops->queue_io_write_byte(ocmts, data);
}

static inline int ocmts_queue_io_read_byte(struct ocmts *ocmts, uint8_t *data)
{
	return ocmts->ops->queue_io_read_byte(ocmts, data);
}

static inline int ocmts_queue_io_set_address(struct ocmts *ocmts, uint32_t addr)
{
	return ocmts->ops->queue_io_set_address(ocmts, addr);
}

static inline int ocmts_run(struct ocmts *ocmts)
{
	return ocmts->ops->run(ocmts);
}
#endif
