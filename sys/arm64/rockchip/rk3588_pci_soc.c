/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/sysctl.h>

#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>
#include <dev/pci/pci_soc.h>

SYSCTL_DECL(_hw_pci);

static int rk3588_pci_mrrs = 128;
SYSCTL_INT(_hw_pci, OID_AUTO, rk3588_mrrs, CTLFLAG_RWTUN,
    &rk3588_pci_mrrs, 0,
    "RK3588 PCIe child Max Read Request Size; set 0 to disable");

static int rk3588_pci_rtl8125_mrrs = 2048;
SYSCTL_INT(_hw_pci, OID_AUTO, rk3588_rtl8125_mrrs, CTLFLAG_RWTUN,
    &rk3588_pci_rtl8125_mrrs, 0,
    "RK3588 RTL8125 Max Read Request Size; set 0 to disable");

static void
rk3588_pci_child_added(device_t child)
{
	device_t root;
	int before, after, target;

	root = pci_find_pcie_root_port(child);
	if (root == NULL || root == child)
		return;
	if (pci_get_vendor(root) != 0x1d87 || pci_get_device(root) != 0x3588)
		return;

	target = pci_get_vendor(child) == 0x10ec &&
	    pci_get_device(child) == 0x8125 ? rk3588_pci_rtl8125_mrrs :
	    rk3588_pci_mrrs;
	if (target <= 0)
		return;

	before = pci_get_max_read_req(child);
	after = pci_set_max_read_req(child, target);
	device_printf(child,
	    "RK3588-DIAG V2166 pci child MRRS root=%s before=%d target=%d after=%d\n",
	    device_get_nameunit(root), before, target, after);
}

static const struct pci_soc_ops rk3588_pci_soc_ops = {
	.child_added = rk3588_pci_child_added,
};

PCI_SOC_REGISTER(rk3588, rk3588_pci_soc_ops);
