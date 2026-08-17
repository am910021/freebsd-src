/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 */

#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/systm.h>
#include <sys/sysctl.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus_subr.h>

#include <dev/iommu/busdma_iommu_soc.h>

static int rk3588_iommu_v253_lazy_pcie_net_unload;
static int rk3588_iommu_v258_retain_pcie_net_map;
static int rk3588_iommu_iova_cache = 1;
static int rk3588_iommu_iova_cache_max = 8192;
static int rk3588_iommu_v265_pcie_net_iova_fq = 1;
static int rk3588_iommu_v265_pcie_net_iova_fq_batch = 1024;

SYSCTL_INT(_hw, OID_AUTO, iommu_v253_lazy_pcie_net_unload, CTLFLAG_RWTUN,
    &rk3588_iommu_v253_lazy_pcie_net_unload, 0,
    "RK3588-DIAG V253 defer selected PCIe NIC IOMMU unloads to taskqueue");
SYSCTL_INT(_hw, OID_AUTO, iommu_v258_retain_pcie_net_map, CTLFLAG_RWTUN,
    &rk3588_iommu_v258_retain_pcie_net_map, 0,
    "RK3588-DIAG V258 retain and reuse selected PCIe NIC IOMMU maps");
SYSCTL_INT(_hw, OID_AUTO, iommu_v262_pcie_net_iova_cache, CTLFLAG_RWTUN,
    &rk3588_iommu_iova_cache, 0,
    "RK3588-DIAG V262 cache unmapped PCIe NIC IOVA entries");
SYSCTL_INT(_hw, OID_AUTO, iommu_v262_pcie_net_iova_cache_max, CTLFLAG_RWTUN,
    &rk3588_iommu_iova_cache_max, 0,
    "RK3588-DIAG V262 max cached PCIe NIC IOVA entries per domain");
SYSCTL_INT(_hw, OID_AUTO, iommu_v265_pcie_net_iova_fq, CTLFLAG_RWTUN,
    &rk3588_iommu_v265_pcie_net_iova_fq, 0,
    "RK3588-DIAG V265 queue PCIe NIC IOVA cache reuse until IOTLB sync");
SYSCTL_INT(_hw, OID_AUTO, iommu_v265_pcie_net_iova_fq_batch, CTLFLAG_RWTUN,
    &rk3588_iommu_v265_pcie_net_iova_fq_batch, 0,
    "RK3588-DIAG V265 pending PCIe NIC IOVA entries per IOTLB sync");

static bool
rk3588_iommu_busdma_active(void)
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
rk3588_iommu_busdma_name_prefix(device_t owner, const char *prefix)
{
	const char *name;

	name = device_get_nameunit(owner);
	return (name != NULL && strncmp(name, prefix, strlen(prefix)) == 0);
}

static bool
rk3588_iommu_busdma_name_contains(device_t owner, const char *needle)
{
	const char *name;

	name = device_get_nameunit(owner);
	return (name != NULL && strstr(name, needle) != NULL);
}

static bool
rk3588_iommu_busdma_is_dma_policy_owner(device_t owner)
{

	return (rk3588_iommu_busdma_active() &&
	    (rk3588_iommu_busdma_name_prefix(owner, "re") ||
	    rk3588_iommu_busdma_name_prefix(owner, "rge") ||
	    rk3588_iommu_busdma_name_prefix(owner, "igb") ||
	    rk3588_iommu_busdma_name_contains(owner, "mlx4") ||
	    rk3588_iommu_busdma_name_contains(owner, "mlx5") ||
	    rk3588_iommu_busdma_name_prefix(owner, "mce")));
}

static bool
rk3588_iommu_busdma_retain_map_enabled(device_t owner)
{

	return (rk3588_iommu_v258_retain_pcie_net_map != 0 &&
	    rk3588_iommu_busdma_is_dma_policy_owner(owner));
}

static bool
rk3588_iommu_busdma_lazy_unload_enabled(device_t owner)
{

	return (rk3588_iommu_v253_lazy_pcie_net_unload != 0 &&
	    rk3588_iommu_busdma_is_dma_policy_owner(owner));
}

static bool
rk3588_iommu_busdma_iova_cache_enabled(device_t owner)
{

	return (rk3588_iommu_iova_cache != 0 &&
	    rk3588_iommu_busdma_is_dma_policy_owner(owner));
}

static u_int
rk3588_iommu_busdma_iova_cache_max(device_t owner)
{

	if (!rk3588_iommu_busdma_iova_cache_enabled(owner))
		return (0);
	if (rk3588_iommu_iova_cache_max < 0)
		return (0);
	return ((u_int)rk3588_iommu_iova_cache_max);
}

static bool
rk3588_iommu_busdma_iova_fq_enabled(device_t owner)
{

	return (rk3588_iommu_v265_pcie_net_iova_fq != 0 &&
	    rk3588_iommu_busdma_is_dma_policy_owner(owner));
}

static u_int
rk3588_iommu_busdma_iova_fq_batch(device_t owner)
{

	if (!rk3588_iommu_busdma_iova_fq_enabled(owner))
		return (0);
	if (rk3588_iommu_v265_pcie_net_iova_fq_batch < 0)
		return (0);
	return ((u_int)rk3588_iommu_v265_pcie_net_iova_fq_batch);
}

static const struct iommu_busdma_soc_ops rk3588_iommu_busdma_ops = {
	.is_dma_policy_owner = rk3588_iommu_busdma_is_dma_policy_owner,
	.retain_map_enabled = rk3588_iommu_busdma_retain_map_enabled,
	.lazy_unload_enabled = rk3588_iommu_busdma_lazy_unload_enabled,
	.iova_cache_enabled = rk3588_iommu_busdma_iova_cache_enabled,
	.iova_cache_max = rk3588_iommu_busdma_iova_cache_max,
	.iova_fq_enabled = rk3588_iommu_busdma_iova_fq_enabled,
	.iova_fq_batch = rk3588_iommu_busdma_iova_fq_batch,
};

static void
rk3588_iommu_busdma_register(void *arg __unused)
{

	iommu_busdma_soc_register(&rk3588_iommu_busdma_ops);
}

SYSINIT(rk3588_iommu_busdma_register, SI_SUB_DRIVERS, SI_ORDER_FIRST,
    rk3588_iommu_busdma_register, NULL);
