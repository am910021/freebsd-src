/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 */

#include "opt_platform.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/cpuset.h>
#include <sys/kernel.h>
#include <sys/sysctl.h>

#ifdef FDT
#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#endif

#include <arm64/arm64/gicv3_its_soc.h>

static int rk3588_its_big_cpus = 1;
static int rk3588_its_cache_quirks = 1;

SYSCTL_INT(_hw, OID_AUTO, rk3588_its_big_cpus, CTLFLAG_RWTUN,
    &rk3588_its_big_cpus, 0,
    "Prefer Cortex-A76 CPUs for RK3588 ITS LPIs");
SYSCTL_INT(_hw, OID_AUTO, rk3588_its_cache_quirks, CTLFLAG_RWTUN,
    &rk3588_its_cache_quirks, 0,
    "Enable RK3588 ITS command/LPI cache writeback quirks");

static bool
rk3588_its_match(device_t dev __unused)
{
#ifdef FDT
	phandle_t root;

	root = OF_finddevice("/");
	if (root <= 0)
		return (false);

	return (ofw_bus_node_is_compatible(root, "rockchip,rk3588") ||
	    ofw_bus_node_is_compatible(root, "rockchip,rk3588s"));
#else
	return (false);
#endif
}

static void
rk3588_its_apply_quirks(device_t dev __unused, u_int *flags)
{

	if (rk3588_its_cache_quirks == 0)
		return;

	*flags |= GICV3_ITS_SOC_FLAG_CMDQ_FLUSH |
	    GICV3_ITS_SOC_FLAG_LPI_CONF_FLUSH |
	    GICV3_ITS_SOC_FLAG_FORCE_CACHE_FLUSH |
	    GICV3_ITS_SOC_FLAG_FORCE_NOSHAREABLE;
}

static bool
rk3588_its_select_cpus(device_t dev __unused, const cpuset_t *available,
    cpuset_t *selected)
{
	cpuset_t big_cpus;
	int cpu;

	if (rk3588_its_big_cpus == 0)
		return (false);

	CPU_ZERO(&big_cpus);
	for (cpu = 4; cpu <= 7; cpu++) {
		if (CPU_ISSET(cpu, available))
			CPU_SET(cpu, &big_cpus);
	}
	if (CPU_EMPTY(&big_cpus))
		return (false);

	CPU_COPY(&big_cpus, selected);
	return (true);
}

static const struct gicv3_its_soc_ops rk3588_its_soc_ops = {
	.match = rk3588_its_match,
	.apply_quirks = rk3588_its_apply_quirks,
	.select_cpus = rk3588_its_select_cpus,
};

static void
rk3588_its_register(void *arg __unused)
{

	gicv3_its_soc_register(&rk3588_its_soc_ops);
}
SYSINIT(rk3588_its_soc, SI_SUB_DRIVERS, SI_ORDER_MIDDLE,
    rk3588_its_register, NULL);
