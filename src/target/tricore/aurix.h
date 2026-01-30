#ifndef OPENOCD_TARGET_AURIX_H
#define OPENOCD_TARGET_AURIX_H

#include <helper/command.h>
#include <target/target.h>

#include "aurix_ocds.h"
#include "helper/types.h"
#include "target/aurix/aurix_device_family.h"
#include "tricore.h"

struct aurix_core {
  struct target *target;
  struct aurix_ocds *ocds;
  struct tricore tricore;
  target_addr_t base;
  enum aurix_device_family family;
  enum aurix_device_type type;
};

struct aurix_private_config {
  struct aurix_ocds *ocds;
};

static inline struct aurix_core *target_to_aurix(struct target *target) {
  return container_of(target->arch_info, struct aurix_core, tricore);
}

static inline target_addr_t aurix_core_get_reg_addr(struct target *target,
                                                    struct reg *reg) {
  struct tricore *tricore = target_to_tricore(target);
  struct tricore_reg *tricore_reg = reg->arch_info;

  if (!tricore->has_virt || !tricore_reg->per_hr) {
    return target_to_aurix(target)->base + 0x10000 + tricore_reg->addr;
  }

  switch (tricore->active_vm) {
  case 0:
    return target_to_aurix(target)->base + 0x30000 + tricore_reg->addr;
  case 1:
    return target_to_aurix(target)->base + 0x10000 + tricore_reg->addr;
  default:
    return target_to_aurix(target)->base + 0x20000 + tricore_reg->addr;
  }
}

#endif