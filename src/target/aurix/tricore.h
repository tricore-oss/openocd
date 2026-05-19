/* SPDX-License-Identifier: GPL-2.0-or-later */

/***************************************************************************
 *   TriCore Target Definitions for Infineon AURIX                         *
 *   Copyright (C) 2026 Infineon Technologies AG                           *
 ***************************************************************************/

#ifndef OPENOCD_TARGET_TRICORE_TRICORE_H
#define OPENOCD_TARGET_TRICORE_TRICORE_H

#include "target/breakpoints.h"
#include "target/register.h"
#include "target/target.h"

#include "ocmts.h"

enum tricore_version {
	TRICORE_1_6,
	TRICORE_1_6_2,
	TRICORE_1_8,
};

enum tricore_fpu { TRICORE_FPU_NONE, TRICORE_FPU_SINGLE, TRICORE_FPU_DOUBLE };

enum tricore_event_type { TRICORE_EVENT_ADDRESS, TRICORE_EVENT_PC };
enum tricore_event_comapre { TRICORE_EVENT_EQUALITY, TRICORE_EVENT_RANGE };
enum tricore_event_access { TRICORE_EVENT_STORE = 0x1, TRICORE_EVENT_LOAD = 0x2 };

struct tricore_event {
	bool enable;
	bool bbm;
	enum tricore_event_type type;
	enum tricore_event_comapre compare;
	enum tricore_event_access access;

	uint32_t address;
};

enum tricore_debug_counter {
	TRICORE_DEBUG_COUNTER_NO_CHANGE,
	TRICORE_DEBUG_COUNTER_START,
	TRICORE_DEBUG_COUNTER_STOP,
	TRICORE_DEBUG_COUNTER_TOGGLE,
};

enum tricore_event_source {
	TRICORE_EVENT_EXTERNAL = 0,
	TRICORE_EVENT_CORE_REGISTER,
	TRICORE_EVENT_SOFTWARE,
	TRICORE_EVENT_TRIGGER0 = 16,
	TRICORE_EVENT_TRIGGER1,
	TRICORE_EVENT_TRIGGER2,
	TRICORE_EVENT_TRIGGER3,
	TRICORE_EVENT_TRIGGER4,
	TRICORE_EVENT_TRIGGER5,
	TRICORE_EVENT_TRIGGER6,
	TRICORE_EVENT_TRIGGER7,
};

#define TRICORE_NUM_EVENTS 8

enum {
	TRICORE_D0 = 0,
	TRICORE_D1,
	TRICORE_D2,
	TRICORE_D3,
	TRICORE_D4,
	TRICORE_D5,
	TRICORE_D6,
	TRICORE_D7,
	TRICORE_D8,
	TRICORE_D9,
	TRICORE_D10,
	TRICORE_D11,
	TRICORE_D12,
	TRICORE_D13,
	TRICORE_D14,
	TRICORE_D15,
	TRICORE_A0,
	TRICORE_A1,
	TRICORE_A2,
	TRICORE_A3,
	TRICORE_A4,
	TRICORE_A5,
	TRICORE_A6,
	TRICORE_A7,
	TRICORE_A8,
	TRICORE_A9,
	TRICORE_A10,
	TRICORE_A11,
	TRICORE_A12,
	TRICORE_A13,
	TRICORE_A14,
	TRICORE_A15,
	TRICORE_PCXI,
	TRICORE_PSW,
	TRICORE_PC,
	TRICORE_ICR
};

struct tricore_info {
	struct ocmts *ocmts;

	enum tricore_version version;
	enum tricore_fpu fpu;
	struct tricore_event events[8];

	uint32_t dbgsr;
	uint32_t icr;
	void *regs;

	struct reg *pc;

	uint32_t (*get_reg_addr)(struct target *target, uint16_t reg);

	int (*poll)(struct target *target);

	uint32_t (*get_single_step_event)(struct target *target);
	uint32_t (*get_step_event)(struct target *target);
	uint32_t (*get_event)(struct target *target, struct tricore_event *event);
	uint32_t (*get_sw_event)(struct target *target, bool enable, bool bbm, enum tricore_debug_counter cnt);

	int (*examine)(struct target *target);

	uint8_t active_vm;
	enum tricore_event_source active_event;

	bool has_dcache;
	bool has_icache;

	bool virt_enabled;
	bool suspended;
	bool halted;

	uint32_t *algorithm_context;
	unsigned int algorithm_context_regs;
	enum target_debug_reason algorithm_debug_reason;
};

struct tricore_reg {
	struct target *target;
	uint16_t addr;
	uint32_t value;
};

static inline struct tricore_info *target_to_tricore(struct target *target)
{
	return (struct tricore_info *)target->arch_info;
}

#endif
