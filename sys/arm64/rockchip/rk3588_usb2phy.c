/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 */

#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>

#include <dev/ofw/openfirm.h>

#include <dev/extres/clk/clk.h>
#include <dev/extres/hwreset/hwreset.h>
#include <dev/extres/regulator/regulator.h>
#include <dev/extres/syscon/syscon.h>

#include "rk_usb2phy.h"
#include "syscon_if.h"

#define	RK3588_USB2PHY_DIAG	"RK3588-DIAG V705 usb2phy"

static void
rk3588_usb2phy_write_prop(struct rk_usb2phy_softc *sc, uint32_t off,
    u_int bit, bool enable)
{
	uint32_t val;

	val = (1U << (bit + 16)) | (enable ? (1U << bit) : 0);
	SYSCON_WRITE_4(sc->grf, off, val);
}

static void
rk3588_usb2phy_host_init(struct rk_usb2phy_softc *sc)
{

	if (sc->host_init_done)
		return;

	rk3588_usb2phy_write_prop(sc, 0x0088, 6, true);
	rk3588_usb2phy_write_prop(sc, 0x0080, 6, true);
	rk3588_usb2phy_write_prop(sc, 0x0088, 5, true);
	rk3588_usb2phy_write_prop(sc, 0x0080, 5, true);
	rk3588_usb2phy_write_prop(sc, 0x0088, 0, true);
	rk3588_usb2phy_write_prop(sc, 0x0080, 0, true);
	sc->host_init_done = true;
}

static void
rk3588_usb2phy_phy_sus(struct rk_usb2phy_softc *sc, intptr_t phy,
    bool enable)
{
	uint32_t off, mask, val;
	u_int bit;

	if (phy == RK_USBPHY_OTG) {
		off = 0x000c;
		bit = 11;
	} else {
		off = sc->regs.host_ctl.offset;
		bit = 2;
	}

	mask = 1U << (bit + 16);
	val = mask | (enable ? 0 : (1U << bit));
	SYSCON_WRITE_4(sc->grf, off, val);
}

static int
rk3588_usb2phy_power_on_reset(struct rk_usb2phy_softc *sc)
{
	int error;

	if (sc->resets == NULL)
		return (0);

	error = hwreset_array_assert(sc->resets);
	if (error != 0) {
		device_printf(sc->dev,
		    "%s power-on-reset assert error=%d\n",
		    RK3588_USB2PHY_DIAG, error);
		return (error);
	}
	DELAY(10);

	error = hwreset_array_deassert(sc->resets);
	if (error != 0) {
		device_printf(sc->dev,
		    "%s power-on-reset deassert error=%d\n",
		    RK3588_USB2PHY_DIAG, error);
		return (error);
	}
	DELAY(200);
	DELAY(2000);

	return (0);
}

static int
rk3588_usb2phy_supply(struct rk_usb2phy_softc *sc)
{
	int error;

	if (sc->running)
		return (0);

	error = clk_enable(sc->clk);
	if (error != 0) {
		device_printf(sc->dev, "Could not enable clock %s\n",
		    clk_get_name(sc->clk));
		return (error);
	}
	SYSCON_WRITE_4(sc->grf, sc->regs.clk_ctl.offset,
	    sc->regs.clk_ctl.enable_mask);
	DELAY(1200);
	sc->running = true;

	return (0);
}

static void
rk3588_usb2phy_tuning(struct rk_usb2phy_softc *sc)
{
	uint32_t suspend_cfg;

	if (sc->tuned)
		return;

	if (sc->reg_base == 0x0000 || sc->reg_base == 0x4000)
		suspend_cfg = 0x15;
	else
		suspend_cfg = 0x14;

	SYSCON_WRITE_4(sc->grf, 0x0008, 0x20000000);
	SYSCON_WRITE_4(sc->grf, 0x000c, 0x001f0000 | suspend_cfg);
	SYSCON_WRITE_4(sc->grf, 0x0004, 0x0f000900);
	SYSCON_WRITE_4(sc->grf, 0x0008, 0x00180010);
	if (sc->reg_base == 0x0000 || sc->reg_base == 0x4000)
		SYSCON_WRITE_4(sc->grf, 0x0010, 0x00030003);
	DELAY(1500);
	sc->tuned = true;
}

static int
rk3588_usb2phy_enable(struct rk_usb2phy_softc *sc, intptr_t phy, bool enable)
{
	int error;

	if (enable && phy == RK_USBPHY_HOST)
		rk3588_usb2phy_host_init(sc);

	if (enable) {
		error = rk3588_usb2phy_supply(sc);
		if (error != 0)
			return (ENXIO);
		rk3588_usb2phy_tuning(sc);
	}

	if (enable && sc->phy_supply == NULL && sc->host_node != 0 &&
	    OF_hasprop(sc->host_node, "phy-supply")) {
		error = regulator_get_by_ofw_property(sc->dev, sc->host_node,
		    "phy-supply", &sc->phy_supply);
		if (error != 0) {
			device_printf(sc->dev,
			    "%s phy-supply not ready during PHY enable error=%d\n",
			    RK3588_USB2PHY_DIAG, error);
			return (ENXIO);
		}
	}

	if (sc->phy_supply != NULL) {
		error = enable ? regulator_enable(sc->phy_supply) :
		    regulator_disable(sc->phy_supply);
		if (error != 0) {
			device_printf(sc->dev, "Cannot %sable the regulator\n",
			    enable ? "En" : "Dis");
			return (ENXIO);
		}
	}

	rk3588_usb2phy_phy_sus(sc, phy, enable);
	DELAY(1500);
	if (enable && rk3588_usb2phy_power_on_reset(sc) != 0)
		return (ENXIO);

	return (0);
}

static void
rk3588_usb2phy_port_init(struct rk_usb2phy_softc *sc, intptr_t phy)
{

	if (phy == RK_USBPHY_HOST)
		rk3588_usb2phy_host_init(sc);
}

const struct rk_usb2phy_regs rk3588_usb2phy_regs = {
	.clk_ctl = {
		.offset = 0x0000,
		.enable_mask = 0x00010000,
		.disable_mask = 0x00010001,
	},
	.host_ctl = {
		.offset = 0x0008,
		.enable_mask = 0x00040000,
		.disable_mask = 0x00040004,
	},
	.has_host_ctl = true,
	.has_otg = true,
	.relative_offsets = true,
	.resets_required = true,
	.clock_enable_delay = 1200,
	.enable = rk3588_usb2phy_enable,
	.port_init = rk3588_usb2phy_port_init,
};
