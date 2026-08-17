/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 */

#ifndef _DEV_PCI_PCI_SOC_H_
#define	_DEV_PCI_PCI_SOC_H_

#include <sys/linker_set.h>

struct pci_soc_ops {
	void	(*child_added)(device_t child);
};

#define	PCI_SOC_REGISTER(name, ops)					\
	DATA_SET(pci_soc_set, ops)

void	pci_soc_child_added(device_t);

#endif /* _DEV_PCI_PCI_SOC_H_ */
