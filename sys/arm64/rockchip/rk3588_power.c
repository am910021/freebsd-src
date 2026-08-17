/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 *
 * Minimal RK3588 power-domain support for early board bring-up.
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#include <dev/ofw/ofw_subr.h>
#include <dev/psci/smccc.h>

#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_kern.h>
#include <vm/pmap.h>

#include "rk3588_power.h"

#define	RK3588_PMU_BASE			0xfd8d8000
#define	RK3588_PMU_SIZE			0x400
#define	RK3588_PMU_PWR_OFFSET		0x14c
#define	RK3588_PMU_STATUS_OFFSET	0x180
#define	RK3588_PMU_REQ_OFFSET		0x10c
#define	RK3588_PMU_IDLE_OFFSET		0x120
#define	RK3588_PMU_ACK_OFFSET		0x118
#define	RK3588_PMU_MEM_STATUS_OFFSET	0x1f8
#define	RK3588_PMU_REPAIR_STATUS	0x290

#define	ROCKCHIP_SIP_SUSPEND_MODE	0x82000003
#define	ROCKCHIP_SLEEP_PD_CONFIG	0xff
#define	RK3588_POWER_UNKNOWN		(-1)
#define	RK3588_POWER_SUMMARY_LEN	1024
#define	RK3588_POWER_CONSUMERS_LEN	4096

#define	RK3588_POWER_QOS_PRIORITY	0x08
#define	RK3588_POWER_QOS_MODE		0x0c
#define	RK3588_POWER_QOS_BANDWIDTH	0x10
#define	RK3588_POWER_QOS_SATURATION	0x14
#define	RK3588_POWER_QOS_EXTCONTROL	0x18
#define	RK3588_POWER_QOS_REG_COUNT	5

#ifndef BIT
#define	BIT(x)				(1U << (x))
#endif

struct rk3588_power_domain {
	uint32_t	id;
	const char	*name;
	uint32_t	pwr_offset;
	uint32_t	pwr_mask;
	uint32_t	status_mask;
	uint32_t	mem_offset;
	uint32_t	mem_status_mask;
	uint32_t	repair_mask;
	uint32_t	req_offset;
	uint32_t	req_mask;
	uint32_t	idle_mask;
	uint32_t	ack_mask;
	bool		notify_bl31;
	bool		need_regulator;
};

struct rk3588_power_softc {
	device_t	dev;
	phandle_t	node;
	phandle_t	xref;
	char		domain_summary[RK3588_POWER_SUMMARY_LEN];
	char		consumer_summary[RK3588_POWER_CONSUMERS_LEN];
	uint64_t	consumer_mask;
	uint32_t	consumer_count;
	uint32_t	request_count;
	uint32_t	pwr0;
	uint32_t	pwr1;
	uint32_t	status0;
	uint32_t	status1;
	uint32_t	req0;
	uint32_t	req1;
	uint32_t	idle0;
	uint32_t	idle1;
	uint32_t	ack0;
	uint32_t	ack1;
	uint32_t	mem_status0;
	uint32_t	mem_status1;
	uint32_t	repair_status;
};

struct rk3588_power_qos_entry {
	uint64_t	addr;
	uint64_t	size;
	uint32_t	regs[RK3588_POWER_QOS_REG_COUNT];
	bool		valid;
};

struct rk3588_power_qos_state {
	struct rk3588_power_qos_entry *entry;
	int		count;
	int		valid_count;
};

#define	RK3588_DOMAIN(_id, _name, _poff, _pwr, _status, _moff, _mstatus, \
	    _repair, _roff, _req, _idle, _notify, _regulator)		\
	{								\
		.id = (_id),						\
		.name = (_name),					\
		.pwr_offset = (_poff),					\
		.pwr_mask = (_pwr),					\
		.status_mask = (_status),				\
		.mem_offset = (_moff),					\
		.mem_status_mask = (_mstatus),				\
		.repair_mask = (_repair),				\
		.req_offset = (_roff),					\
		.req_mask = (_req),					\
		.idle_mask = (_idle),					\
		.ack_mask = (_idle),					\
		.notify_bl31 = (_notify),				\
		.need_regulator = (_regulator),				\
	}

static const struct rk3588_power_domain rk3588_power_domains[] = {
	RK3588_DOMAIN(RK3588_PD_GPU, "gpu", 0x0, BIT(0), 0, 0x0, 0,
	    BIT(1), 0x0, BIT(0), BIT(0), false, true),
	RK3588_DOMAIN(RK3588_PD_NPU, "npu", 0x0, BIT(1), BIT(1), 0x0, 0,
	    0, 0x0, 0, 0, false, true),
	RK3588_DOMAIN(RK3588_PD_VCODEC, "vcodec", 0x0, BIT(2), BIT(2),
	    0x0, 0, 0, 0x0, 0, 0, false, true),
	RK3588_DOMAIN(RK3588_PD_NPUTOP, "nputop", 0x0, BIT(3), 0, 0x0,
	    BIT(11), BIT(2), 0x0, BIT(1), BIT(1), false, false),
	RK3588_DOMAIN(RK3588_PD_NPU1, "npu1", 0x0, BIT(4), 0, 0x0,
	    BIT(12), BIT(3), 0x0, BIT(2), BIT(2), false, false),
	RK3588_DOMAIN(RK3588_PD_NPU2, "npu2", 0x0, BIT(5), 0, 0x0,
	    BIT(13), BIT(4), 0x0, BIT(3), BIT(3), false, false),
	RK3588_DOMAIN(RK3588_PD_VENC0, "venc0", 0x0, BIT(6), 0, 0x0,
	    BIT(14), BIT(5), 0x0, BIT(4), BIT(4), false, false),
	RK3588_DOMAIN(RK3588_PD_VENC1, "venc1", 0x0, BIT(7), 0, 0x0,
	    BIT(15), BIT(6), 0x0, BIT(5), BIT(5), false, false),
	RK3588_DOMAIN(RK3588_PD_RKVDEC0, "rkvdec0", 0x0, BIT(8), 0, 0x0,
	    BIT(16), BIT(7), 0x0, BIT(6), BIT(6), false, false),
	RK3588_DOMAIN(RK3588_PD_RKVDEC1, "rkvdec1", 0x0, BIT(9), 0, 0x0,
	    BIT(17), BIT(8), 0x0, BIT(7), BIT(7), false, false),
	RK3588_DOMAIN(RK3588_PD_VDPU, "vdpu", 0x0, BIT(10), 0, 0x0,
	    BIT(18), BIT(9), 0x0, BIT(8), BIT(8), false, false),
	RK3588_DOMAIN(RK3588_PD_RGA30, "rga30", 0x0, BIT(11), 0, 0x0,
	    BIT(19), BIT(10), 0x0, 0, 0, false, false),
	RK3588_DOMAIN(RK3588_PD_AV1, "av1", 0x0, BIT(12), 0, 0x0,
	    BIT(20), BIT(11), 0x0, BIT(9), BIT(9), false, false),
	RK3588_DOMAIN(RK3588_PD_VI, "vi", 0x0, BIT(13), 0, 0x0,
	    BIT(21), BIT(12), 0x0, BIT(10), BIT(10), false, false),
	RK3588_DOMAIN(RK3588_PD_FEC, "fec", 0x0, BIT(14), 0, 0x0,
	    BIT(22), BIT(13), 0x0, 0, 0, false, false),
	RK3588_DOMAIN(RK3588_PD_ISP1, "isp1", 0x0, BIT(15), 0, 0x0,
	    BIT(23), BIT(14), 0x0, BIT(11), BIT(11), false, false),
	RK3588_DOMAIN(RK3588_PD_RGA31, "rga31", 0x4, BIT(0), 0, 0x0,
	    BIT(24), BIT(15), 0x0, BIT(12), BIT(12), false, false),
	RK3588_DOMAIN(RK3588_PD_VOP, "vop", 0x4, BIT(1), 0, 0x0,
	    BIT(25), BIT(16), 0x0, BIT(13) | BIT(14),
	    BIT(13) | BIT(14), false, false),
	RK3588_DOMAIN(RK3588_PD_VO0, "vo0", 0x4, BIT(2), 0, 0x0,
	    BIT(26), BIT(17), 0x0, BIT(15), BIT(15), false, false),
	RK3588_DOMAIN(RK3588_PD_VO1, "vo1", 0x4, BIT(3), 0, 0x0,
	    BIT(27), BIT(18), 0x4, BIT(0), BIT(16), false, false),
	RK3588_DOMAIN(RK3588_PD_AUDIO, "audio", 0x4, BIT(4), 0, 0x0,
	    BIT(28), BIT(19), 0x4, BIT(1), BIT(17), false, false),
	RK3588_DOMAIN(RK3588_PD_PHP, "php", 0x4, BIT(5), 0, 0x0,
	    BIT(29), BIT(20), 0x4, BIT(5), BIT(21), false, false),
	RK3588_DOMAIN(RK3588_PD_GMAC, "gmac", 0x4, BIT(6), 0, 0x0,
	    BIT(30), BIT(21), 0x0, 0, 0, false, false),
	RK3588_DOMAIN(RK3588_PD_PCIE, "pcie", 0x4, BIT(7), 0, 0x0,
	    BIT(31), BIT(22), 0x0, 0, 0, true, false),
	RK3588_DOMAIN(RK3588_PD_NVM, "nvm", 0x4, BIT(8), BIT(24), 0x4,
	    0, 0, 0x4, BIT(2), BIT(18), false, false),
	RK3588_DOMAIN(RK3588_PD_NVM0, "nvm0", 0x4, BIT(9), 0, 0x4,
	    BIT(1), BIT(23), 0x0, 0, 0, false, false),
	RK3588_DOMAIN(RK3588_PD_SDIO, "sdio", 0x4, BIT(10), 0, 0x4,
	    BIT(2), BIT(24), 0x4, BIT(3), BIT(19), false, false),
	RK3588_DOMAIN(RK3588_PD_USB, "usb", 0x4, BIT(11), 0, 0x4,
	    BIT(3), BIT(25), 0x4, BIT(4), BIT(20), true, false),
	RK3588_DOMAIN(RK3588_PD_SDMMC, "sdmmc", 0x4, BIT(13), 0, 0x4,
	    BIT(5), BIT(26), 0x0, 0, 0, false, false),
};

static const struct rk3588_power_domain *
rk3588_power_find(uint32_t domain_id)
{
	size_t i;

	for (i = 0; i < nitems(rk3588_power_domains); i++) {
		if (rk3588_power_domains[i].id == domain_id)
			return (&rk3588_power_domains[i]);
	}
	return (NULL);
}

static volatile uint32_t *
rk3588_power_map(device_t dev, void **cookie)
{
	void *pmu_mem;

	pmu_mem = pmap_mapdev(RK3588_PMU_BASE, RK3588_PMU_SIZE);
	if (pmu_mem == NULL) {
		device_printf(dev, "failed to map PMU\n");
		return (NULL);
	}

	*cookie = pmu_mem;
	return ((volatile uint32_t *)pmu_mem);
}

static uint32_t
rk3588_power_read(volatile uint32_t *pmu, uint32_t offset)
{

	return (pmu[offset / sizeof(uint32_t)]);
}

static int
rk3588_power_domain_on(volatile uint32_t *pmu,
    const struct rk3588_power_domain *pd)
{
	uint32_t reg;

	if (pd->repair_mask != 0) {
		reg = rk3588_power_read(pmu, RK3588_PMU_REPAIR_STATUS);
		return ((reg & pd->repair_mask) != 0);
	}

	if (pd->status_mask != 0) {
		reg = rk3588_power_read(pmu,
		    RK3588_PMU_STATUS_OFFSET + pd->pwr_offset);
		return ((reg & pd->status_mask) == 0);
	}

	if (pd->idle_mask != 0) {
		reg = rk3588_power_read(pmu,
		    RK3588_PMU_IDLE_OFFSET + pd->req_offset);
		return ((reg & pd->idle_mask) == 0);
	}

	return (RK3588_POWER_UNKNOWN);
}

static void
rk3588_power_update_snapshot(struct rk3588_power_softc *sc,
    volatile uint32_t *pmu)
{
	const struct rk3588_power_domain *pd;
	const char *state;
	size_t len;
	int on;

	sc->pwr0 = rk3588_power_read(pmu, RK3588_PMU_PWR_OFFSET);
	sc->pwr1 = rk3588_power_read(pmu, RK3588_PMU_PWR_OFFSET + 0x4);
	sc->status0 = rk3588_power_read(pmu, RK3588_PMU_STATUS_OFFSET);
	sc->status1 = rk3588_power_read(pmu, RK3588_PMU_STATUS_OFFSET + 0x4);
	sc->req0 = rk3588_power_read(pmu, RK3588_PMU_REQ_OFFSET);
	sc->req1 = rk3588_power_read(pmu, RK3588_PMU_REQ_OFFSET + 0x4);
	sc->idle0 = rk3588_power_read(pmu, RK3588_PMU_IDLE_OFFSET);
	sc->idle1 = rk3588_power_read(pmu, RK3588_PMU_IDLE_OFFSET + 0x4);
	sc->ack0 = rk3588_power_read(pmu, RK3588_PMU_ACK_OFFSET);
	sc->ack1 = rk3588_power_read(pmu, RK3588_PMU_ACK_OFFSET + 0x4);
	sc->mem_status0 = rk3588_power_read(pmu, RK3588_PMU_MEM_STATUS_OFFSET);
	sc->mem_status1 = rk3588_power_read(pmu,
	    RK3588_PMU_MEM_STATUS_OFFSET + 0x4);
	sc->repair_status = rk3588_power_read(pmu,
	    RK3588_PMU_REPAIR_STATUS);

	sc->domain_summary[0] = '\0';
	len = 0;
	for (size_t i = 0; i < nitems(rk3588_power_domains); i++) {
		pd = &rk3588_power_domains[i];
		on = rk3588_power_domain_on(pmu, pd);
		state = (on == RK3588_POWER_UNKNOWN) ? "?" :
		    (on != 0 ? "on" : "off");
		len += snprintf(sc->domain_summary + len,
		    sizeof(sc->domain_summary) - len, "%s%s:%s",
		    len == 0 ? "" : " ", pd->name, state);
		if (len >= sizeof(sc->domain_summary))
			break;
	}
}

static void
rk3588_power_consumer_append(struct rk3588_power_softc *sc,
    const char *node_name, uint32_t domain_id)
{
	const struct rk3588_power_domain *pd;
	const char *domain_name;
	size_t len;

	len = strlen(sc->consumer_summary);
	if (len >= sizeof(sc->consumer_summary) - 1)
		return;

	pd = rk3588_power_find(domain_id);
	domain_name = (pd != NULL) ? pd->name : "?";
	snprintf(sc->consumer_summary + len,
	    sizeof(sc->consumer_summary) - len, "%s%s:%u/%s",
	    len == 0 ? "" : " ", node_name, domain_id, domain_name);
}

static void
rk3588_power_scan_node(struct rk3588_power_softc *sc, phandle_t node)
{
	uint32_t *domains;
	ssize_t count;
	char node_name[64];
	bool node_matched;

	for (; node > 0; node = OF_peer(node)) {
		rk3588_power_scan_node(sc, OF_child(node));

		count = OF_getencprop_alloc_multi(node, "power-domains",
		    sizeof(*domains), (void **)&domains);
		if (count <= 0)
			continue;

		memset(node_name, 0, sizeof(node_name));
		if (OF_getprop(node, "name", node_name, sizeof(node_name)) <= 0)
			snprintf(node_name, sizeof(node_name), "node%jx",
			    (uintmax_t)node);

		node_matched = false;
		for (ssize_t i = 0; i + 1 < count; i += 2) {
			if (domains[i] != sc->xref)
				continue;
			node_matched = true;
			sc->request_count++;
			if (domains[i + 1] < 64)
				sc->consumer_mask |= (1ULL << domains[i + 1]);
			rk3588_power_consumer_append(sc, node_name,
			    domains[i + 1]);
		}
		if (node_matched)
			sc->consumer_count++;

		OF_prop_free(domains);
	}
}

static void
rk3588_power_scan_consumers(struct rk3588_power_softc *sc)
{
	phandle_t root;

	sc->consumer_mask = 0;
	sc->consumer_count = 0;
	sc->request_count = 0;
	sc->consumer_summary[0] = '\0';

	root = OF_peer(0);
	if (root == 0 || sc->xref == 0)
		return;

	rk3588_power_scan_node(sc, root);
	if (sc->consumer_summary[0] == '\0')
		snprintf(sc->consumer_summary, sizeof(sc->consumer_summary),
		    "none");
}

static void
rk3588_power_add_sysctls(struct rk3588_power_softc *sc)
{
	struct sysctl_ctx_list *ctx;
	struct sysctl_oid_list *children;
	device_t dev;

	dev = sc->dev;
	ctx = device_get_sysctl_ctx(dev);
	children = SYSCTL_CHILDREN(device_get_sysctl_tree(dev));

	SYSCTL_ADD_STRING(ctx, children, OID_AUTO, "domains", CTLFLAG_RD,
	    sc->domain_summary, 0, "RK3588 power-domain status summary");
	SYSCTL_ADD_STRING(ctx, children, OID_AUTO, "consumers", CTLFLAG_RD,
	    sc->consumer_summary, 0, "DT power-domains consumers");
	SYSCTL_ADD_U64(ctx, children, OID_AUTO, "consumer_mask", CTLFLAG_RD,
	    &sc->consumer_mask, 0, "DT power-domain consumer bitmask");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "request_count", CTLFLAG_RD,
	    &sc->request_count, 0, "DT power-domain request count");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "consumer_count", CTLFLAG_RD,
	    &sc->consumer_count, 0, "DT node count using this provider");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "pwr0", CTLFLAG_RD,
	    &sc->pwr0, 0, "PMU pwr register 0");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "pwr1", CTLFLAG_RD,
	    &sc->pwr1, 0, "PMU pwr register 1");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "status0", CTLFLAG_RD,
	    &sc->status0, 0, "PMU status register 0");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "status1", CTLFLAG_RD,
	    &sc->status1, 0, "PMU status register 1");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "req0", CTLFLAG_RD,
	    &sc->req0, 0, "PMU bus idle request register 0");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "req1", CTLFLAG_RD,
	    &sc->req1, 0, "PMU bus idle request register 1");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "idle0", CTLFLAG_RD,
	    &sc->idle0, 0, "PMU bus idle status register 0");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "idle1", CTLFLAG_RD,
	    &sc->idle1, 0, "PMU bus idle status register 1");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "ack0", CTLFLAG_RD,
	    &sc->ack0, 0, "PMU bus idle ack register 0");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "ack1", CTLFLAG_RD,
	    &sc->ack1, 0, "PMU bus idle ack register 1");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "mem_status0", CTLFLAG_RD,
	    &sc->mem_status0, 0, "PMU memory power status register 0");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "mem_status1", CTLFLAG_RD,
	    &sc->mem_status1, 0, "PMU memory power status register 1");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "repair_status", CTLFLAG_RD,
	    &sc->repair_status, 0, "PMU repair status register");
}

static void
rk3588_power_unmap(void *cookie)
{

	pmap_unmapdev(cookie, RK3588_PMU_SIZE);
}

static phandle_t
rk3588_power_domain_child(phandle_t provider, uint32_t domain_id)
{
	phandle_t child;
	uint32_t reg;

	for (child = OF_child(provider); child > 0; child = OF_peer(child)) {
		if (OF_getencprop(child, "reg", &reg, sizeof(reg)) <= 0)
			continue;
		if (reg == domain_id)
			return (child);
	}

	return (0);
}

static int
rk3588_power_qos_map_node(device_t consumer, phandle_t node, int idx,
    struct rk3588_power_qos_entry *entry)
{
	uint32_t reg[4];
	ssize_t len;

	memset(entry, 0, sizeof(*entry));

	len = OF_getencprop(node, "reg", reg, sizeof(reg));
	if (len < (ssize_t)sizeof(reg)) {
		device_printf(consumer,
		    "QoS[%d] has an invalid reg property length: %zd\n",
		    idx, len);
		return (EINVAL);
	}

	entry->addr = ((uint64_t)reg[0] << 32) | reg[1];
	entry->size = ((uint64_t)reg[2] << 32) | reg[3];
	if (entry->size < 0x20 || entry->size > 0x1000) {
		device_printf(consumer,
		    "QoS[%d] has an invalid register size at %#jx: %#jx\n",
		    idx, (uintmax_t)entry->addr,
		    (uintmax_t)entry->size);
		return (EINVAL);
	}

	return (0);
}

static int
rk3588_power_qos_save(device_t consumer, phandle_t domain,
    struct rk3588_power_qos_state *state)
{
	static const uint32_t offsets[RK3588_POWER_QOS_REG_COUNT] = {
		RK3588_POWER_QOS_PRIORITY,
		RK3588_POWER_QOS_MODE,
		RK3588_POWER_QOS_BANDWIDTH,
		RK3588_POWER_QOS_SATURATION,
		RK3588_POWER_QOS_EXTCONTROL,
	};
	struct rk3588_power_qos_entry *entry;
	uint32_t *qos;
	volatile uint32_t *mem;
	phandle_t node;
	void *map;
	ssize_t count;
	int i, j;

	memset(state, 0, sizeof(*state));
	count = OF_getencprop_alloc_multi(domain, "pm_qos", sizeof(*qos),
	    (void **)&qos);
	if (count <= 0)
		return (0);

	state->entry = mallocarray(count, sizeof(*state->entry), M_DEVBUF,
	    M_WAITOK | M_ZERO);
	state->count = count;

	for (i = 0; i < count; i++) {
		node = OF_node_from_xref(qos[i]);
		if (node <= 0)
			continue;
		entry = &state->entry[i];
		if (rk3588_power_qos_map_node(consumer, node, i, entry) != 0)
			continue;
		map = pmap_mapdev(entry->addr, entry->size);
		if (map == NULL) {
			device_printf(consumer,
			    "could not map QoS[%d] registers at %#jx/%#jx\n",
			    i, (uintmax_t)entry->addr,
			    (uintmax_t)entry->size);
			continue;
		}
		mem = map;
		for (j = 0; j < RK3588_POWER_QOS_REG_COUNT; j++)
			entry->regs[j] = mem[offsets[j] / sizeof(uint32_t)];
		pmap_unmapdev(map, entry->size);
		entry->valid = true;
		state->valid_count++;
	}

	OF_prop_free(qos);
	return (0);
}

static void
rk3588_power_qos_restore(device_t consumer,
    const struct rk3588_power_qos_state *state)
{
	static const uint32_t offsets[RK3588_POWER_QOS_REG_COUNT] = {
		RK3588_POWER_QOS_PRIORITY,
		RK3588_POWER_QOS_MODE,
		RK3588_POWER_QOS_BANDWIDTH,
		RK3588_POWER_QOS_SATURATION,
		RK3588_POWER_QOS_EXTCONTROL,
	};
	const struct rk3588_power_qos_entry *entry;
	volatile uint32_t *mem;
	void *map;
	int i, j;

	for (i = 0; i < state->count; i++) {
		entry = &state->entry[i];
		if (!entry->valid)
			continue;
		map = pmap_mapdev(entry->addr, entry->size);
		if (map == NULL) {
			device_printf(consumer,
			    "could not remap QoS[%d] registers at %#jx/%#jx\n",
			    i, (uintmax_t)entry->addr,
			    (uintmax_t)entry->size);
			continue;
		}
		mem = map;
		for (j = 0; j < RK3588_POWER_QOS_REG_COUNT; j++)
			mem[offsets[j] / sizeof(uint32_t)] = entry->regs[j];
		wmb();
		pmap_unmapdev(map, entry->size);
	}
}

static void
rk3588_power_qos_free(struct rk3588_power_qos_state *state)
{

	free(state->entry, M_DEVBUF);
	memset(state, 0, sizeof(*state));
}

static int
rk3588_power_domain_enable_internal(device_t consumer, uint32_t domain_id,
    phandle_t domain)
{
	const struct rk3588_power_domain *pd;
	struct rk3588_power_qos_state qos_state;
	struct arm_smccc_res res;
	volatile uint32_t *pmu;
	void *cookie;
	uint32_t reg_offset, write_mask;
	int i;

	pd = rk3588_power_find(domain_id);
	if (pd == NULL) {
		device_printf(consumer, "unknown power domain id %u\n",
		    domain_id);
		return (ENOENT);
	}

	cookie = NULL;
	pmu = rk3588_power_map(consumer, &cookie);
	if (pmu == NULL)
		return (ENXIO);

	reg_offset = RK3588_PMU_PWR_OFFSET + pd->pwr_offset;
	write_mask = pd->pwr_mask << 16;
	memset(&qos_state, 0, sizeof(qos_state));

	if (domain > 0)
		(void)rk3588_power_qos_save(consumer, domain, &qos_state);

	pmu[reg_offset / sizeof(uint32_t)] = write_mask;
	for (i = 0; i < 1000; i++) {
		if (rk3588_power_domain_on(pmu, pd) != 0)
			break;
		DELAY(10);
	}
	if (i == 1000)
		device_printf(consumer, "timed out enabling %s power domain\n",
		    pd->name);

	rk3588_power_unmap(cookie);
	rk3588_power_qos_restore(consumer, &qos_state);
	rk3588_power_qos_free(&qos_state);

	if (pd->notify_bl31) {
		arm_smccc_invoke_smc(ROCKCHIP_SIP_SUSPEND_MODE,
		    ROCKCHIP_SLEEP_PD_CONFIG, reg_offset, pd->pwr_mask, 1,
		    &res);
	}

	return (0);
}

int
rk3588_power_domain_enable(device_t consumer, uint32_t domain_id)
{

	return (rk3588_power_domain_enable_internal(consumer, domain_id, 0));
}

int
rk3588_power_domain_enable_by_node(device_t consumer, phandle_t node)
{
	uint32_t *domains;
	phandle_t provider, domain;
	ssize_t count;
	int error, rv;

	count = OF_getencprop_alloc_multi(node, "power-domains",
	    sizeof(*domains), (void **)&domains);
	if (count <= 0)
		return (ENOENT);

	rv = ENOENT;
	for (ssize_t i = 0; i + 1 < count; i += 2) {
		provider = OF_node_from_xref(domains[i]);
		if (provider == 0 ||
		    !ofw_bus_node_is_compatible(provider,
		    "rockchip,rk3588-power-controller"))
			continue;

		domain = rk3588_power_domain_child(provider, domains[i + 1]);
		error = rk3588_power_domain_enable_internal(consumer,
		    domains[i + 1], domain);
		if (error != 0) {
			rv = error;
			break;
		}
		rv = 0;
	}

	OF_prop_free(domains);
	return (rv);
}

static int
rk3588_power_probe(device_t dev)
{

	if (!ofw_bus_status_okay(dev))
		return (ENXIO);
	if (!ofw_bus_is_compatible(dev, "rockchip,rk3588-power-controller"))
		return (ENXIO);

	device_set_desc(dev, "RockChip RK3588 Power Domain Controller");
	return (BUS_PROBE_DEFAULT);
}

static int
rk3588_power_attach(device_t dev)
{
	struct rk3588_power_softc *sc;
	volatile uint32_t *pmu;
	void *cookie;

	sc = device_get_softc(dev);
	sc->dev = dev;
	sc->node = ofw_bus_get_node(dev);
	sc->xref = OF_xref_from_node(sc->node);

	cookie = NULL;
	pmu = rk3588_power_map(dev, &cookie);
	if (pmu == NULL)
		return (ENXIO);

	rk3588_power_update_snapshot(sc, pmu);
	rk3588_power_scan_consumers(sc);

	rk3588_power_unmap(cookie);
	rk3588_power_add_sysctls(sc);
	return (0);
}

static device_method_t rk3588_power_methods[] = {
	DEVMETHOD(device_probe,		rk3588_power_probe),
	DEVMETHOD(device_attach,	rk3588_power_attach),

	DEVMETHOD_END
};

DEFINE_CLASS_0(rk3588_power, rk3588_power_driver, rk3588_power_methods,
    sizeof(struct rk3588_power_softc));
EARLY_DRIVER_MODULE(rk3588_power, simplebus, rk3588_power_driver, 0, 0,
    BUS_PASS_BUS + BUS_PASS_ORDER_MIDDLE);
MODULE_VERSION(rk3588_power, 1);
