/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/linker_set.h>

#include <dev/pci/pci_soc.h>

SET_DECLARE(pci_soc_set, const struct pci_soc_ops);

void
pci_soc_child_added(device_t child)
{
	const struct pci_soc_ops **ops;

	SET_FOREACH(ops, pci_soc_set) {
		if (*ops != NULL && (*ops)->child_added != NULL)
			(*ops)->child_added(child);
	}
}
