/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 *
 * RK3588 SMMU platform policy hooks.
 */

#include <sys/cdefs.h>
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/sysctl.h>

#include <dev/ofw/ofw_bus_subr.h>
#include <dev/ofw/openfirm.h>

#include <arm64/iommu/smmu_platform.h>
#include <arm64/iommu/smmu_soc.h>

SYSCTL_DECL(_hw_smmu);

static int rk3588_smmu_event_print_limit = 32;
static u_long rk3588_smmu_event_total;
static u_long rk3588_smmu_event_translation;
static u_long rk3588_smmu_event_other;
static u_long rk3588_smmu_event_last_id;
static u_long rk3588_smmu_event_last_sid;
static u_long rk3588_smmu_event_last_input_addr;
static int rk3588_smmu_dma_memattr_policy = SMMU_DMA_MEMATTR_AUTO;
static int rk3588_smmu_sync_failures;
static int rk3588_smmu_suppress_map_tlbi_enable = 1;
static int rk3588_smmu_range_tlbi = 1;
static int rk3588_smmu_unmap_asid_tlbi_enable = 1;
static int rk3588_smmu_unmap_asid_tlbi_threshold = 2;

SYSCTL_INT(_hw_smmu, OID_AUTO, v134_skip_map_tlbi, CTLFLAG_RWTUN,
    &rk3588_smmu_suppress_map_tlbi_enable, 0,
    "RK3588-DIAG V256 suppress SMMU map-side TLBI; unmap-side TLBI remains enabled");
SYSCTL_INT(_hw_smmu, OID_AUTO, v257_range_tlbi, CTLFLAG_RWTUN,
    &rk3588_smmu_range_tlbi, 0,
    "RK3588-DIAG V257 use SMMU range TLBI when hardware supports it");
SYSCTL_INT(_hw_smmu, OID_AUTO, v264_unmap_asid_tlbi, CTLFLAG_RWTUN,
    &rk3588_smmu_unmap_asid_tlbi_enable, 0,
    "RK3588-DIAG V264 use ASID TLBI for multi-page SMMU unmaps");
SYSCTL_INT(_hw_smmu, OID_AUTO, v264_unmap_asid_tlbi_threshold,
    CTLFLAG_RWTUN, &rk3588_smmu_unmap_asid_tlbi_threshold, 0,
    "RK3588-DIAG V264 minimum unmapped pages for ASID TLBI");
SYSCTL_INT(_hw_smmu, OID_AUTO, v219_event_print_limit, CTLFLAG_RWTUN,
    &rk3588_smmu_event_print_limit, 0,
    "RK3588-DIAG V219 maximum full SMMU event records printed");
SYSCTL_ULONG(_hw_smmu, OID_AUTO, v219_event_total, CTLFLAG_RD,
    &rk3588_smmu_event_total, 0,
    "RK3588-DIAG V219 total SMMU events seen");
SYSCTL_ULONG(_hw_smmu, OID_AUTO, v302_event_translation, CTLFLAG_RD,
    &rk3588_smmu_event_translation, 0,
    "RK3588-DIAG V302 SMMU translation fault events seen");
SYSCTL_ULONG(_hw_smmu, OID_AUTO, v302_event_other, CTLFLAG_RD,
    &rk3588_smmu_event_other, 0,
    "RK3588-DIAG V302 SMMU non-translation events seen");
SYSCTL_ULONG(_hw_smmu, OID_AUTO, v302_event_last_id, CTLFLAG_RD,
    &rk3588_smmu_event_last_id, 0,
    "RK3588-DIAG V302 last SMMU event id");
SYSCTL_ULONG(_hw_smmu, OID_AUTO, v302_event_last_sid, CTLFLAG_RD,
    &rk3588_smmu_event_last_sid, 0,
    "RK3588-DIAG V302 last SMMU event SID");
SYSCTL_ULONG(_hw_smmu, OID_AUTO, v302_event_last_input_addr, CTLFLAG_RD,
    &rk3588_smmu_event_last_input_addr, 0,
    "RK3588-DIAG V302 last SMMU event input address");
SYSCTL_INT(_hw_smmu, OID_AUTO, v268_dma_memattr_policy, CTLFLAG_RWTUN,
    &rk3588_smmu_dma_memattr_policy, SMMU_DMA_MEMATTR_AUTO,
    "RK3588-DIAG V268 SMMU DMA memattr policy: -2=auto, -1=generic, 0=normal-nc, 1=preserve page attr, 2=device");

static bool
rk3588_smmu_active(void)
{
	static int active = -1;
	phandle_t root;

	if (active >= 0)
		return (active != 0);

	root = OF_peer(0);
	active = root > 0 &&
	    (ofw_bus_node_is_compatible(root, "rockchip,rk3588") ||
	    ofw_bus_node_is_compatible(root, "rockchip,rk3588s"));
	return (active != 0);
}

static int
rk3588_smmu_get_dma_memattr_policy(device_t dev __unused, int device_policy)
{

	if (!rk3588_smmu_active())
		return (device_policy);
	if (rk3588_smmu_dma_memattr_policy == SMMU_DMA_MEMATTR_AUTO)
		return (device_policy);
	return (rk3588_smmu_dma_memattr_policy);
}

static void
rk3588_smmu_sync_timeout(device_t dev, uint32_t target, uint32_t prod,
    uint32_t cons, uint32_t hwprod, uint32_t hwcons, uint32_t cerr,
    uint32_t cr0, uint32_t cr0ack, uint32_t gerror, uint32_t gerrorn)
{

	if (!rk3588_smmu_active())
		return;
	if (rk3588_smmu_sync_failures < 8) {
		device_printf(dev,
		    "RK3588-DIAG V099 smmu sync cons-poll timeout "
		    "target=0x%x prod=0x%x cons=0x%x hwprod=0x%x "
		    "hwcons=0x%x cerr=%u cr0=0x%x cr0ack=0x%x "
		    "gerror=0x%x gerrorn=0x%x\n",
		    target, prod, cons, hwprod, hwcons, cerr, cr0, cr0ack,
		    gerror, gerrorn);
	} else if (rk3588_smmu_sync_failures == 8) {
		device_printf(dev,
		    "RK3588-DIAG V099 smmu sync timeout logs suppressed\n");
	}
	rk3588_smmu_sync_failures++;
}

static bool
rk3588_smmu_event_record(device_t dev, uint8_t event_id, u_int sid,
    uintptr_t input_addr)
{
	u_long event_no;

	if (!rk3588_smmu_active())
		return (true);

	event_no = atomic_fetchadd_long(&rk3588_smmu_event_total, 1) + 1;
	rk3588_smmu_event_last_id = event_id;
	rk3588_smmu_event_last_sid = sid;
	rk3588_smmu_event_last_input_addr = input_addr;

	if (event_id == 0x10)
		atomic_add_long(&rk3588_smmu_event_translation, 1);
	else
		atomic_add_long(&rk3588_smmu_event_other, 1);

	if (rk3588_smmu_event_print_limit >= 0 &&
	    event_no > (u_long)rk3588_smmu_event_print_limit) {
		if (event_no == (u_long)rk3588_smmu_event_print_limit + 1) {
			device_printf(dev,
			    "RK3588-DIAG V219 SMMU event print limit reached; "
			    "further events suppressed\n");
		}
		return (false);
	}
	return (true);
}

static bool
rk3588_smmu_range_tlbi_enabled(device_t dev __unused,
    uint32_t features __unused)
{

	if (!rk3588_smmu_active())
		return (true);
	return (rk3588_smmu_range_tlbi != 0);
}

static bool
rk3588_smmu_unmap_asid_tlbi(device_t dev __unused, uintmax_t pages,
    uint32_t features __unused)
{

	if (!rk3588_smmu_active() || rk3588_smmu_unmap_asid_tlbi_enable == 0)
		return (false);
	if (pages < (uintmax_t)rk3588_smmu_unmap_asid_tlbi_threshold)
		return (false);
	return (true);
}

static bool
rk3588_smmu_suppress_map_tlbi(device_t dev __unused)
{

	return (rk3588_smmu_active() &&
	    rk3588_smmu_suppress_map_tlbi_enable != 0);
}

static const struct smmu_soc_ops rk3588_smmu_soc_ops = {
	.dma_memattr_policy = rk3588_smmu_get_dma_memattr_policy,
	.sync_timeout = rk3588_smmu_sync_timeout,
	.event_record = rk3588_smmu_event_record,
	.range_tlbi_enabled = rk3588_smmu_range_tlbi_enabled,
	.unmap_asid_tlbi = rk3588_smmu_unmap_asid_tlbi,
	.suppress_map_tlbi = rk3588_smmu_suppress_map_tlbi,
};

static void
rk3588_smmu_register(void *arg __unused)
{

	smmu_soc_register(&rk3588_smmu_soc_ops);
}

SYSINIT(rk3588_smmu_register, SI_SUB_DRIVERS, SI_ORDER_FIRST,
    rk3588_smmu_register, NULL);

int
smmu_fdt_platform_dma_memattr_policy(device_t dev)
{
	phandle_t root;

	root = OF_peer(0);
	if (root > 0 &&
	    (ofw_bus_node_is_compatible(root, "rockchip,rk3588") ||
	    ofw_bus_node_is_compatible(root, "rockchip,rk3588s"))) {
		device_printf(dev,
		    "RK3588-DIAG V269 FDT SMMU uses Normal-NC DMA memattr policy\n");
		return (SMMU_DMA_MEMATTR_NORMAL_NC);
	}

	return (SMMU_DMA_MEMATTR_GENERIC);
}
