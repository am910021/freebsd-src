/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2021, 2022 Soren Schmidt <sos@deepcore.dk>
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/rman.h>
#include <machine/bus.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include <dev/fdt/simple_mfd.h>

#include <dev/extres/clk/clk.h>
#include <dev/extres/hwreset/hwreset.h>
#include <dev/extres/syscon/syscon.h>
#include <dev/extres/phy/phy.h>

#include <contrib/device-tree/include/dt-bindings/phy/phy.h>

#include "syscon_if.h"
#include "phydev_if.h"
#include "phynode_if.h"

#define	RK3588_PCIE3PHY_GRF_CMN_CON0		0x000
#define	 RK3588_PCIE30_PHY_MODE_EN		(0x7 << 16)
#define	 RK3588_PCIE30_PHY_PMA_OCM_EN		((1 << 8) | (1 << 24))
#define	RK3588_PCIE3PHY_GRF_PHY0_STATUS1	0x904
#define	RK3588_PCIE3PHY_GRF_PHY1_STATUS1	0xa04
#define	RK3588_PCIE3PHY_GRF_PHY0_LN0_CON1	0x1004
#define	RK3588_PCIE3PHY_GRF_PHY0_LN1_CON1	0x1104
#define	RK3588_PCIE3PHY_GRF_PHY1_LN0_CON1	0x2004
#define	RK3588_PCIE3PHY_GRF_PHY1_LN1_CON1	0x2104
#define	 RK3588_SRAM_INIT_DONE(reg)		((reg) & (1 << 0))
#define	 RK3588_RX_CMN_REFCLK_MODE_EN		((1 << 23) | (1 << 7))
#define	 RK3588_RX_CMN_REFCLK_MODE_DIS		(1 << 23)
#define	 RK3588_BIFURCATION_LANE_0_1		(1 << 0)
#define	 RK3588_BIFURCATION_LANE_2_3		(1 << 1)
#define	 RK3588_LANE_AGGREGATION		(1 << 2)

#define	PHP_GRF_PCIESEL_CON			0x100
#define	 RK3588_PCIE1LN_SEL_EN			(0x3 << 16)

static struct ofw_compat_data compat_data[] = {
	{"rockchip,rk3588-pcie3-phy", 1},
	{NULL, 0}
};

struct rk3588_pciephy_softc {
	phandle_t	node;
	struct resource	*mem;
	struct phynode	*phynode;
	struct syscon	*phy_grf;
	struct syscon	*pipe_grf;
	clk_t		pclk;
	hwreset_t	phy_reset;
	uint32_t	pcie30_phymode;
	uint32_t	rx_cmn_refclk_mode[4];
};

static int
rk3588_pciephy_wait_sram(device_t dev, uint32_t regoff, const char *name)
{
	struct rk3588_pciephy_softc *sc;
	uint32_t reg;
	int count;

	sc = device_get_softc(dev);

	for (count = 200; count > 0; count--) {
		reg = SYSCON_READ_4(sc->phy_grf, regoff);
		if (RK3588_SRAM_INIT_DONE(reg))
			return (0);
		DELAY(100);
	}

	device_printf(dev, "%s SRAM init timeout (status %#x)\n", name, reg);
	return (ENXIO);
}

static void
rk3588_pciephy_program_mode(struct rk3588_pciephy_softc *sc)
{
	uint32_t pcie1ln_sel;

	pcie1ln_sel = 0;

	SYSCON_WRITE_4(sc->phy_grf, RK3588_PCIE3PHY_GRF_CMN_CON0,
	    RK3588_PCIE30_PHY_MODE_EN | sc->pcie30_phymode);
	if (sc->pipe_grf != NULL) {
		pcie1ln_sel = sc->pcie30_phymode &
		    (RK3588_BIFURCATION_LANE_0_1 |
		    RK3588_BIFURCATION_LANE_2_3);
		if (pcie1ln_sel != 0)
			SYSCON_WRITE_4(sc->pipe_grf, PHP_GRF_PCIESEL_CON,
			    RK3588_PCIE1LN_SEL_EN | pcie1ln_sel);
	}
}

static int
rk3588_pciephy_enable(struct phynode *phynode, bool enable)
{
	device_t dev;
	struct rk3588_pciephy_softc *sc;
	int error;

	dev = phynode_get_device(phynode);
	sc = device_get_softc(dev);

	if (!enable)
		return (0);

	if (sc->phy_reset != NULL) {
		hwreset_assert(sc->phy_reset);
		DELAY(1);
	}

	SYSCON_WRITE_4(sc->phy_grf,
	    RK3588_PCIE3PHY_GRF_PHY0_LN0_CON1,
	    sc->rx_cmn_refclk_mode[0] ?
	    RK3588_RX_CMN_REFCLK_MODE_EN :
	    RK3588_RX_CMN_REFCLK_MODE_DIS);
	SYSCON_WRITE_4(sc->phy_grf,
	    RK3588_PCIE3PHY_GRF_PHY0_LN1_CON1,
	    sc->rx_cmn_refclk_mode[1] ?
	    RK3588_RX_CMN_REFCLK_MODE_EN :
	    RK3588_RX_CMN_REFCLK_MODE_DIS);
	SYSCON_WRITE_4(sc->phy_grf,
	    RK3588_PCIE3PHY_GRF_PHY1_LN0_CON1,
	    sc->rx_cmn_refclk_mode[2] ?
	    RK3588_RX_CMN_REFCLK_MODE_EN :
	    RK3588_RX_CMN_REFCLK_MODE_DIS);
	SYSCON_WRITE_4(sc->phy_grf,
	    RK3588_PCIE3PHY_GRF_PHY1_LN1_CON1,
	    sc->rx_cmn_refclk_mode[3] ?
	    RK3588_RX_CMN_REFCLK_MODE_EN :
	    RK3588_RX_CMN_REFCLK_MODE_DIS);

	SYSCON_WRITE_4(sc->phy_grf, RK3588_PCIE3PHY_GRF_CMN_CON0,
	    RK3588_PCIE30_PHY_PMA_OCM_EN);
	/*
	 * Linux programs the PHY mode during PHY init, after PMA OCM
	 * and before reset deassert.  Keep the attach-time write, but
	 * repeat it here to match that reset timing on RK3588.
	 */
	rk3588_pciephy_program_mode(sc);

	if (sc->phy_reset != NULL)
		hwreset_deassert(sc->phy_reset);

	error = rk3588_pciephy_wait_sram(dev,
	    RK3588_PCIE3PHY_GRF_PHY0_STATUS1, "phy0");
	if (error != 0)
		return (error);
	if (sc->pcie30_phymode == RK3588_LANE_AGGREGATION) {
		error = rk3588_pciephy_wait_sram(dev,
		    RK3588_PCIE3PHY_GRF_PHY1_STATUS1, "phy1");
		if (error != 0)
			return (error);
	}
	return (0);
}

static phynode_method_t rk3588_pciephy_phynode_methods[] = {
	PHYNODEMETHOD(phynode_enable,	rk3588_pciephy_enable),

	PHYNODEMETHOD_END
};
DEFINE_CLASS_1(rk3588_pciephy_phynode, rk3588_pciephy_phynode_class,
    rk3588_pciephy_phynode_methods, 0, phynode_class);

static int
rk3588_pciephy_probe(device_t dev)
{

	if (!ofw_bus_status_okay(dev))
		return (ENXIO);
	if (ofw_bus_search_compatible(dev, compat_data)->ocd_data == 0)
		return (ENXIO);
	device_set_desc(dev, "RockChip RK3588 PCIe PHY");
	return (BUS_PROBE_DEFAULT);
}

static int
rk3588_pciephy_attach(device_t dev)
{
	struct rk3588_pciephy_softc *sc;
	struct phynode_init_def phy_init;
	struct phynode *phynode;
	uint32_t data_lanes[4], mode;
	ssize_t len;
	int rid;

	sc = device_get_softc(dev);
	sc->node = ofw_bus_get_node(dev);
	rid = 0;

	sc->mem = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid, RF_ACTIVE);
	if (sc->mem == NULL) {
		device_printf(dev, "Cannot allocate memory resources\n");
		return (ENXIO);
	}

	if (syscon_get_by_ofw_property(dev, sc->node, "rockchip,phy-grf",
	    &sc->phy_grf) != 0) {
		device_printf(dev, "Failed to find rockchip,phy-grf syscon\n");
		bus_release_resource(dev, SYS_RES_MEMORY, rid, sc->mem);
		sc->mem = NULL;
		return (ENXIO);
	}
	syscon_get_by_ofw_property(dev, sc->node, "rockchip,pipe-grf",
	    &sc->pipe_grf);

	if (clk_get_by_ofw_name(dev, 0, "pclk", &sc->pclk) != 0) {
		device_printf(dev, "getting pclk failed\n");
		return (ENXIO);
	}
	if (clk_enable(sc->pclk) != 0)
		device_printf(dev, "enable pclk failed\n");

	if (hwreset_get_by_ofw_idx(dev, sc->node, 0, &sc->phy_reset) != 0)
		device_printf(dev, "Cannot get reset\n");
	else
		hwreset_assert(sc->phy_reset);

	mode = RK3588_LANE_AGGREGATION;
	len = OF_getencprop(sc->node, "data-lanes", data_lanes,
	    sizeof(data_lanes));
	if (len <= 0) {
		data_lanes[0] = 1;
		len = sizeof(uint32_t);
	}
	for (int i = len / sizeof(uint32_t); i < 4; i++)
		data_lanes[i] = 0;
	for (int i = 0; i < len / sizeof(uint32_t); i++) {
		if (data_lanes[i] > 1)
			mode &= ~RK3588_LANE_AGGREGATION;
		if (data_lanes[i] == 3)
			mode |= RK3588_BIFURCATION_LANE_0_1;
		if (data_lanes[i] == 4)
			mode |= RK3588_BIFURCATION_LANE_2_3;
	}
	sc->pcie30_phymode = mode;

	len = OF_getencprop(sc->node, "rockchip,rx-common-refclk-mode",
	    sc->rx_cmn_refclk_mode, sizeof(sc->rx_cmn_refclk_mode));
	if (len <= 0) {
		sc->rx_cmn_refclk_mode[0] = 1;
		sc->rx_cmn_refclk_mode[1] = 1;
		sc->rx_cmn_refclk_mode[2] = 1;
		sc->rx_cmn_refclk_mode[3] = 1;
	} else {
		for (int i = len / sizeof(uint32_t); i < 4; i++)
			sc->rx_cmn_refclk_mode[i] = 1;
	}
	rk3588_pciephy_program_mode(sc);

	bzero(&phy_init, sizeof(phy_init));
	phy_init.id = PHY_NONE;
	phy_init.ofw_node = sc->node;
	phynode = phynode_create(dev, &rk3588_pciephy_phynode_class,
	    &phy_init);
	if (phynode == NULL) {
		device_printf(dev, "failed to create pciephy PHY\n");
		return (ENXIO);
	}
	if (!phynode_register(phynode)) {
		device_printf(dev, "failed to register pciephy PHY\n");
		return (ENXIO);
	}
	sc->phynode = phynode;

	return (0);
}

static device_method_t rk3588_pciephy_methods[] = {
	DEVMETHOD(device_probe,		rk3588_pciephy_probe),
	DEVMETHOD(device_attach,	rk3588_pciephy_attach),

	DEVMETHOD_END
};

DEFINE_CLASS_1(rk3588_pciephy, rk3588_pciephy_driver, rk3588_pciephy_methods,
    sizeof(struct rk3588_pciephy_softc), simple_mfd_driver);
EARLY_DRIVER_MODULE(rk3588_pciephy, simplebus, rk3588_pciephy_driver,
    0, 0, BUS_PASS_BUS + BUS_PASS_ORDER_LATE);
