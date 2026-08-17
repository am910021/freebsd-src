/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 *
 * Minimal RK3588 DFI/DDRMON support for platform bring-up.
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/resource.h>
#include <sys/rman.h>
#include <sys/systm.h>
#include <sys/sysctl.h>

#include <machine/bus.h>

#include <dev/extres/syscon/syscon.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include "syscon_if.h"

#define	RK3588_DFI_DIAG			"RK3588-DIAG V294 dfi"

#define	DMC_MAX_CHANNELS		4

#define	DDRMON_VERSION			0x0000
#define	DDRMON_CTRL			0x0004
#define	 DDRMON_CTRL_LPDDR5		(1u << 6)
#define	 DDRMON_CTRL_DDR4		(1u << 5)
#define	 DDRMON_CTRL_LPDDR4		(1u << 4)
#define	 DDRMON_CTRL_HARDWARE_EN	(1u << 3)
#define	 DDRMON_CTRL_LPDDR23		(1u << 2)
#define	 DDRMON_CTRL_SOFTWARE_EN	(1u << 1)
#define	 DDRMON_CTRL_TIMER_CNT_EN	(1u << 0)
#define	 DDRMON_CTRL_LP5_BANK_MODE_MASK	(0x3u << 7)

#define	DDRMON_CH0_WR_NUM		0x0020
#define	DDRMON_CH0_RD_NUM		0x0024
#define	DDRMON_CH0_COUNT_NUM		0x0028
#define	DDRMON_CH0_DFI_ACCESS_NUM	0x002c

#define	RK3588_PMUGRF_OS_REG2		0x0208
#define	 OS_REG2_BW_CH0			(0x3u << 2)
#define	 OS_REG2_DRAMTYPE_INFO		(0x7u << 13)
#define	 OS_REG2_BW_CH1			(0x3u << 18)
#define	 OS_REG2_CH_INFO		(0x3u << 28)
#define	RK3588_PMUGRF_OS_REG3		0x020c
#define	 OS_REG3_DRAMTYPE_INFO_V3	(0x3u << 12)
#define	 OS_REG3_SYSREG_VERSION		(0xfu << 28)
#define	RK3588_PMUGRF_OS_REG4		0x0210
#define	RK3588_PMUGRF_OS_REG6		0x0218
#define	 OS_REG6_LP5_CKR		(1u << 0)
#define	 OS_REG6_LP5_BANK_MODE		(0x3u << 1)

#define	ROCKCHIP_DDRTYPE_DDR3		3
#define	ROCKCHIP_DDRTYPE_LPDDR2		5
#define	ROCKCHIP_DDRTYPE_LPDDR3		6
#define	ROCKCHIP_DDRTYPE_LPDDR4		7
#define	ROCKCHIP_DDRTYPE_LPDDR4X	8
#define	ROCKCHIP_DDRTYPE_LPDDR5		9

struct rk3588_dfi_softc {
	device_t		dev;
	struct resource		*res;
	struct syscon		*pmu;
	uint32_t		ddrmon_version;
	uint32_t		ddr_type;
	uint32_t		channel_mask;
	uint32_t		sysreg_version;
	uint32_t		lp5_bank_mode;
	uint32_t		count_multiplier;
	bool			lp5_ckr;
	int			buswidth[DMC_MAX_CHANNELS];
	int			ddrmon_stride;
};

static uint32_t
rk3588_dfi_field_get(uint32_t mask, uint32_t val)
{

	return ((val & mask) >> __builtin_ctz(mask));
}

static uint32_t
rk3588_dfi_wm16(uint32_t mask, uint32_t val)
{

	return ((mask << 16) | ((val << __builtin_ctz(mask)) & mask));
}

static uint32_t
rk3588_dfi_read_4(struct rk3588_dfi_softc *sc, bus_size_t off)
{

	return (bus_read_4(sc->res, off));
}

static void
rk3588_dfi_write_4(struct rk3588_dfi_softc *sc, bus_size_t off, uint32_t val)
{

	bus_write_4(sc->res, off, val);
}

static const char *
rk3588_dfi_ddr_type_name(uint32_t type)
{

	switch (type) {
	case ROCKCHIP_DDRTYPE_DDR3:
		return ("DDR3");
	case ROCKCHIP_DDRTYPE_LPDDR2:
		return ("LPDDR2");
	case ROCKCHIP_DDRTYPE_LPDDR3:
		return ("LPDDR3");
	case ROCKCHIP_DDRTYPE_LPDDR4:
		return ("LPDDR4");
	case ROCKCHIP_DDRTYPE_LPDDR4X:
		return ("LPDDR4X");
	case ROCKCHIP_DDRTYPE_LPDDR5:
		return ("LPDDR5");
	default:
		return ("unknown");
	}
}

static int
rk3588_dfi_ctrl_for_ddr_type(struct rk3588_dfi_softc *sc, uint32_t *ctrl)
{

	switch (sc->ddr_type) {
	case ROCKCHIP_DDRTYPE_LPDDR2:
	case ROCKCHIP_DDRTYPE_LPDDR3:
		*ctrl = rk3588_dfi_wm16(DDRMON_CTRL_LPDDR23, 1) |
		    rk3588_dfi_wm16(DDRMON_CTRL_LPDDR4, 0) |
		    rk3588_dfi_wm16(DDRMON_CTRL_LPDDR5, 0);
		return (0);
	case ROCKCHIP_DDRTYPE_LPDDR4:
	case ROCKCHIP_DDRTYPE_LPDDR4X:
		*ctrl = rk3588_dfi_wm16(DDRMON_CTRL_LPDDR23, 0) |
		    rk3588_dfi_wm16(DDRMON_CTRL_LPDDR4, 1) |
		    rk3588_dfi_wm16(DDRMON_CTRL_LPDDR5, 0);
		return (0);
	case ROCKCHIP_DDRTYPE_LPDDR5:
		if (sc->ddrmon_version >= 0x40) {
			device_printf(sc->dev,
			    "%s unsupported LPDDR5 DDRMON version %#x\n",
			    RK3588_DFI_DIAG, sc->ddrmon_version);
			return (EOPNOTSUPP);
		}
		*ctrl = rk3588_dfi_wm16(DDRMON_CTRL_LPDDR23, 0) |
		    rk3588_dfi_wm16(DDRMON_CTRL_LPDDR4, 0) |
		    rk3588_dfi_wm16(DDRMON_CTRL_LPDDR5, 1) |
		    rk3588_dfi_wm16(DDRMON_CTRL_LP5_BANK_MODE_MASK,
		    sc->lp5_bank_mode);
		return (0);
	default:
		device_printf(sc->dev, "%s unsupported DDR type %#x\n",
		    RK3588_DFI_DIAG, sc->ddr_type);
		return (EOPNOTSUPP);
	}
}

static void
rk3588_dfi_read_pmu(struct rk3588_dfi_softc *sc)
{
	uint32_t reg2, reg3, reg4, reg6;

	reg2 = SYSCON_READ_4(sc->pmu, RK3588_PMUGRF_OS_REG2);
	reg3 = SYSCON_READ_4(sc->pmu, RK3588_PMUGRF_OS_REG3);
	reg4 = SYSCON_READ_4(sc->pmu, RK3588_PMUGRF_OS_REG4);

	sc->ddr_type = rk3588_dfi_field_get(OS_REG2_DRAMTYPE_INFO, reg2);
	sc->sysreg_version = rk3588_dfi_field_get(OS_REG3_SYSREG_VERSION, reg3);
	if (sc->sysreg_version >= 0x3)
		sc->ddr_type |= rk3588_dfi_field_get(
		    OS_REG3_DRAMTYPE_INFO_V3, reg3) << 3;

	sc->buswidth[0] = rk3588_dfi_field_get(OS_REG2_BW_CH0, reg2) == 0 ?
	    4 : 2;
	sc->buswidth[1] = rk3588_dfi_field_get(OS_REG2_BW_CH1, reg2) == 0 ?
	    4 : 2;
	sc->buswidth[2] = rk3588_dfi_field_get(OS_REG2_BW_CH0, reg4) == 0 ?
	    4 : 2;
	sc->buswidth[3] = rk3588_dfi_field_get(OS_REG2_BW_CH1, reg4) == 0 ?
	    4 : 2;
	sc->channel_mask = rk3588_dfi_field_get(OS_REG2_CH_INFO, reg2) |
	    (rk3588_dfi_field_get(OS_REG2_CH_INFO, reg4) << 2);
	sc->ddrmon_stride = 0x4000;
	sc->count_multiplier = 2;

	if (sc->ddr_type == ROCKCHIP_DDRTYPE_LPDDR5) {
		reg6 = SYSCON_READ_4(sc->pmu, RK3588_PMUGRF_OS_REG6);
		sc->lp5_bank_mode = rk3588_dfi_field_get(
		    OS_REG6_LP5_BANK_MODE, reg6) << 7;
		sc->lp5_ckr = rk3588_dfi_field_get(OS_REG6_LP5_CKR, reg6) != 0;
		if (sc->lp5_ckr)
			sc->count_multiplier *= 2;
	}
}

static int
rk3588_dfi_enable_counter(struct rk3588_dfi_softc *sc)
{
	uint32_t ctrl;
	int error;
	int i;

	error = rk3588_dfi_ctrl_for_ddr_type(sc, &ctrl);
	if (error != 0)
		return (error);

	for (i = 0; i < DMC_MAX_CHANNELS; i++) {
		if ((sc->channel_mask & (1u << i)) == 0)
			continue;
		rk3588_dfi_write_4(sc, i * sc->ddrmon_stride + DDRMON_CTRL,
		    rk3588_dfi_wm16(DDRMON_CTRL_TIMER_CNT_EN, 0) |
		    rk3588_dfi_wm16(DDRMON_CTRL_SOFTWARE_EN, 0) |
		    rk3588_dfi_wm16(DDRMON_CTRL_HARDWARE_EN, 0));
		rk3588_dfi_write_4(sc, i * sc->ddrmon_stride + DDRMON_CTRL,
		    ctrl);
		rk3588_dfi_write_4(sc, i * sc->ddrmon_stride + DDRMON_CTRL,
		    rk3588_dfi_wm16(DDRMON_CTRL_SOFTWARE_EN, 1));
	}

	return (0);
}

static int
rk3588_dfi_sysctl_counters(SYSCTL_HANDLER_ARGS)
{
	struct rk3588_dfi_softc *sc;
	char buf[512];
	char *p;
	size_t left;
	uint32_t access, cycles, rd, wr;
	int i, n;

	sc = arg1;
	p = buf;
	left = sizeof(buf);
	for (i = 0; i < DMC_MAX_CHANNELS; i++) {
		if ((sc->channel_mask & (1u << i)) == 0)
			continue;
		rd = rk3588_dfi_read_4(sc, i * sc->ddrmon_stride +
		    DDRMON_CH0_RD_NUM);
		wr = rk3588_dfi_read_4(sc, i * sc->ddrmon_stride +
		    DDRMON_CH0_WR_NUM);
		cycles = rk3588_dfi_read_4(sc, i * sc->ddrmon_stride +
		    DDRMON_CH0_COUNT_NUM);
		access = rk3588_dfi_read_4(sc, i * sc->ddrmon_stride +
		    DDRMON_CH0_DFI_ACCESS_NUM);
		n = snprintf(p, left,
		    "%sch%d rd=%u wr=%u access=%u cycles=%u",
		    p == buf ? "" : " ", i, rd, wr, access, cycles);
		if (n < 0 || (size_t)n >= left)
			break;
		p += n;
		left -= n;
	}

	return (sysctl_handle_string(oidp, buf, sizeof(buf), req));
}

static void
rk3588_dfi_add_sysctls(struct rk3588_dfi_softc *sc)
{
	struct sysctl_ctx_list *ctx;
	struct sysctl_oid_list *children;

	ctx = device_get_sysctl_ctx(sc->dev);
	children = SYSCTL_CHILDREN(device_get_sysctl_tree(sc->dev));

	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "ddrmon_version",
	    CTLFLAG_RD, &sc->ddrmon_version, 0, "DDRMON version register");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "ddr_type",
	    CTLFLAG_RD, &sc->ddr_type, 0, "Rockchip DDR type value");
	SYSCTL_ADD_STRING(ctx, children, OID_AUTO, "ddr_type_name",
	    CTLFLAG_RD, __DECONST(char *, rk3588_dfi_ddr_type_name(sc->ddr_type)),
	    0, "Rockchip DDR type name");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "channel_mask",
	    CTLFLAG_RD, &sc->channel_mask, 0, "Active DFI channel mask");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "buswidth_ch0",
	    CTLFLAG_RD, &sc->buswidth[0], 0, "Channel 0 bus width in bytes");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "buswidth_ch1",
	    CTLFLAG_RD, &sc->buswidth[1], 0, "Channel 1 bus width in bytes");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "buswidth_ch2",
	    CTLFLAG_RD, &sc->buswidth[2], 0, "Channel 2 bus width in bytes");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "buswidth_ch3",
	    CTLFLAG_RD, &sc->buswidth[3], 0, "Channel 3 bus width in bytes");
	SYSCTL_ADD_PROC(ctx, children, OID_AUTO, "counters",
	    CTLTYPE_STRING | CTLFLAG_RD, sc, 0, rk3588_dfi_sysctl_counters,
	    "A", "Raw DDRMON counters");
}

static int
rk3588_dfi_probe(device_t dev)
{

	if (!ofw_bus_status_okay(dev))
		return (ENXIO);
	if (!ofw_bus_is_compatible(dev, "rockchip,rk3588-dfi"))
		return (ENXIO);

	device_set_desc(dev, "Rockchip RK3588 DFI DDR Monitor");
	return (BUS_PROBE_DEFAULT);
}

static int
rk3588_dfi_attach(device_t dev)
{
	struct rk3588_dfi_softc *sc;
	phandle_t node;
	int error;
	int rid;

	sc = device_get_softc(dev);
	sc->dev = dev;
	node = ofw_bus_get_node(dev);

	rid = 0;
	sc->res = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid, RF_ACTIVE);
	if (sc->res == NULL) {
		device_printf(dev, "Cannot allocate memory resource\n");
		return (ENXIO);
	}

	error = syscon_get_by_ofw_property(dev, node, "rockchip,pmu",
	    &sc->pmu);
	if (error != 0) {
		device_printf(dev, "Cannot get PMU syscon: %d\n", error);
		goto fail;
	}

	sc->ddrmon_version = rk3588_dfi_read_4(sc, DDRMON_VERSION);
	rk3588_dfi_read_pmu(sc);

	error = rk3588_dfi_enable_counter(sc);
	if (error != 0)
		goto fail;

	device_printf(dev,
	    "%s attached version=%#x ddr=%s(%u) chmask=%#x width=%d/%d/%d/%d mult=%u\n",
	    RK3588_DFI_DIAG, sc->ddrmon_version,
	    rk3588_dfi_ddr_type_name(sc->ddr_type), sc->ddr_type,
	    sc->channel_mask, sc->buswidth[0], sc->buswidth[1],
	    sc->buswidth[2], sc->buswidth[3], sc->count_multiplier);

	rk3588_dfi_add_sysctls(sc);
	return (0);

fail:
	bus_release_resource(dev, SYS_RES_MEMORY, 0, sc->res);
	return (error);
}

static int
rk3588_dfi_detach(device_t dev)
{
	struct rk3588_dfi_softc *sc;

	sc = device_get_softc(dev);
	if (sc->res != NULL)
		bus_release_resource(dev, SYS_RES_MEMORY, 0, sc->res);

	return (0);
}

static device_method_t rk3588_dfi_methods[] = {
	DEVMETHOD(device_probe,		rk3588_dfi_probe),
	DEVMETHOD(device_attach,	rk3588_dfi_attach),
	DEVMETHOD(device_detach,	rk3588_dfi_detach),

	DEVMETHOD_END
};

static driver_t rk3588_dfi_driver = {
	"rk3588_dfi",
	rk3588_dfi_methods,
	sizeof(struct rk3588_dfi_softc),
};

EARLY_DRIVER_MODULE(rk3588_dfi, simplebus, rk3588_dfi_driver, 0, 0,
    BUS_PASS_DEFAULT);
MODULE_VERSION(rk3588_dfi, 1);
