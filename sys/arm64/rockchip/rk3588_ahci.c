/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2020 Alstom Group
 * Copyright (c) 2020 Semihalf
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/rman.h>

#include <machine/bus.h>
#include <machine/resource.h>

#include <dev/ahci/ahci.h>

#include <dev/extres/clk/clk.h>
#include <dev/extres/phy/phy.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#define	RK3588_AHCI_NCLKS	5

struct rk3588_ahci_softc {
	struct ahci_controller	ctlr;	/* Must be the first field. */
	clk_t			clks[RK3588_AHCI_NCLKS];
	int			nclks_enabled;
	phy_t			phy;
	bool			phy_enabled;
};

static const char * const rk3588_ahci_clk_names[RK3588_AHCI_NCLKS] = {
	"sata", "pmalive", "rxoob", "ref", "asic"
};

static void
rk3588_ahci_release_resources(struct rk3588_ahci_softc *sc)
{
	int i;

	if (sc->phy != NULL) {
		if (sc->phy_enabled)
			(void)phy_disable(sc->phy);
		phy_release(sc->phy);
		sc->phy = NULL;
		sc->phy_enabled = false;
	}
	for (i = RK3588_AHCI_NCLKS - 1; i >= 0; i--) {
		if (sc->clks[i] != NULL) {
			if (i < sc->nclks_enabled)
				(void)clk_disable(sc->clks[i]);
			clk_release(sc->clks[i]);
			sc->clks[i] = NULL;
		}
	}
	sc->nclks_enabled = 0;
}

static int
rk3588_ahci_enable_resources(device_t dev, struct rk3588_ahci_softc *sc)
{
	phandle_t node, port;
	pcell_t reg;
	int error, i;

	node = ofw_bus_get_node(dev);
	for (port = OF_child(node); port > 0; port = OF_peer(port)) {
		if (!ofw_bus_node_status_okay(port))
			continue;
		if (OF_getencprop(port, "reg", &reg, sizeof(reg)) ==
		    sizeof(reg) && reg == 0)
			break;
	}
	if (port <= 0) {
		device_printf(dev, "cannot find SATA port 0\n");
		return (ENXIO);
	}
	for (i = 0; i < RK3588_AHCI_NCLKS; i++) {
		error = clk_get_by_ofw_name(dev, node,
		    rk3588_ahci_clk_names[i], &sc->clks[i]);
		if (error != 0) {
			device_printf(dev, "cannot get %s clock: %d\n",
			    rk3588_ahci_clk_names[i], error);
			goto fail;
		}
	}
	for (i = 0; i < RK3588_AHCI_NCLKS; i++) {
		error = clk_enable(sc->clks[i]);
		if (error != 0) {
			device_printf(dev, "cannot enable %s clock: %d\n",
			    rk3588_ahci_clk_names[i], error);
			goto fail;
		}
		sc->nclks_enabled++;
	}

	error = phy_get_by_ofw_name(dev, port, "sata-phy", &sc->phy);
	if (error != 0) {
		device_printf(dev, "cannot get SATA PHY: %d\n", error);
		goto fail;
	}
	error = phy_enable(sc->phy);
	if (error != 0) {
		device_printf(dev, "cannot enable SATA PHY: %d\n", error);
		goto fail;
	}
	sc->phy_enabled = true;
	return (0);

fail:
	rk3588_ahci_release_resources(sc);
	return (error);
}

static int
rk3588_ahci_probe(device_t dev)
{

	if (!ofw_bus_status_okay(dev) ||
	    !ofw_bus_is_compatible(dev, "rockchip,rk3588-dwc-ahci"))
		return (ENXIO);
	device_set_desc(dev, "Rockchip RK3588 AHCI SATA controller");
	return (BUS_PROBE_SPECIFIC);
}

static int
rk3588_ahci_attach(device_t dev)
{
	struct rk3588_ahci_softc *sc;
	struct ahci_controller *ctlr;
	phandle_t node;
	int error;
	uint32_t saved_cap, saved_cap2, saved_pi;

	sc = device_get_softc(dev);
	ctlr = &sc->ctlr;
	node = ofw_bus_get_node(dev);
	ctlr->dev = dev;
	ctlr->r_rid = 0;
	ctlr->numirqs = 1;
	ctlr->quirks = AHCI_Q_FORCE_PI;
	ctlr->dma_coherent = OF_hasprop(node, "dma-coherent");

	error = rk3588_ahci_enable_resources(dev, sc);
	if (error != 0)
		return (error);

	ctlr->r_mem = bus_alloc_resource_any(dev, SYS_RES_MEMORY,
	    &ctlr->r_rid, RF_ACTIVE);
	if (ctlr->r_mem == NULL) {
		error = ENXIO;
		goto fail;
	}
	saved_cap = ATA_INL(ctlr->r_mem, AHCI_CAP);
	saved_cap2 = ATA_INL(ctlr->r_mem, AHCI_CAP2);
	saved_pi = ATA_INL(ctlr->r_mem, AHCI_PI);
	if (saved_pi == 0)
		saved_pi = 1;
	error = ahci_ctlr_reset(dev);
	if (error != 0) {
		bus_release_resource(dev, SYS_RES_MEMORY, ctlr->r_rid,
		    ctlr->r_mem);
		ctlr->r_mem = NULL;
		goto fail;
	}
	/* Match Linux ahci_restore_initial_config() after global reset. */
	ATA_OUTL(ctlr->r_mem, AHCI_CAP, saved_cap);
	ATA_OUTL(ctlr->r_mem, AHCI_CAP2, saved_cap2);
	ATA_OUTL(ctlr->r_mem, AHCI_PI, saved_pi);
	(void)ATA_INL(ctlr->r_mem, AHCI_PI);
	error = ahci_attach(dev);
	if (error == 0)
		return (0);
	ctlr->r_mem = NULL;

fail:
	rk3588_ahci_release_resources(sc);
	return (error);
}

static int
rk3588_ahci_detach(device_t dev)
{
	struct rk3588_ahci_softc *sc;
	int error;

	sc = device_get_softc(dev);
	error = ahci_detach(dev);
	rk3588_ahci_release_resources(sc);
	return (error);
}

static device_method_t rk3588_ahci_methods[] = {
	DEVMETHOD(device_probe,		rk3588_ahci_probe),
	DEVMETHOD(device_attach,	rk3588_ahci_attach),
	DEVMETHOD(device_detach,	rk3588_ahci_detach),
	DEVMETHOD(bus_alloc_resource,	ahci_alloc_resource),
	DEVMETHOD(bus_release_resource,	ahci_release_resource),
	DEVMETHOD(bus_setup_intr,	ahci_setup_intr),
	DEVMETHOD(bus_teardown_intr,	ahci_teardown_intr),
	DEVMETHOD(bus_print_child,	ahci_print_child),
	DEVMETHOD(bus_child_location,	ahci_child_location),
	DEVMETHOD(bus_get_dma_tag,	ahci_get_dma_tag),
	DEVMETHOD_END
};

static driver_t rk3588_ahci_driver = {
	"ahci",
	rk3588_ahci_methods,
	sizeof(struct rk3588_ahci_softc),
};

DRIVER_MODULE(rk3588_ahci, simplebus, rk3588_ahci_driver, NULL, NULL);
