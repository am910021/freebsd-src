/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 */

#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/sysctl.h>

#include <vm/vm.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus_subr.h>

#include <compat/linuxkpi/common/src/linux_pci_soc.h>

SYSCTL_DECL(_compat_linuxkpi);

static int rk3588_linux_pci_dma_pool_coherent_enable = 1;
static int rk3588_linux_pci_dma_alloc_coherent_uncached_enable = 1;

SYSCTL_INT(_compat_linuxkpi, OID_AUTO, rk3588_dma_pool_coherent,
    CTLFLAG_RWTUN, &rk3588_linux_pci_dma_pool_coherent_enable, 0,
    "RK3588-DIAG V221 use coherent DMA semantics for LinuxKPI dma_pool");
SYSCTL_INT(_compat_linuxkpi, OID_AUTO, rk3588_dma_alloc_coherent_uncached,
    CTLFLAG_RWTUN, &rk3588_linux_pci_dma_alloc_coherent_uncached_enable, 0,
    "RK3588-DIAG V249 use uncached CPU mappings for LinuxKPI dma_alloc_coherent on arm64");

static bool
rk3588_linux_pci_active(void)
{
	static int active = -1;
	phandle_t root;

	if (active >= 0)
		return (active != 0);

	root = OF_finddevice("/");
	active = root > 0 &&
	    (ofw_bus_node_is_compatible(root, "rockchip,rk3588") ||
	    ofw_bus_node_is_compatible(root, "rockchip,rk3588s"));
	return (active != 0);
}

static bool
rk3588_linux_pci_dma_pool_coherent(device_t dev __unused)
{

	return (rk3588_linux_pci_active() &&
	    rk3588_linux_pci_dma_pool_coherent_enable != 0);
}

static vm_memattr_t
rk3588_linux_pci_dma_alloc_coherent_memattr(device_t dev __unused,
    vm_memattr_t defattr)
{

#if defined(VM_MEMATTR_UNCACHEABLE)
	if (rk3588_linux_pci_active() &&
	    rk3588_linux_pci_dma_alloc_coherent_uncached_enable != 0)
		return (VM_MEMATTR_UNCACHEABLE);
#endif
	return (defattr);
}

static const struct linux_pci_soc_ops rk3588_linux_pci_ops = {
	.dma_pool_coherent = rk3588_linux_pci_dma_pool_coherent,
	.dma_alloc_coherent_memattr =
	    rk3588_linux_pci_dma_alloc_coherent_memattr,
};

static void
rk3588_linux_pci_register(void *arg __unused)
{

	linux_pci_soc_register(&rk3588_linux_pci_ops);
}

SYSINIT(rk3588_linux_pci_register, SI_SUB_DRIVERS, SI_ORDER_FIRST,
    rk3588_linux_pci_register, NULL);
