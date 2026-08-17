/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 */

#ifndef _ARM64_ARM64_MACHDEP_SOC_H_
#define	_ARM64_ARM64_MACHDEP_SOC_H_

#include <sys/types.h>
#include <sys/efi.h>
#include <sys/linker_set.h>

#include <machine/metadata.h>

#include <vm/vm.h>

struct arm64_machdep_soc_ops {
	bool	(*match)(void);
	void	(*efi_map_fixup)(struct efi_map_header *efihdr);
	bool	(*memory_contains)(vm_paddr_t pa);
};

#define	ARM64_MACHDEP_SOC_REGISTER(name, ops)				\
	DATA_SET(arm64_machdep_soc_set, ops)

void	arm64_machdep_soc_efi_map_fixup(struct efi_map_header *efihdr);
bool	arm64_machdep_soc_memory_contains(vm_paddr_t pa);

#endif /* _ARM64_ARM64_MACHDEP_SOC_H_ */
