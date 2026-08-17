/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/module.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include <dev/extres/clk/clk.h>
#include <dev/extres/hwreset/hwreset.h>
#include <dev/extres/phy/phy.h>
#include <dev/extres/phy/phy_usb.h>
#include <dev/extres/regulator/regulator.h>

#include <arm64/rockchip/rk3588_power.h>

#define	RK3588_OHCI_RETRY_SCHEDULED	0x00000001

struct rk3588_ohci_clk {
	TAILQ_ENTRY(rk3588_ohci_clk)	next;
	clk_t				clk;
};

struct rk3588_ohci_reset {
	TAILQ_ENTRY(rk3588_ohci_reset)	next;
	hwreset_t			reset;
};

struct rk3588_ohci_phy {
	TAILQ_ENTRY(rk3588_ohci_phy)	next;
	phy_t				phy;
};

struct rk3588_ohci_softc {
	device_t			dev;
	TAILQ_HEAD(, rk3588_ohci_clk)	clocks;
	TAILQ_HEAD(, rk3588_ohci_reset)	resets;
	TAILQ_HEAD(, rk3588_ohci_phy)	phys;
	bool				retry_scheduled;
};

static int rk3588_ohci_detach(device_t);

static void
rk3588_ohci_retry_attach(void *arg)
{
	device_t dev;
	int error;

	dev = arg;
	error = device_probe_and_attach(dev);
	if (error != 0)
		device_printf(dev, "deferred attach failed: %d\n", error);
}

static void
rk3588_ohci_schedule_retry(struct rk3588_ohci_softc *sc,
    const char *reason, int error)
{

	if (sc->retry_scheduled)
		return;
	sc->retry_scheduled = true;
	if (bootverbose)
		device_printf(sc->dev, "deferring attach after %s failed: %d\n",
		    reason, error);
	config_intrhook_oneshot(rk3588_ohci_retry_attach, sc->dev);
}

static int
rk3588_ohci_preflight_phy_supplies(device_t dev)
{
	phandle_t node, phy_node;
	pcell_t *phys;
	regulator_t supply;
	int count, error, i, ncells;

	node = ofw_bus_get_node(dev);
	count = OF_getencprop_alloc_multi(node, "phys", sizeof(*phys),
	    (void **)&phys);
	if (count <= 0)
		return (0);

	for (i = 0; i < count;) {
		phy_node = OF_node_from_xref(phys[i++]);
		if (phy_node == 0)
			continue;
		if (OF_getencprop(phy_node, "#phy-cells", &ncells,
		    sizeof(ncells)) != sizeof(ncells))
			ncells = 0;
		if (!OF_hasprop(phy_node, "phy-supply")) {
			i += ncells;
			continue;
		}
		error = regulator_get_by_ofw_property(dev, phy_node,
		    "phy-supply", &supply);
		if (error != 0) {
			OF_prop_free(phys);
			return (ENXIO);
		}
		regulator_release(supply);
		i += ncells;
	}
	OF_prop_free(phys);
	return (0);
}

static int
rk3588_ohci_probe(device_t dev)
{

	if (!ofw_bus_status_okay(dev) ||
	    !ofw_bus_is_compatible(dev, "rockchip,rk3588-ohci"))
		return (ENXIO);
	device_set_desc(dev, "Rockchip RK3588 OHCI dependencies");
	return (BUS_PROBE_SPECIFIC);
}

static int
rk3588_ohci_attach(device_t dev)
{
	struct rk3588_ohci_softc *sc;
	struct rk3588_ohci_clk *clock;
	struct rk3588_ohci_reset *reset;
	struct rk3588_ohci_phy *phy_entry;
	clk_t clk;
	hwreset_t hwreset;
	phy_t phy;
	int error, index;

	sc = device_get_softc(dev);
	sc->dev = dev;
	TAILQ_INIT(&sc->clocks);
	TAILQ_INIT(&sc->resets);
	TAILQ_INIT(&sc->phys);

	error = rk3588_power_domain_enable_by_node(dev,
	    ofw_bus_get_node(dev));
	if (error != 0 && error != ENOENT)
		goto fail;

	error = rk3588_ohci_preflight_phy_supplies(dev);
	if (error != 0) {
		rk3588_ohci_schedule_retry(sc, "late phy-supply", error);
		goto fail;
	}

	for (index = 0;
	    clk_get_by_ofw_index(dev, 0, index, &clk) == 0; index++) {
		error = clk_enable(clk);
		if (error != 0)
			goto fail;
		clock = malloc(sizeof(*clock), M_DEVBUF, M_WAITOK | M_ZERO);
		clock->clk = clk;
		TAILQ_INSERT_TAIL(&sc->clocks, clock, next);
	}

	for (index = 0;
	    hwreset_get_by_ofw_idx(dev, 0, index, &hwreset) == 0; index++) {
		error = hwreset_deassert(hwreset);
		if (error != 0)
			goto fail;
		reset = malloc(sizeof(*reset), M_DEVBUF, M_WAITOK | M_ZERO);
		reset->reset = hwreset;
		TAILQ_INSERT_TAIL(&sc->resets, reset, next);
	}

	for (index = 0;
	    phy_get_by_ofw_idx(dev, 0, index, &phy) == 0; index++) {
		error = phy_usb_set_mode(phy, PHY_USB_MODE_HOST);
		if (error == 0)
			error = phy_enable(phy);
		if (error != 0) {
			rk3588_ohci_schedule_retry(sc, "phy-enable", error);
			goto fail;
		}
		phy_entry = malloc(sizeof(*phy_entry), M_DEVBUF,
		    M_WAITOK | M_ZERO);
		phy_entry->phy = phy;
		TAILQ_INSERT_TAIL(&sc->phys, phy_entry, next);
	}

	return (0);

fail:
	rk3588_ohci_detach(dev);
	return (error);
}

static int
rk3588_ohci_detach(device_t dev)
{
	struct rk3588_ohci_softc *sc;
	struct rk3588_ohci_clk *clock, *clock_tmp;
	struct rk3588_ohci_reset *reset, *reset_tmp;
	struct rk3588_ohci_phy *phy, *phy_tmp;

	sc = device_get_softc(dev);
	TAILQ_FOREACH_SAFE(phy, &sc->phys, next, phy_tmp) {
		(void)phy_disable(phy->phy);
		phy_release(phy->phy);
		TAILQ_REMOVE(&sc->phys, phy, next);
		free(phy, M_DEVBUF);
	}
	TAILQ_FOREACH_SAFE(reset, &sc->resets, next, reset_tmp) {
		(void)hwreset_assert(reset->reset);
		hwreset_release(reset->reset);
		TAILQ_REMOVE(&sc->resets, reset, next);
		free(reset, M_DEVBUF);
	}
	TAILQ_FOREACH_SAFE(clock, &sc->clocks, next, clock_tmp) {
		(void)clk_disable(clock->clk);
		(void)clk_release(clock->clk);
		TAILQ_REMOVE(&sc->clocks, clock, next);
		free(clock, M_DEVBUF);
	}
	return (0);
}

static device_method_t rk3588_ohci_methods[] = {
	DEVMETHOD(device_probe,		rk3588_ohci_probe),
	DEVMETHOD(device_attach,	rk3588_ohci_attach),
	DEVMETHOD(device_detach,	rk3588_ohci_detach),
	DEVMETHOD_END
};

static driver_t rk3588_ohci_driver = {
	"rk3588_ohci",
	rk3588_ohci_methods,
	sizeof(struct rk3588_ohci_softc),
};

DRIVER_MODULE(rk3588_ohci, simplebus, rk3588_ohci_driver, 0, 0);
