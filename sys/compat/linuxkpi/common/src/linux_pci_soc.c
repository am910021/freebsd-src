/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 */

#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/systm.h>

#include <compat/linuxkpi/common/src/linux_pci_soc.h>

static const struct linux_pci_soc_ops *linux_pci_soc_ops;

void
linux_pci_soc_register(const struct linux_pci_soc_ops *ops)
{

	linux_pci_soc_ops = ops;
}

bool
linux_pci_soc_dma_pool_coherent(device_t dev)
{

	return (linux_pci_soc_ops != NULL &&
	    linux_pci_soc_ops->dma_pool_coherent != NULL &&
	    linux_pci_soc_ops->dma_pool_coherent(dev));
}

vm_memattr_t
linux_pci_soc_dma_alloc_coherent_memattr(device_t dev, vm_memattr_t defattr)
{

	if (linux_pci_soc_ops != NULL &&
	    linux_pci_soc_ops->dma_alloc_coherent_memattr != NULL)
		return (linux_pci_soc_ops->dma_alloc_coherent_memattr(dev,
		    defattr));
	return (defattr);
}
