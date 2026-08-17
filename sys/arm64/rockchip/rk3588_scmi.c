/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 */

#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/malloc.h>

#include <vm/vm.h>
#include <vm/pmap.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#include <dev/firmware/arm/scmi_soc.h>
#include <dev/psci/smccc.h>

struct rk3588_scmi_shmem {
	uint8_t		*va;
	vm_size_t	size;
};

struct rk3588_scmi_transport {
	uint32_t	smc_id;
};

static MALLOC_DEFINE(M_RK3588_SCMI, "rk3588_scmi", "RK3588 SCMI data");

static uint64_t
rk3588_scmi_cells_to_u64(pcell_t *cells, int ncells)
{
	uint64_t val;
	int i;

	val = 0;
	for (i = 0; i < ncells; i++)
		val = (val << 32) | cells[i];

	return (val);
}

static bool
rk3588_scmi_root_match(void)
{
	phandle_t root;

	root = OF_finddevice("/");
	return (root > 0 &&
	    (ofw_bus_node_is_compatible(root, "rockchip,rk3588") ||
	    ofw_bus_node_is_compatible(root, "rockchip,rk3588s")));
}

static bool
rk3588_scmi_probe(device_t dev)
{

	return (rk3588_scmi_root_match() &&
	    ofw_bus_is_compatible(dev, "arm,scmi-smc"));
}

static uint8_t
rk3588_scmi_shmem_read_1(void *arg, bus_size_t offset)
{
	struct rk3588_scmi_shmem *shmem;

	shmem = arg;
	KASSERT(offset < shmem->size, ("%s: offset past shmem", __func__));
	return (shmem->va[offset]);
}

static void
rk3588_scmi_shmem_write_1(void *arg, bus_size_t offset, uint8_t val)
{
	struct rk3588_scmi_shmem *shmem;

	shmem = arg;
	KASSERT(offset < shmem->size, ("%s: offset past shmem", __func__));
	shmem->va[offset] = val;
}

static void
rk3588_scmi_shmem_fini(void *arg)
{
	struct rk3588_scmi_shmem *shmem;

	shmem = arg;
	if (shmem->va != NULL)
		pmap_unmapdev(shmem->va, shmem->size);
	free(shmem, M_RK3588_SCMI);
}

static int
rk3588_scmi_map_shmem(device_t dev, phandle_t node,
    struct scmi_soc_shmem *soc_shmem)
{
	struct rk3588_scmi_shmem *shmem;
	pcell_t *reg;
	phandle_t parent;
	uint64_t addr, size;
	int acells, len, scells;

	if (!OF_hasprop(node, "no-map") ||
	    !ofw_bus_node_is_compatible(node, "arm,scmi-shmem"))
		return (ENXIO);

	parent = OF_parent(node);
	if (OF_getencprop(parent, "#address-cells", &acells,
	    sizeof(acells)) <= 0)
		acells = 2;
	if (OF_getencprop(parent, "#size-cells", &scells,
	    sizeof(scells)) <= 0)
		scells = 1;

	len = OF_getencprop_alloc_multi(node, "reg", sizeof(*reg),
	    (void **)&reg);
	if (len < acells + scells) {
		if (len > 0)
			OF_prop_free(reg);
		return (ENXIO);
	}

	addr = rk3588_scmi_cells_to_u64(reg, acells);
	size = rk3588_scmi_cells_to_u64(reg + acells, scells);
	OF_prop_free(reg);

	if (size == 0 || size > PAGE_SIZE)
		return (ENXIO);

	shmem = malloc(sizeof(*shmem), M_RK3588_SCMI, M_WAITOK | M_ZERO);
	shmem->va = pmap_mapdev(addr, size);
	shmem->size = size;

	soc_shmem->cookie = shmem;
	soc_shmem->read_1 = rk3588_scmi_shmem_read_1;
	soc_shmem->write_1 = rk3588_scmi_shmem_write_1;
	soc_shmem->fini = rk3588_scmi_shmem_fini;

	if (bootverbose)
		device_printf(dev,
		    "mapped reserved SCMI shmem pa=%#jx size=%#jx\n",
		    (uintmax_t)addr, (uintmax_t)size);

	return (0);
}

static bool
rk3588_scmi_has_transport(device_t dev)
{

	return (rk3588_scmi_probe(dev));
}

static int
rk3588_scmi_transport_init(device_t dev, void **cookie)
{
	struct rk3588_scmi_transport *transport;
	phandle_t node;
	uint32_t smc_id;

	node = ofw_bus_get_node(dev);
	if (OF_getencprop(node, "arm,smc-id", &smc_id,
	    sizeof(smc_id)) <= 0) {
		device_printf(dev, "SMC transport missing arm,smc-id.\n");
		return (ENXIO);
	}

	transport = malloc(sizeof(*transport), M_RK3588_SCMI,
	    M_WAITOK | M_ZERO);
	transport->smc_id = smc_id;
	*cookie = transport;

	if (bootverbose)
		device_printf(dev, "using SCMI SMC transport smc-id=%#x\n",
		    transport->smc_id);

	return (0);
}

static int
rk3588_scmi_transport_xfer(device_t dev, void *cookie)
{
	struct arm_smccc_res res;
	struct rk3588_scmi_transport *transport;

	transport = cookie;
	arm_smccc_invoke_smc(transport->smc_id, &res);
	if (res.a0 != SMCCC_RET_SUCCESS) {
		device_printf(dev,
		    "SCMI SMC call failed smc-id=%#x a0=%#jx a1=%#jx\n",
		    transport->smc_id, (uintmax_t)res.a0,
		    (uintmax_t)res.a1);
		return (EIO);
	}

	return (0);
}

static void
rk3588_scmi_transport_fini(device_t dev __unused, void *cookie)
{

	free(cookie, M_RK3588_SCMI);
}

static const struct scmi_soc_ops rk3588_scmi_soc_ops = {
	.probe = rk3588_scmi_probe,
	.map_shmem = rk3588_scmi_map_shmem,
	.has_transport = rk3588_scmi_has_transport,
	.transport_init = rk3588_scmi_transport_init,
	.transport_xfer = rk3588_scmi_transport_xfer,
	.transport_fini = rk3588_scmi_transport_fini,
};

static void
rk3588_scmi_register(void *arg __unused)
{

	scmi_soc_register(&rk3588_scmi_soc_ops);
}

SYSINIT(rk3588_scmi_register, SI_SUB_DRIVERS, SI_ORDER_FIRST,
    rk3588_scmi_register, NULL);
