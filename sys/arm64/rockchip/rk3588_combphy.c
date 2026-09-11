/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 *
 * Minimal RK3588 Naneng combo PHY support for PCIe bring-up.
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/rman.h>
#include <sys/systm.h>

#include <machine/bus.h>

#include <dev/fdt/simple_mfd.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include <dev/extres/clk/clk.h>
#include <dev/extres/hwreset/hwreset.h>
#include <dev/extres/phy/phy.h>
#include <dev/extres/syscon/syscon.h>

#include <contrib/device-tree/include/dt-bindings/phy/phy.h>

#include "syscon_if.h"
#include "phydev_if.h"
#include "phynode_if.h"

#define	REF_CLOCK_24MHZ		24000000ULL
#define	REF_CLOCK_25MHZ		25000000ULL
#define	REF_CLOCK_100MHZ	100000000ULL

#define	RK3568_PHYREG6				0x14
#define	 RK3568_PHYREG6_PLL_DIV_MASK		(0x3 << 6)
#define	 RK3568_PHYREG6_PLL_DIV_2		(0x1 << 6)
#define	RK3568_PHYREG7				0x18
#define	 RK3568_PHYREG7_TX_RTERM_50OHM		(0x8 << 4)
#define	 RK3568_PHYREG7_RX_RTERM_44OHM		0xf
#define	RK3568_PHYREG8				0x20
#define	 RK3568_PHYREG8_SSC_EN			(1 << 4)
#define	RK3588_PHYREG9				0x24
#define	 RK3588_PHYREG9_SSC_PPM_MASK		0xf
#define	 RK3588_PHYREG9_SSC_PPM_3500		0x7
#define	RK3568_PHYREG11				0x28
#define	 RK3568_PHYREG11_SU_TRIM_0_7		0xf0
#define	RK3568_PHYREG12				0x2c
#define	 RK3568_PHYREG12_PLL_LPF_ADJ_VALUE	0x4
#define	RK3568_PHYREG13				0x30
#define	 RK3568_PHYREG13_RESISTER_MASK		(0x3 << 4)
#define	 RK3568_PHYREG13_RESISTER_HIGH_Z	(0x3 << 4)
#define	 RK3568_PHYREG13_CKRCV_AMP0		(1 << 7)
#define	RK3568_PHYREG14				0x34
#define	 RK3568_PHYREG14_CKRCV_AMP1		(1 << 0)
#define	RK3568_PHYREG15				0x38
#define	 RK3568_PHYREG15_CTLE_EN		(1 << 0)
#define	 RK3568_PHYREG15_SSC_CNT_MASK		(0x3 << 6)
#define	 RK3568_PHYREG15_SSC_CNT_VALUE		(0x1 << 6)
#define	RK3568_PHYREG16				0x3c
#define	 RK3568_PHYREG16_SSC_CNT_VALUE		0x5f
#define	RK3568_PHYREG18				0x44
#define	 RK3568_PHYREG18_PLL_LOOP		0x32
#define	RK3588_PHYREG27				0x6c
#define	 RK3588_PHYREG27_RX_TRIM		0x4c
#define	RK3568_PHYREG32				0x7c
#define	 RK3568_PHYREG32_SSC_MASK		(0xf << 4)
#define	 RK3568_PHYREG32_SSC_DOWNWARD		(0x1 << 4)
#define	 RK3568_PHYREG32_SSC_OFFSET_500PPM	(0x1 << 6)
#define	RK3568_PHYREG33				0x80
#define	 RK3568_PHYREG33_PLL_KVCO_MASK		(0x7 << 2)
#define	 RK3568_PHYREG33_PLL_KVCO_VALUE_RK3588	(0x4 << 2)

struct rk3588_combphy_reg {
	uint16_t	offset;
	uint8_t		bitend;
	uint8_t		bitstart;
	uint16_t	disable;
	uint16_t	enable;
};

struct rk3588_combphy_softc {
	device_t		dev;
	phandle_t		node;
	struct resource		*mem;
	struct phynode		*phynode;
	struct syscon		*pipe_grf;
	struct syscon		*pipe_phy_grf;
	clk_t			ref_clk;
	clk_t			apb_clk;
	clk_t			pipe_clk;
	hwreset_t		phy_reset;
	int			id;
	int			mode;
	bool			enable_ssc;
	bool			ext_refclk;
};

static struct ofw_compat_data compat_data[] = {
	{ "rockchip,rk3588-naneng-combphy",	1 },
	{ NULL,					0 }
};

static const struct rk3588_combphy_reg rk3588_combphy_con0_for_pcie =
    { 0x0000, 15, 0, 0x00, 0x1000 };
static const struct rk3588_combphy_reg rk3588_combphy_con1_for_pcie =
    { 0x0004, 15, 0, 0x00, 0x0000 };
static const struct rk3588_combphy_reg rk3588_combphy_con2_for_pcie =
    { 0x0008, 15, 0, 0x00, 0x0101 };
static const struct rk3588_combphy_reg rk3588_combphy_con3_for_pcie =
    { 0x000c, 15, 0, 0x00, 0x0200 };
static const struct rk3588_combphy_reg rk3588_combphy_pipe_clk_100m =
    { 0x0004, 14, 13, 0x00, 0x02 };
static const struct rk3588_combphy_reg rk3588_combphy_pipe_clk_25m =
    { 0x0004, 14, 13, 0x00, 0x01 };
static const struct rk3588_combphy_reg rk3588_combphy_pipe_clk_ext =
    { 0x000c, 9, 8, 0x02, 0x01 };
static const struct rk3588_combphy_reg rk3588_combphy_pipe_pcie1l0_sel =
    { 0x0100, 0, 0, 0x01, 0x00 };
static const struct rk3588_combphy_reg rk3588_combphy_pipe_pcie1l1_sel =
    { 0x0100, 1, 1, 0x01, 0x00 };
static const struct rk3588_combphy_reg rk3588_combphy_con0_for_sata =
    { 0x0000, 15, 0, 0x00, 0x0129 };
static const struct rk3588_combphy_reg rk3588_combphy_con1_for_sata =
    { 0x0004, 15, 0, 0x00, 0x0000 };
static const struct rk3588_combphy_reg rk3588_combphy_con2_for_sata =
    { 0x0008, 15, 0, 0x00, 0x80c1 };
static const struct rk3588_combphy_reg rk3588_combphy_con3_for_sata =
    { 0x000c, 15, 0, 0x00, 0x0407 };
static const struct rk3588_combphy_reg rk3588_combphy_pipe_con0_for_sata =
    { 0x0000, 11, 5, 0x00, 0x22 };
static const struct rk3588_combphy_reg rk3588_combphy_pipe_con1_for_sata =
    { 0x0004, 2, 0, 0x00, 0x02 };

static uint32_t
rk3588_combphy_mask(uint8_t bitend, uint8_t bitstart)
{
	uint32_t width;

	width = bitend - bitstart + 1;
	if (width == 32)
		return (0xffffffffU);
	return (((1U << width) - 1U) << bitstart);
}

static void
rk3588_combphy_param_write(struct syscon *syscon,
    const struct rk3588_combphy_reg *reg, bool enable)
{
	uint32_t mask, raw, val;

	raw = enable ? reg->enable : reg->disable;
	mask = rk3588_combphy_mask(reg->bitend, reg->bitstart);
	val = (raw << reg->bitstart) | (mask << 16);
	SYSCON_WRITE_4(syscon, reg->offset, val);
}

static void
rk3588_combphy_update(struct rk3588_combphy_softc *sc, uint32_t reg,
    uint32_t mask, uint32_t val)
{
	uint32_t tmp;

	tmp = bus_read_4(sc->mem, reg);
	tmp = (tmp & ~mask) | val;
	bus_write_4(sc->mem, reg, tmp);
}

static uint64_t
rk3588_combphy_ref_rate(struct rk3588_combphy_softc *sc)
{
	uint64_t rate;

	rate = 0;
	if (sc->ref_clk != NULL)
		clk_get_freq(sc->ref_clk, &rate);
	if (rate == 0)
		rate = REF_CLOCK_100MHZ;
	return (rate);
}

static int
rk3588_combphy_enable_clks(struct rk3588_combphy_softc *sc)
{
	int error;

	error = clk_enable(sc->ref_clk);
	if (error != 0)
		device_printf(sc->dev, "cannot enable ref clock: %d\n", error);
	error = clk_enable(sc->apb_clk);
	if (error != 0)
		device_printf(sc->dev, "cannot enable APB clock: %d\n", error);
	error = clk_enable(sc->pipe_clk);
	if (error != 0)
		device_printf(sc->dev, "cannot enable pipe clock: %d\n", error);

	return (0);
}

static int
rk3588_combphy_config_pcie(struct rk3588_combphy_softc *sc)
{
	uint64_t rate;
	uint32_t reg;

	rk3588_combphy_param_write(sc->pipe_phy_grf,
	    &rk3588_combphy_con0_for_pcie, true);
	rk3588_combphy_param_write(sc->pipe_phy_grf,
	    &rk3588_combphy_con1_for_pcie, true);
	rk3588_combphy_param_write(sc->pipe_phy_grf,
	    &rk3588_combphy_con2_for_pcie, true);
	rk3588_combphy_param_write(sc->pipe_phy_grf,
	    &rk3588_combphy_con3_for_pcie, true);

	switch (sc->id) {
	case 1:
		rk3588_combphy_param_write(sc->pipe_grf,
		    &rk3588_combphy_pipe_pcie1l0_sel, true);
		break;
	case 2:
		rk3588_combphy_param_write(sc->pipe_grf,
		    &rk3588_combphy_pipe_pcie1l1_sel, true);
		break;
	default:
		break;
	}

	rate = rk3588_combphy_ref_rate(sc);
	switch (rate) {
	case REF_CLOCK_24MHZ:
		break;
	case REF_CLOCK_25MHZ:
		rk3588_combphy_param_write(sc->pipe_phy_grf,
		    &rk3588_combphy_pipe_clk_25m, true);
		break;
	case REF_CLOCK_100MHZ:
		rk3588_combphy_param_write(sc->pipe_phy_grf,
		    &rk3588_combphy_pipe_clk_100m, true);
		rk3588_combphy_update(sc, RK3568_PHYREG33,
		    RK3568_PHYREG33_PLL_KVCO_MASK,
		    RK3568_PHYREG33_PLL_KVCO_VALUE_RK3588);
		bus_write_4(sc->mem, RK3568_PHYREG12,
		    RK3568_PHYREG12_PLL_LPF_ADJ_VALUE);
		bus_write_4(sc->mem, RK3588_PHYREG27,
		    RK3588_PHYREG27_RX_TRIM);
		bus_write_4(sc->mem, RK3568_PHYREG11,
		    RK3568_PHYREG11_SU_TRIM_0_7);
		break;
	default:
		device_printf(sc->dev, "unsupported ref clock: %ju\n",
		    (uintmax_t)rate);
		return (EINVAL);
	}

	if (sc->ext_refclk) {
		rk3588_combphy_param_write(sc->pipe_phy_grf,
		    &rk3588_combphy_pipe_clk_ext, true);
		if (rate == REF_CLOCK_100MHZ) {
			rk3588_combphy_update(sc, RK3568_PHYREG13,
			    RK3568_PHYREG13_RESISTER_MASK,
			    RK3568_PHYREG13_RESISTER_HIGH_Z |
			    RK3568_PHYREG13_CKRCV_AMP0);
			reg = bus_read_4(sc->mem, RK3568_PHYREG14);
			bus_write_4(sc->mem, RK3568_PHYREG14,
			    reg | RK3568_PHYREG14_CKRCV_AMP1);
		}
	}

	if (sc->enable_ssc) {
		reg = bus_read_4(sc->mem, RK3568_PHYREG8);
		bus_write_4(sc->mem, RK3568_PHYREG8,
		    reg | RK3568_PHYREG8_SSC_EN);
	}

	return (0);
}

static int
rk3588_combphy_config_sata(struct rk3588_combphy_softc *sc)
{
	uint64_t rate;
	uint32_t reg;

	reg = bus_read_4(sc->mem, RK3568_PHYREG15);
	bus_write_4(sc->mem, RK3568_PHYREG15,
	    reg | RK3568_PHYREG15_CTLE_EN);
	bus_write_4(sc->mem, RK3568_PHYREG7,
	    RK3568_PHYREG7_TX_RTERM_50OHM |
	    RK3568_PHYREG7_RX_RTERM_44OHM);

	rk3588_combphy_param_write(sc->pipe_phy_grf,
	    &rk3588_combphy_con0_for_sata, true);
	rk3588_combphy_param_write(sc->pipe_phy_grf,
	    &rk3588_combphy_con1_for_sata, true);
	rk3588_combphy_param_write(sc->pipe_phy_grf,
	    &rk3588_combphy_con2_for_sata, true);
	rk3588_combphy_param_write(sc->pipe_phy_grf,
	    &rk3588_combphy_con3_for_sata, true);
	rk3588_combphy_param_write(sc->pipe_grf,
	    &rk3588_combphy_pipe_con0_for_sata, true);
	rk3588_combphy_param_write(sc->pipe_grf,
	    &rk3588_combphy_pipe_con1_for_sata, true);

	rate = rk3588_combphy_ref_rate(sc);
	switch (rate) {
	case REF_CLOCK_24MHZ:
		rk3588_combphy_update(sc, RK3568_PHYREG15,
		    RK3568_PHYREG15_SSC_CNT_MASK,
		    RK3568_PHYREG15_SSC_CNT_VALUE);
		bus_write_4(sc->mem, RK3568_PHYREG16,
		    RK3568_PHYREG16_SSC_CNT_VALUE);
		break;
	case REF_CLOCK_25MHZ:
		rk3588_combphy_param_write(sc->pipe_phy_grf,
		    &rk3588_combphy_pipe_clk_25m, true);
		break;
	case REF_CLOCK_100MHZ:
		rk3588_combphy_param_write(sc->pipe_phy_grf,
		    &rk3588_combphy_pipe_clk_100m, true);
		rk3588_combphy_update(sc, RK3568_PHYREG32,
		    RK3568_PHYREG32_SSC_MASK,
		    RK3568_PHYREG32_SSC_DOWNWARD |
		    RK3568_PHYREG32_SSC_OFFSET_500PPM);
		rk3588_combphy_update(sc, RK3588_PHYREG9,
		    RK3588_PHYREG9_SSC_PPM_MASK,
		    RK3588_PHYREG9_SSC_PPM_3500);
		break;
	default:
		device_printf(sc->dev, "unsupported ref clock: %ju\n",
		    (uintmax_t)rate);
		return (EINVAL);
	}

	if (sc->ext_refclk)
		rk3588_combphy_param_write(sc->pipe_phy_grf,
		    &rk3588_combphy_pipe_clk_ext, true);
	if (sc->enable_ssc) {
		reg = bus_read_4(sc->mem, RK3568_PHYREG8);
		bus_write_4(sc->mem, RK3568_PHYREG8,
		    reg | RK3568_PHYREG8_SSC_EN);
	}

	return (0);
}

static int
rk3588_combphy_enable(struct phynode *phynode, bool enable)
{
	device_t dev;
	struct rk3588_combphy_softc *sc;
	int error;

	dev = phynode_get_device(phynode);
	sc = device_get_softc(dev);

	if (!enable)
		return (0);

	error = rk3588_combphy_enable_clks(sc);
	if (error != 0)
		return (error);

	switch (sc->mode) {
	case PHY_TYPE_PCIE:
		error = rk3588_combphy_config_pcie(sc);
		break;
	case PHY_TYPE_SATA:
		error = rk3588_combphy_config_sata(sc);
		break;
	default:
		device_printf(dev, "unsupported PHY mode %d\n", sc->mode);
		return (EINVAL);
	}
	if (error != 0)
		return (error);

	if (sc->phy_reset != NULL) {
		error = hwreset_deassert(sc->phy_reset);
		if (error != 0) {
			device_printf(dev, "cannot deassert PHY reset: %d\n",
			    error);
			return (error);
		}
	}
	return (0);
}

static phynode_method_t rk3588_combphy_phynode_methods[] = {
	PHYNODEMETHOD(phynode_enable,	rk3588_combphy_enable),

	PHYNODEMETHOD_END
};
DEFINE_CLASS_1(rk3588_combphy_phynode, rk3588_combphy_phynode_class,
    rk3588_combphy_phynode_methods, 0, phynode_class);

static int
rk3588_combphy_probe(device_t dev)
{

	if (!ofw_bus_status_okay(dev))
		return (ENXIO);
	if (ofw_bus_search_compatible(dev, compat_data)->ocd_data == 0)
		return (ENXIO);
	device_set_desc(dev, "RockChip RK3588 Naneng combo PHY");
	return (BUS_PROBE_DEFAULT);
}

static int
rk3588_combphy_attach(device_t dev)
{
	struct rk3588_combphy_softc *sc;
	struct phynode_init_def phy_init;
	struct phynode *phynode;
	rman_res_t start;
	int error, rid;

	sc = device_get_softc(dev);
	sc->dev = dev;
	sc->node = ofw_bus_get_node(dev);
	sc->mode = PHY_NONE;
	rid = 0;

	sc->mem = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid, RF_ACTIVE);
	if (sc->mem == NULL) {
		device_printf(dev, "cannot allocate memory resource\n");
		return (ENXIO);
	}
	start = rman_get_start(sc->mem);
	switch (start) {
	case 0xfee00000:
		sc->id = 0;
		break;
	case 0xfee10000:
		sc->id = 1;
		break;
	case 0xfee20000:
		sc->id = 2;
		break;
	default:
		sc->id = -1;
		break;
	}
	error = syscon_get_by_ofw_property(dev, sc->node, "rockchip,pipe-grf",
	    &sc->pipe_grf);
	if (error != 0) {
		device_printf(dev, "cannot get rockchip,pipe-grf: %d\n", error);
		return (ENXIO);
	}
	error = syscon_get_by_ofw_property(dev, sc->node,
	    "rockchip,pipe-phy-grf", &sc->pipe_phy_grf);
	if (error != 0) {
		device_printf(dev, "cannot get rockchip,pipe-phy-grf: %d\n",
		    error);
		return (ENXIO);
	}

	error = clk_set_assigned(dev, sc->node);
	if (error != 0) {
		device_printf(dev, "cannot set assigned clocks: %d\n", error);
		return (error);
	}

	if (clk_get_by_ofw_name(dev, 0, "ref", &sc->ref_clk) != 0) {
		device_printf(dev, "cannot get ref clock\n");
		return (ENXIO);
	}
	if (clk_get_by_ofw_name(dev, 0, "apb", &sc->apb_clk) != 0) {
		device_printf(dev, "cannot get APB clock\n");
		return (ENXIO);
	}
	if (clk_get_by_ofw_name(dev, 0, "pipe", &sc->pipe_clk) != 0) {
		device_printf(dev, "cannot get pipe clock\n");
		return (ENXIO);
	}

	error = hwreset_get_by_ofw_name(dev, 0, "phy", &sc->phy_reset);
	if (error != 0) {
		device_printf(dev, "cannot get PHY reset: %d\n", error);
		return (ENXIO);
	}
	hwreset_assert(sc->phy_reset);

	sc->enable_ssc = OF_hasprop(sc->node, "rockchip,enable-ssc");
	sc->ext_refclk = OF_hasprop(sc->node, "rockchip,ext-refclk");

	memset(&phy_init, 0, sizeof(phy_init));
	phy_init.id = 0;
	phy_init.ofw_node = sc->node;
	phynode = phynode_create(dev, &rk3588_combphy_phynode_class,
	    &phy_init);
	if (phynode == NULL) {
		device_printf(dev, "failed to create PHY node\n");
		return (ENXIO);
	}
	if (!phynode_register(phynode)) {
		device_printf(dev, "failed to register PHY node\n");
		return (ENXIO);
	}
	sc->phynode = phynode;

	return (0);
}

static int
rk3588_combphy_map(device_t dev, phandle_t xref, int ncells, pcell_t *cells,
    intptr_t *id)
{
	struct rk3588_combphy_softc *sc;
	int error;

	sc = device_get_softc(dev);
	error = phydev_default_ofw_map(dev, xref, ncells, cells, id);
	if (error != 0)
		return (error);

	sc->mode = *id;
	*id = 0;
	return (0);
}

static device_method_t rk3588_combphy_methods[] = {
	DEVMETHOD(device_probe,		rk3588_combphy_probe),
	DEVMETHOD(device_attach,	rk3588_combphy_attach),
	DEVMETHOD(phydev_map,		rk3588_combphy_map),

	DEVMETHOD_END
};

DEFINE_CLASS_1(rk3588_combphy, rk3588_combphy_driver,
    rk3588_combphy_methods, sizeof(struct rk3588_combphy_softc),
    simple_mfd_driver);
EARLY_DRIVER_MODULE(rk3588_combphy, simplebus, rk3588_combphy_driver,
    0, 0, BUS_PASS_BUS + BUS_PASS_ORDER_LATE);
