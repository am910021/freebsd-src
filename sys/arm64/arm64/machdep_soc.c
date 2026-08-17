/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/efi.h>
#include <sys/linker_set.h>

#include <arm64/arm64/machdep_soc.h>

SET_DECLARE(arm64_machdep_soc_set, const struct arm64_machdep_soc_ops);

static const struct arm64_machdep_soc_ops *
arm64_machdep_soc_find(void)
{
	const struct arm64_machdep_soc_ops **ops;

	SET_FOREACH(ops, arm64_machdep_soc_set) {
		if (*ops == NULL || (*ops)->match == NULL)
			continue;
		if ((*ops)->match())
			return (*ops);
	}

	return (NULL);
}

void
arm64_machdep_soc_efi_map_fixup(struct efi_map_header *efihdr)
{
	const struct arm64_machdep_soc_ops *ops;

	ops = arm64_machdep_soc_find();
	if (ops != NULL && ops->efi_map_fixup != NULL)
		ops->efi_map_fixup(efihdr);
}

bool
arm64_machdep_soc_memory_contains(vm_paddr_t pa)
{
	const struct arm64_machdep_soc_ops *ops;

	ops = arm64_machdep_soc_find();
	if (ops != NULL && ops->memory_contains != NULL)
		return (ops->memory_contains(pa));

	return (false);
}
