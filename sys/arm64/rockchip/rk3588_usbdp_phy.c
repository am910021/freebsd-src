/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/rman.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

#include <machine/bus.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include <dev/extres/clk/clk.h>
#include <dev/extres/hwreset/hwreset.h>
#include <dev/extres/phy/phy_usb.h>
#include <dev/extres/regulator/regulator.h>
#include <dev/extres/syscon/syscon.h>

#include <dev/usb/typec/usb_typec.h>

#include "phydev_if.h"
#include "phynode_if.h"
#include "syscon_if.h"
#include "typec_switch_if.h"

#define	RK3588_USBDP_DIAG	"RK3588-DIAG V803 usbdp"

#define	RK3588_UDPHY_PMA			0x8000

#define	RK3588_UDPHY_CMN_LANE_MUX_AND_EN	0x0288
#define	 RK3588_UDPHY_CMN_DP_LANE_MUX_ALL	0x000000f0
#define	 RK3588_UDPHY_CMN_DP_LANE_EN_ALL	0x0000000f
#define	 RK3588_UDPHY_CMN_TYPEC_NORMAL		0x000000c0
#define	 RK3588_UDPHY_CMN_TYPEC_REVERSE	0x00000030
#define	RK3588_UDPHY_CMN_ANA_LCPLL_DONE		0x0350
#define	 RK3588_UDPHY_CMN_ANA_LCPLL_LOCK_DONE	(1u << 7)
#define	 RK3588_UDPHY_CMN_ANA_LCPLL_AFC_DONE	(1u << 6)
#define	RK3588_UDPHY_TRSV_LN0_MON_RX_CDR_DONE	0x0b84
#define	RK3588_UDPHY_TRSV_LN2_MON_RX_CDR_DONE	0x1b84
#define	 RK3588_UDPHY_TRSV_MON_RX_CDR_LOCK_DONE	(1u << 0)

#define	RK3588_USBDP_PHY_MAX_CLOCKS		4
#define	RK3588_USBDP_PHY_MAX_RESETS		5
#define	RK3588_PHY_TYPE_USB3			4

#define	RK3588_USBDP_GENMASK(h, l)		\
	(((~0u) >> (31 - ((h) - (l)))) << (l))

struct rk3588_usbdp_grf_reg {
	uint32_t	offset;
	uint32_t	bitend;
	uint32_t	bitstart;
	uint32_t	disable;
	uint32_t	enable;
};

struct rk3588_usbdp_regseq {
	uint32_t	reg;
	uint32_t	val;
};

struct rk3588_usbdp_softc {
	device_t	dev;
	phandle_t	node;
	struct resource	*mem;
	struct syscon	*u2phy_grf;
	struct syscon	*usbdpphy_grf;
	struct syscon	*usb_grf;
	struct syscon	*vo_grf;
	regulator_t	phy_supply;
	clk_t		clks[RK3588_USBDP_PHY_MAX_CLOCKS];
	hwreset_t	resets[RK3588_USBDP_PHY_MAX_RESETS];
	struct phynode	*phynode;
	int		id;
	int		mode;
	bool		enabled;
	bool		phy_supply_enabled;
	uint32_t	clocks_enabled;
	bool		typec;
	bool		flip;
};

static char *rk3588_usbdp_clk_names[] = {
	"refclk", "immortal", "pclk", "utmi",
};

static char *rk3588_usbdp_reset_names[] = {
	"init", "cmn", "lane", "pcs_apb", "pma_apb",
};

static struct ofw_compat_data rk3588_usbdp_compat[] = {
	{ "rockchip,rk3588-usbdp-phy",	1 },
	{ NULL,				0 },
};

static const struct rk3588_usbdp_grf_reg rk3588_usbdp_low_pwrn =
    { 0x0004, 13, 13, 0, 1 };
static const struct rk3588_usbdp_grf_reg rk3588_usbdp_rx_lfps =
    { 0x0004, 14, 14, 0, 1 };
static const struct rk3588_usbdp_grf_reg rk3588_usbdp_bvalid_phy_con =
    { 0x0008, 1, 0, 0x2, 0x3 };
static const struct rk3588_usbdp_grf_reg rk3588_usbdp_bvalid_grf_con =
    { 0x0010, 3, 2, 0x2, 0x3 };
static const struct rk3588_usbdp_grf_reg rk3588_usbdp_usb3otg0_cfg =
    { 0x001c, 15, 0, 0x1100, 0x0188 };
static const struct rk3588_usbdp_grf_reg rk3588_usbdp_usb3otg1_cfg =
    { 0x0034, 15, 0, 0x1100, 0x0188 };

static int rk3588_usbdp_v633_bvalid_apply = 1;
TUNABLE_INT("hw.rk3588.usbdp_v633_bvalid_apply",
    &rk3588_usbdp_v633_bvalid_apply);
static int rk3588_usbdp_v633_bvalid_state = 0;
TUNABLE_INT("hw.rk3588.usbdp_v633_bvalid_state",
    &rk3588_usbdp_v633_bvalid_state);
static const struct rk3588_usbdp_regseq rk3588_usbdp_24m_refclk_cfg[] = {
	{0x0090, 0x68}, {0x0094, 0x68}, {0x0128, 0x24}, {0x012c, 0x44},
	{0x0130, 0x3f}, {0x0134, 0x44}, {0x015c, 0xa9}, {0x0160, 0x71},
	{0x0164, 0x71}, {0x0168, 0xa9}, {0x0174, 0xa9}, {0x0178, 0x71},
	{0x017c, 0x71}, {0x0180, 0xa9}, {0x018c, 0x41}, {0x0190, 0x00},
	{0x0194, 0x05}, {0x01ac, 0x2a}, {0x01b0, 0x17}, {0x01b4, 0x17},
	{0x01b8, 0x2a}, {0x01c8, 0x04}, {0x01cc, 0x08}, {0x01d0, 0x08},
	{0x01d4, 0x04}, {0x01d8, 0x20}, {0x01dc, 0x01}, {0x01e0, 0x09},
	{0x01e4, 0x03}, {0x01f0, 0x29}, {0x01f4, 0x02}, {0x01f8, 0x02},
	{0x01fc, 0x29}, {0x0208, 0x2a}, {0x020c, 0x17}, {0x0210, 0x17},
	{0x0214, 0x2a}, {0x0224, 0x20}, {0x03f0, 0x0a}, {0x03f4, 0x07},
	{0x03f8, 0x07}, {0x03fc, 0x0c}, {0x0404, 0x12}, {0x0408, 0x1a},
	{0x040c, 0x1a}, {0x0410, 0x3f}, {0x0ce0, 0x68}, {0x0ce8, 0xd0},
	{0x0cf0, 0x87}, {0x0cf8, 0x70}, {0x0d00, 0x70}, {0x0d08, 0xa9},
	{0x1ce0, 0x68}, {0x1ce8, 0xd0}, {0x1cf0, 0x87}, {0x1cf8, 0x70},
	{0x1d00, 0x70}, {0x1d08, 0xa9}, {0x0a3c, 0xd0}, {0x0a44, 0xd0},
	{0x0a48, 0x01}, {0x0a4c, 0x0d}, {0x0a54, 0xe0}, {0x0a5c, 0xe0},
	{0x0a64, 0xa8}, {0x1a3c, 0xd0}, {0x1a44, 0xd0}, {0x1a48, 0x01},
	{0x1a4c, 0x0d}, {0x1a54, 0xe0}, {0x1a5c, 0xe0}, {0x1a64, 0xa8},
};

static const struct rk3588_usbdp_regseq rk3588_usbdp_init_sequence[] = {
	{0x0104, 0x44}, {0x0234, 0xe8}, {0x0248, 0x44}, {0x028c, 0x18},
	{0x081c, 0xe5}, {0x0878, 0x00}, {0x0994, 0x1c}, {0x0af0, 0x00},
	{0x181c, 0xe5}, {0x1878, 0x00}, {0x1994, 0x1c}, {0x1af0, 0x00},
	{0x0428, 0x60}, {0x0d58, 0x33}, {0x1d58, 0x33}, {0x0990, 0x74},
	{0x0d64, 0x17}, {0x08c8, 0x13}, {0x1990, 0x74}, {0x1d64, 0x17},
	{0x18c8, 0x13}, {0x0d90, 0x40}, {0x0da8, 0x40}, {0x0dc0, 0x40},
	{0x0dd8, 0x40}, {0x1d90, 0x40}, {0x1da8, 0x40}, {0x1dc0, 0x40},
	{0x1dd8, 0x40}, {0x03c0, 0x30}, {0x03c4, 0x06}, {0x0e10, 0x00},
	{0x1e10, 0x00}, {0x043c, 0x0f}, {0x0d2c, 0xff}, {0x1d2c, 0xff},
	{0x0d34, 0x0f}, {0x1d34, 0x0f}, {0x08fc, 0x2a}, {0x0914, 0x28},
	{0x0a30, 0x03}, {0x0e38, 0x03}, {0x0ecc, 0x27}, {0x0ed0, 0x22},
	{0x0ed4, 0x26}, {0x18fc, 0x2a}, {0x1914, 0x28}, {0x1a30, 0x03},
	{0x1e38, 0x03}, {0x1ecc, 0x27}, {0x1ed0, 0x22}, {0x1ed4, 0x26},
	{0x0048, 0x0f}, {0x0060, 0x3c}, {0x0064, 0xf7}, {0x006c, 0x20},
	{0x0070, 0x7d}, {0x0074, 0x68}, {0x0af4, 0x1a}, {0x1af4, 0x1a},
	{0x0440, 0x3f}, {0x10d4, 0x08}, {0x20d4, 0x08}, {0x00d4, 0x30},
	{0x0024, 0x6e},
};

static void
rk3588_usbdp_pma_write(struct rk3588_usbdp_softc *sc, bus_size_t reg,
    uint32_t val)
{

	bus_write_4(sc->mem, RK3588_UDPHY_PMA + reg, val);
}

static uint32_t
rk3588_usbdp_pma_read(struct rk3588_usbdp_softc *sc, bus_size_t reg)
{

	return (bus_read_4(sc->mem, RK3588_UDPHY_PMA + reg));
}

static void
rk3588_usbdp_pma_update(struct rk3588_usbdp_softc *sc, bus_size_t reg,
    uint32_t mask, uint32_t val)
{
	uint32_t tmp;

	tmp = rk3588_usbdp_pma_read(sc, reg);
	tmp &= ~mask;
	tmp |= val & mask;
	rk3588_usbdp_pma_write(sc, reg, tmp);
}

static void
rk3588_usbdp_grf_write(struct syscon *syscon,
    const struct rk3588_usbdp_grf_reg *reg, bool enable)
{
	uint32_t mask, raw, val;

	raw = enable ? reg->enable : reg->disable;
	mask = RK3588_USBDP_GENMASK(reg->bitend, reg->bitstart);
	val = (raw << reg->bitstart) | (mask << 16);
	SYSCON_WRITE_4(syscon, reg->offset, val);
}

static int
rk3588_usbdp_deassert_reset(struct rk3588_usbdp_softc *sc, int idx,
    const char *stage)
{
	int error;

	if (idx < 0 || idx >= RK3588_USBDP_PHY_MAX_RESETS ||
	    sc->resets[idx] == NULL) {
		device_printf(sc->dev,
		    "%s required reset %d missing at %s\n",
		    RK3588_USBDP_DIAG, idx, stage);
		return (ENXIO);
	}

	error = hwreset_deassert(sc->resets[idx]);
	if (error != 0)
		device_printf(sc->dev,
		    "%s reset %s deassert failed at %s: %d\n",
		    RK3588_USBDP_DIAG, rk3588_usbdp_reset_names[idx], stage,
		    error);
	return (error);
}

static void
rk3588_usbdp_v633_set_bvalid(struct rk3588_usbdp_softc *sc, bool enable)
{

	if (rk3588_usbdp_v633_bvalid_apply == 0)
		return;
	if (sc->id != 0)
		return;

	rk3588_usbdp_grf_write(sc->u2phy_grf, &rk3588_usbdp_bvalid_phy_con,
	    enable);
	rk3588_usbdp_grf_write(sc->u2phy_grf, &rk3588_usbdp_bvalid_grf_con,
	    enable);
}

static void
rk3588_usbdp_write_seq(struct rk3588_usbdp_softc *sc,
    const struct rk3588_usbdp_regseq *seq, int count)
{
	int i;

	for (i = 0; i < count; i++)
		rk3588_usbdp_pma_write(sc, seq[i].reg, seq[i].val);
}

static int
rk3588_usbdp_wait_set(struct rk3588_usbdp_softc *sc, bus_size_t reg,
    uint32_t mask, const char *name)
{
	uint32_t val;
	int i;

	for (i = 0; i < 500; i++) {
		val = rk3588_usbdp_pma_read(sc, reg);
		if ((val & mask) == mask) {
			if (bootverbose)
				device_printf(sc->dev,
				    "%s %s locked val=%#010x\n",
				    RK3588_USBDP_DIAG, name, val);
			return (0);
		}
		DELAY(200);
	}

	val = rk3588_usbdp_pma_read(sc, reg);
	device_printf(sc->dev, "%s %s timeout val=%#010x mask=%#010x\n",
	    RK3588_USBDP_DIAG, name, val, mask);
	return (ETIMEDOUT);
}

static int
rk3588_usbdp_assert_resets(struct rk3588_usbdp_softc *sc)
{
	int error, i;

	for (i = 0; i < RK3588_USBDP_PHY_MAX_RESETS; i++) {
		if (sc->resets[i] == NULL) {
			device_printf(sc->dev, "%s required reset %s missing\n",
			    RK3588_USBDP_DIAG, rk3588_usbdp_reset_names[i]);
			return (ENXIO);
		}
		error = hwreset_assert(sc->resets[i]);
		if (error != 0) {
			device_printf(sc->dev, "%s reset %s assert failed: %d\n",
			    RK3588_USBDP_DIAG, rk3588_usbdp_reset_names[i],
			    error);
			return (error);
		}
	}
	return (0);
}

static int
rk3588_usbdp_acquire_supply(struct rk3588_usbdp_softc *sc, const char *where)
{
	int error;

	if (sc->phy_supply != NULL || !OF_hasprop(sc->node, "phy-supply"))
		return (0);

	error = regulator_get_by_ofw_property(sc->dev, sc->node,
	    "phy-supply", &sc->phy_supply);
	if (error != 0) {
		sc->phy_supply = NULL;
		device_printf(sc->dev,
		    "%s phy-supply unavailable at %s id=%d node=%#x "
		    "error=%d provider-late-suspect=1\n",
		    RK3588_USBDP_DIAG, where, sc->id, sc->node, error);
		return (error);
	}

	return (0);
}

static int
rk3588_usbdp_enable_supply(struct rk3588_usbdp_softc *sc)
{
	int error;

	error = rk3588_usbdp_acquire_supply(sc, "enable");
	if (error != 0)
		return (error);

	if (sc->phy_supply == NULL || sc->phy_supply_enabled)
		return (0);

	error = regulator_enable(sc->phy_supply);
	if (error != 0) {
		device_printf(sc->dev, "%s phy-supply enable error=%d\n",
		    RK3588_USBDP_DIAG, error);
		return (error);
	}

	sc->phy_supply_enabled = true;
	return (0);
}

static int
rk3588_usbdp_enable_clocks(struct rk3588_usbdp_softc *sc)
{
	int error, i;

	for (i = 0; i < RK3588_USBDP_PHY_MAX_CLOCKS; i++) {
		if (sc->clks[i] == NULL) {
			device_printf(sc->dev, "%s required clock %s missing\n",
			    RK3588_USBDP_DIAG, rk3588_usbdp_clk_names[i]);
			return (ENXIO);
		}
		if ((sc->clocks_enabled & (1u << i)) != 0)
			continue;
		error = clk_enable(sc->clks[i]);
		if (error != 0) {
			device_printf(sc->dev, "%s clock %s enable error=%d\n",
			    RK3588_USBDP_DIAG, rk3588_usbdp_clk_names[i],
			    error);
			return (error);
		}
		sc->clocks_enabled |= 1u << i;
	}

	return (0);
}

static int
rk3588_usbdp_hw_disable(struct rk3588_usbdp_softc *sc)
{
	int error, first_error, i;

	first_error = 0;
	for (i = RK3588_USBDP_PHY_MAX_CLOCKS - 1; i >= 0; i--) {
		if ((sc->clocks_enabled & (1u << i)) == 0)
			continue;
		error = clk_disable(sc->clks[i]);
		if (error != 0) {
			device_printf(sc->dev, "%s clock %s disable error=%d\n",
			    RK3588_USBDP_DIAG, rk3588_usbdp_clk_names[i],
			    error);
			if (first_error == 0)
				first_error = error;
		} else {
			sc->clocks_enabled &= ~(1u << i);
		}
	}

	error = rk3588_usbdp_assert_resets(sc);
	if (error != 0 && first_error == 0)
		first_error = error;

	if (sc->phy_supply_enabled) {
		error = regulator_disable(sc->phy_supply);
		if (error != 0) {
			device_printf(sc->dev, "%s phy-supply disable error=%d\n",
			    RK3588_USBDP_DIAG, error);
			if (first_error == 0)
				first_error = error;
		} else {
			sc->phy_supply_enabled = false;
		}
	}

	sc->enabled = false;
	if (first_error == 0) {
		device_printf(sc->dev,
		    "%s disabled; clocks off and resets asserted\n",
		    RK3588_USBDP_DIAG);
	} else {
		device_printf(sc->dev,
		    "%s disable incomplete error=%d clocks=%#x supply=%d\n",
		    RK3588_USBDP_DIAG, first_error, sc->clocks_enabled,
		    sc->phy_supply_enabled);
	}
	return (first_error);
}

static int
rk3588_usbdp_hw_init(struct rk3588_usbdp_softc *sc)
{
	const struct rk3588_usbdp_grf_reg *usb3otg_cfg;
	bus_size_t cdr_reg;
	const char *cdr_name;
	uint32_t lane_mux;
	int error;

	if (sc->enabled)
		return (0);

	error = rk3588_usbdp_enable_supply(sc);
	if (error != 0)
		goto fail;

	error = rk3588_usbdp_enable_clocks(sc);
	if (error != 0)
		goto fail;

	error = rk3588_usbdp_assert_resets(sc);
	if (error != 0)
		goto fail;
	DELAY(10000);

	rk3588_usbdp_grf_write(sc->usbdpphy_grf,
	    &rk3588_usbdp_rx_lfps, true);
	rk3588_usbdp_grf_write(sc->usbdpphy_grf,
	    &rk3588_usbdp_low_pwrn, true);
	if (!sc->typec)
		rk3588_usbdp_v633_set_bvalid(sc,
		    rk3588_usbdp_v633_bvalid_state != 0);

	/*
	 * Linux deasserts pma_apb before pcs_apb in rk_udphy_init().
	 * This ordering is part of the USBDP dependency chain.
	 */
	error = rk3588_usbdp_deassert_reset(sc, 4, "pma_apb");
	if (error != 0)
		goto fail;
	error = rk3588_usbdp_deassert_reset(sc, 3, "pcs_apb");
	if (error != 0)
		goto fail;

	rk3588_usbdp_write_seq(sc, rk3588_usbdp_init_sequence,
	    nitems(rk3588_usbdp_init_sequence));
	rk3588_usbdp_write_seq(sc, rk3588_usbdp_24m_refclk_cfg,
	    nitems(rk3588_usbdp_24m_refclk_cfg));

	lane_mux = 0;
	cdr_reg = RK3588_UDPHY_TRSV_LN0_MON_RX_CDR_DONE;
	cdr_name = "lane0-cdr";
	if (sc->typec) {
		lane_mux = sc->flip ? RK3588_UDPHY_CMN_TYPEC_REVERSE :
		    RK3588_UDPHY_CMN_TYPEC_NORMAL;
		if (sc->flip) {
			cdr_reg = RK3588_UDPHY_TRSV_LN2_MON_RX_CDR_DONE;
			cdr_name = "lane2-cdr";
		}
		device_printf(sc->dev,
		    "%s Type-C orientation=%s lane-mux=0x%02x\n",
		    RK3588_USBDP_DIAG, sc->flip ? "reverse" : "normal",
		    lane_mux);
	}
	rk3588_usbdp_pma_update(sc, RK3588_UDPHY_CMN_LANE_MUX_AND_EN,
	    RK3588_UDPHY_CMN_DP_LANE_MUX_ALL |
	    RK3588_UDPHY_CMN_DP_LANE_EN_ALL, lane_mux);

	error = rk3588_usbdp_deassert_reset(sc, 0, "init");
	if (error != 0)
		goto fail;
	DELAY(1);
	error = rk3588_usbdp_deassert_reset(sc, 1, "cmn");
	if (error != 0)
		goto fail;
	error = rk3588_usbdp_deassert_reset(sc, 2, "lane");
	if (error != 0)
		goto fail;

	error = rk3588_usbdp_wait_set(sc, RK3588_UDPHY_CMN_ANA_LCPLL_DONE,
	    RK3588_UDPHY_CMN_ANA_LCPLL_LOCK_DONE |
	    RK3588_UDPHY_CMN_ANA_LCPLL_AFC_DONE, "lcpll");
	if (error != 0)
		goto fail;
	error = rk3588_usbdp_wait_set(sc, cdr_reg,
	    RK3588_UDPHY_TRSV_MON_RX_CDR_LOCK_DONE, cdr_name);
	if (error != 0)
		goto fail;

	usb3otg_cfg = sc->id ? &rk3588_usbdp_usb3otg1_cfg :
	    &rk3588_usbdp_usb3otg0_cfg;
	rk3588_usbdp_grf_write(sc->usb_grf, usb3otg_cfg, false);

	sc->enabled = true;
	return (0);

fail:
	(void)rk3588_usbdp_hw_disable(sc);
	return (error);
}

static int
rk3588_usbdp_enable(struct phynode *phynode, bool enable)
{
	struct rk3588_usbdp_softc *sc;

	sc = device_get_softc(phynode_get_device(phynode));
	if (!enable)
		return (rk3588_usbdp_hw_disable(sc));

	return (rk3588_usbdp_hw_init(sc));
}

static int
rk3588_usbdp_get_mode(struct phynode *phynode, int *mode)
{
	struct rk3588_usbdp_softc *sc;

	sc = device_get_softc(phynode_get_device(phynode));
	*mode = sc->mode;
	return (0);
}

static int
rk3588_usbdp_set_mode(struct phynode *phynode, int mode)
{
	struct rk3588_usbdp_softc *sc;

	sc = device_get_softc(phynode_get_device(phynode));
	sc->mode = mode;
	return (0);
}

static phynode_method_t rk3588_usbdp_phynode_methods[] = {
	PHYNODEMETHOD(phynode_enable,		rk3588_usbdp_enable),
	PHYNODEMETHOD(phynode_usb_get_mode,	rk3588_usbdp_get_mode),
	PHYNODEMETHOD(phynode_usb_set_mode,	rk3588_usbdp_set_mode),

	PHYNODEMETHOD_END
};

DEFINE_CLASS_1(rk3588_usbdp_phynode, rk3588_usbdp_phynode_class,
    rk3588_usbdp_phynode_methods, sizeof(struct phynode_usb_sc),
    phynode_usb_class);

static int
rk3588_usbdp_set_orientation(device_t dev,
    enum typec_orientation orientation)
{
	struct rk3588_usbdp_softc *sc;

	sc = device_get_softc(dev);
	if (!sc->typec)
		return (EOPNOTSUPP);
	if (sc->enabled)
		return (EBUSY);

	switch (orientation) {
	case TYPEC_ORIENTATION_NONE:
		rk3588_usbdp_v633_set_bvalid(sc, false);
		break;
	case TYPEC_ORIENTATION_NORMAL:
		sc->flip = false;
		rk3588_usbdp_v633_set_bvalid(sc, true);
		break;
	case TYPEC_ORIENTATION_REVERSE:
		sc->flip = true;
		rk3588_usbdp_v633_set_bvalid(sc, true);
		break;
	default:
		return (EINVAL);
	}
	if (bootverbose)
		device_printf(sc->dev, "%s Type-C orientation=%d\n",
		    RK3588_USBDP_DIAG, orientation);
	return (0);
}

static int
rk3588_usbdp_probe(device_t dev)
{

	if (!ofw_bus_status_okay(dev))
		return (ENXIO);
	if (ofw_bus_search_compatible(dev, rk3588_usbdp_compat)->ocd_data == 0)
		return (ENXIO);

	device_set_desc(dev, "Rockchip RK3588 USBDP PHY");
	return (BUS_PROBE_DEFAULT);
}

static int
rk3588_usbdp_attach(device_t dev)
{
	struct rk3588_usbdp_softc *sc;
	struct phynode_usb_init_def phy_init;
	struct phynode *phynode;
	uint64_t start;
	int error, i, rid;

	sc = device_get_softc(dev);
	sc->dev = dev;
	sc->node = ofw_bus_get_node(dev);
	sc->mode = PHY_USB_MODE_HOST;
	sc->typec = OF_hasprop(sc->node, "orientation-switch");
	sc->flip = false;

	rid = 0;
	sc->mem = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid,
	    RF_ACTIVE);
	if (sc->mem == NULL) {
		device_printf(dev, "Cannot allocate memory resource\n");
		return (ENXIO);
	}

	start = rman_get_start(sc->mem);
	sc->id = (start == 0xfed90000) ? 1 : 0;

	error = syscon_get_by_ofw_property(dev, sc->node,
	    "rockchip,u2phy-grf", &sc->u2phy_grf);
	if (error != 0) {
		device_printf(dev, "Cannot get u2phy-grf: %d\n", error);
		return (ENXIO);
	}

	error = syscon_get_by_ofw_property(dev, sc->node,
	    "rockchip,usbdpphy-grf", &sc->usbdpphy_grf);
	if (error != 0) {
		device_printf(dev, "Cannot get usbdpphy-grf: %d\n", error);
		return (ENXIO);
	}

	error = syscon_get_by_ofw_property(dev, sc->node,
	    "rockchip,usb-grf", &sc->usb_grf);
	if (error != 0) {
		device_printf(dev, "Cannot get usb-grf: %d\n", error);
		return (ENXIO);
	}

	error = syscon_get_by_ofw_property(dev, sc->node,
	    "rockchip,vo-grf", &sc->vo_grf);
	if (error != 0)
		device_printf(dev, "%s vo-grf missing error=%d\n",
		    RK3588_USBDP_DIAG, error);

	(void)rk3588_usbdp_acquire_supply(sc, "attach");
	if (!OF_hasprop(sc->node, "phy-supply") && bootverbose)
		device_printf(dev, "%s no phy-supply property\n",
		    RK3588_USBDP_DIAG);

	for (i = 0; i < RK3588_USBDP_PHY_MAX_CLOCKS; i++) {
		error = clk_get_by_ofw_name(dev, 0,
		    rk3588_usbdp_clk_names[i], &sc->clks[i]);
		if (error != 0) {
			device_printf(dev, "%s clock %s missing error=%d\n",
			    RK3588_USBDP_DIAG, rk3588_usbdp_clk_names[i],
			    error);
			return (error);
		}
	}

	for (i = 0; i < RK3588_USBDP_PHY_MAX_RESETS; i++) {
		error = hwreset_get_by_ofw_name(dev, sc->node,
		    rk3588_usbdp_reset_names[i], &sc->resets[i]);
		if (error != 0) {
			device_printf(dev, "%s reset %s missing error=%d\n",
			    RK3588_USBDP_DIAG, rk3588_usbdp_reset_names[i],
			    error);
			return (error);
		}
	}
	error = rk3588_usbdp_assert_resets(sc);
	if (error != 0)
		return (error);

	memset(&phy_init, 0, sizeof(phy_init));
	phy_init.phynode_init_def.id = RK3588_PHY_TYPE_USB3;
	phy_init.phynode_init_def.ofw_node = sc->node;
	phy_init.std_param.usb_mode = PHY_USB_MODE_HOST;

	phynode = phynode_usb_create(dev, &rk3588_usbdp_phynode_class,
	    &phy_init);
	if (phynode == NULL) {
		device_printf(dev, "failed to create USBDP PHY\n");
		return (ENXIO);
	}

	if (phynode_usb_register(phynode) == NULL) {
		device_printf(dev, "failed to register USBDP PHY\n");
		return (ENXIO);
	}

	sc->phynode = phynode;
	OF_device_register_xref(OF_xref_from_node(sc->node), dev);
	return (0);
}

static int
rk3588_usbdp_map(device_t dev, phandle_t xref, int ncells, pcell_t *cells,
    intptr_t *id)
{
	int error;

	error = phydev_default_ofw_map(dev, xref, ncells, cells, id);
	if (error != 0)
		return (error);

	if (*id != RK3588_PHY_TYPE_USB3)
		return (ERANGE);

	return (0);
}

static device_method_t rk3588_usbdp_methods[] = {
	DEVMETHOD(device_probe,		rk3588_usbdp_probe),
	DEVMETHOD(device_attach,	rk3588_usbdp_attach),
	DEVMETHOD(phydev_map,		rk3588_usbdp_map),
	DEVMETHOD(typec_switch_set,	rk3588_usbdp_set_orientation),

	DEVMETHOD_END
};

static driver_t rk3588_usbdp_driver = {
	"rk3588_usbdp",
	rk3588_usbdp_methods,
	sizeof(struct rk3588_usbdp_softc),
};

EARLY_DRIVER_MODULE(rk3588_usbdp, simplebus, rk3588_usbdp_driver, 0, 0,
    BUS_PASS_SUPPORTDEV + BUS_PASS_ORDER_MIDDLE);
MODULE_VERSION(rk3588_usbdp, 1);
