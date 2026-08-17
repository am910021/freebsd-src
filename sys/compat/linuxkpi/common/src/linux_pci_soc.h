/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 */

#ifndef _LINUXKPI_COMMON_LINUX_PCI_SOC_H_
#define	_LINUXKPI_COMMON_LINUX_PCI_SOC_H_

#include <sys/bus.h>
#include <vm/vm.h>

struct linux_pci_soc_ops {
	bool	(*dma_pool_coherent)(device_t);
	vm_memattr_t (*dma_alloc_coherent_memattr)(device_t,
			    vm_memattr_t);
};

void	linux_pci_soc_register(const struct linux_pci_soc_ops *);
bool	linux_pci_soc_dma_pool_coherent(device_t);
vm_memattr_t linux_pci_soc_dma_alloc_coherent_memattr(device_t,
		    vm_memattr_t);

#endif /* _LINUXKPI_COMMON_LINUX_PCI_SOC_H_ */
