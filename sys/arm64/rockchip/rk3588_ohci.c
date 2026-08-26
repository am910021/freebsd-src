/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2006 M. Warner Losh <imp@FreeBSD.org>
 * Copyright (c) 2016 Emmanuel Vadot <manu@FreeBSD.org>
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/condvar.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/rman.h>
#include <sys/systm.h>

#include <machine/bus.h>
#include <machine/resource.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usb_core.h>
#include <dev/usb/usb_busdma.h>
#include <dev/usb/usb_process.h>
#include <dev/usb/usb_controller.h>
#include <dev/usb/usb_bus.h>
#include <dev/usb/controller/ohci.h>
#include <dev/usb/controller/ohcireg.h>

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
#define	RK3588_OHCI_NCLOCKS		4
#define	RK3588_OHCI_NPHYS		1

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
	ohci_softc_t			ohci_sc;
	device_t			dev;
	TAILQ_HEAD(, rk3588_ohci_clk)	clocks;
	TAILQ_HEAD(, rk3588_ohci_reset)	resets;
	TAILQ_HEAD(, rk3588_ohci_phy)	phys;
	bool				hcd_initialized;
	bool				hcd_mem_allocated;
	bool				retry_scheduled;
};

static int rk3588_ohci_detach(device_t);

static int
rk3588_ohci_prepare_hcd(device_t dev)
{
	struct rk3588_ohci_softc *sc;
	ohci_softc_t *osc;
	uint32_t control, intrenable, intrstatus, revision;
	int rid, unit;

	sc = device_get_softc(dev);
	unit = device_get_unit(dev);
	osc = &sc->ohci_sc;
	osc->sc_bus.parent = dev;
	osc->sc_bus.devices = osc->sc_devices;
	osc->sc_bus.devices_max = OHCI_MAX_DEVICES;
	osc->sc_bus.dma_bits = 32;

	rid = 0;
	osc->sc_io_res = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid,
	    RF_ACTIVE);
	if (osc->sc_io_res == NULL)
		return (ENOMEM);
	osc->sc_io_tag = rman_get_bustag(osc->sc_io_res);
	osc->sc_io_hdl = rman_get_bushandle(osc->sc_io_res);
	osc->sc_io_size = rman_get_size(osc->sc_io_res);

	/* Linux reaches the RK3588 PHY with the companion OHCI in reset. */
	revision = bus_read_4(osc->sc_io_res, OHCI_REVISION);
	control = bus_read_4(osc->sc_io_res, OHCI_CONTROL);
	intrstatus = bus_read_4(osc->sc_io_res, OHCI_INTERRUPT_STATUS);
	intrenable = bus_read_4(osc->sc_io_res, OHCI_INTERRUPT_ENABLE);
	device_printf(dev,
	    "RK3588-DIAG V2478 OHCI%d pre-PHY revision=%#010x "
	    "control=%#010x intrstatus=%#010x intrenable=%#010x\n",
	    unit, revision, control, intrstatus, intrenable);
	bus_write_4(osc->sc_io_res, OHCI_INTERRUPT_DISABLE, OHCI_ALL_INTRS);
	bus_write_4(osc->sc_io_res, OHCI_INTERRUPT_STATUS, OHCI_ALL_INTRS);
	bus_write_4(osc->sc_io_res, OHCI_CONTROL, OHCI_HCFS_RESET);
	usb_pause_mtx(NULL, USB_MS_TO_TICKS(USB_BUS_RESET_DELAY));
	device_printf(dev,
	    "RK3588-DIAG V2478 OHCI%d reset before PHY control=%#010x\n",
	    unit, bus_read_4(osc->sc_io_res, OHCI_CONTROL));
	return (0);
}

static int
rk3588_ohci_attach_hcd(device_t dev)
{
	struct rk3588_ohci_softc *sc;
	ohci_softc_t *osc;
	int error, rid, unit;

	sc = device_get_softc(dev);
	unit = device_get_unit(dev);
	osc = &sc->ohci_sc;
	if (usb_bus_mem_alloc_all(&osc->sc_bus, USB_GET_DMA_TAG(dev),
	    &ohci_iterate_hw_softc))
		return (ENOMEM);
	sc->hcd_mem_allocated = true;

	rid = 0;
	osc->sc_irq_res = bus_alloc_resource_any(dev, SYS_RES_IRQ, &rid,
	    RF_SHAREABLE | RF_ACTIVE);
	if (osc->sc_irq_res == NULL)
		return (ENXIO);

	osc->sc_bus.bdev = device_add_child(dev, "usbus", -1);
	if (osc->sc_bus.bdev == NULL)
		return (ENXIO);
	device_set_ivars(osc->sc_bus.bdev, &osc->sc_bus);
	strlcpy(osc->sc_vendor, "Rockchip", sizeof(osc->sc_vendor));

	error = bus_setup_intr(dev, osc->sc_irq_res,
	    INTR_TYPE_BIO | INTR_MPSAFE, NULL,
	    (driver_intr_t *)ohci_interrupt, osc, &osc->sc_intr_hdl);
	if (error != 0) {
		osc->sc_intr_hdl = NULL;
		return (error);
	}

	device_printf(dev, "RK3588-DIAG V2478 OHCI%d core init-begin\n",
	    unit);
	error = ohci_init(osc);
	if (error != 0)
		return (error);
	sc->hcd_initialized = true;
	device_printf(dev, "RK3588-DIAG V2478 OHCI%d core init-done\n", unit);
	error = device_probe_and_attach(osc->sc_bus.bdev);
	if (error == 0)
		device_printf(dev,
		    "RK3588-DIAG V2478 OHCI%d root bus attached\n", unit);
	return (error);
}

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
	device_set_desc(dev, "Rockchip RK3588 OHCI controller");
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
	int count, error, index;

	sc = device_get_softc(dev);
	sc->dev = dev;
	TAILQ_INIT(&sc->clocks);
	TAILQ_INIT(&sc->resets);
	TAILQ_INIT(&sc->phys);
	sc->hcd_initialized = false;
	sc->hcd_mem_allocated = false;

	error = rk3588_power_domain_enable_by_node(dev,
	    ofw_bus_get_node(dev));
	if (error != 0 && error != ENOENT)
		goto fail;

	error = rk3588_ohci_preflight_phy_supplies(dev);
	if (error != 0) {
		rk3588_ohci_schedule_retry(sc, "late phy-supply", error);
		goto fail;
	}

	error = ofw_bus_parse_xref_list_get_length(ofw_bus_get_node(dev),
	    "clocks", "#clock-cells", &count);
	if (error != 0 || count != RK3588_OHCI_NCLOCKS) {
		device_printf(dev, "Expected %d clocks, found %d\n",
		    RK3588_OHCI_NCLOCKS, error == 0 ? count : 0);
		if (error == 0)
			error = EINVAL;
		goto fail;
	}
	for (index = 0; index < count; index++) {
		error = clk_get_by_ofw_index(dev, 0, index, &clk);
		if (error != 0)
			goto fail;
		error = clk_enable(clk);
		if (error != 0) {
			clk_release(clk);
			goto fail;
		}
		clock = malloc(sizeof(*clock), M_DEVBUF, M_WAITOK | M_ZERO);
		clock->clk = clk;
		TAILQ_INSERT_TAIL(&sc->clocks, clock, next);
	}

	error = ofw_bus_parse_xref_list_get_length(ofw_bus_get_node(dev),
	    "resets", "#reset-cells", &count);
	if (error == ENOENT)
		count = 0;
	else if (error != 0)
		goto fail;
	for (index = 0; index < count; index++) {
		error = hwreset_get_by_ofw_idx(dev, 0, index, &hwreset);
		if (error != 0)
			goto fail;
		error = hwreset_deassert(hwreset);
		if (error != 0) {
			hwreset_release(hwreset);
			goto fail;
		}
		reset = malloc(sizeof(*reset), M_DEVBUF, M_WAITOK | M_ZERO);
		reset->reset = hwreset;
		TAILQ_INSERT_TAIL(&sc->resets, reset, next);
	}

	error = rk3588_ohci_prepare_hcd(dev);
	if (error != 0)
		goto fail;

	error = ofw_bus_parse_xref_list_get_length(ofw_bus_get_node(dev),
	    "phys", "#phy-cells", &count);
	if (error != 0 || count != RK3588_OHCI_NPHYS) {
		device_printf(dev, "Expected %d PHY, found %d\n",
		    RK3588_OHCI_NPHYS, error == 0 ? count : 0);
		if (error == 0)
			error = EINVAL;
		goto fail;
	}
	for (index = 0; index < count; index++) {
		error = phy_get_by_ofw_idx(dev, 0, index, &phy);
		if (error != 0)
			goto fail;
		error = phy_usb_set_mode(phy, PHY_USB_MODE_HOST);
		if (error == 0)
			error = phy_enable(phy);
		if (error != 0) {
			phy_release(phy);
			rk3588_ohci_schedule_retry(sc, "phy-enable", error);
			goto fail;
		}
		phy_entry = malloc(sizeof(*phy_entry), M_DEVBUF,
		    M_WAITOK | M_ZERO);
		phy_entry->phy = phy;
		TAILQ_INSERT_TAIL(&sc->phys, phy_entry, next);
	}
	error = rk3588_ohci_attach_hcd(dev);
	if (error != 0)
		goto fail;

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
	ohci_softc_t *osc;

	sc = device_get_softc(dev);
	osc = &sc->ohci_sc;
	device_delete_children(dev);
	if (sc->hcd_initialized) {
		ohci_detach(osc);
		sc->hcd_initialized = false;
	} else if (osc->sc_io_res != NULL) {
		bus_write_4(osc->sc_io_res, OHCI_INTERRUPT_DISABLE,
		    OHCI_ALL_INTRS);
		bus_write_4(osc->sc_io_res, OHCI_CONTROL, OHCI_HCFS_RESET);
	}
	if (osc->sc_intr_hdl != NULL) {
		bus_teardown_intr(dev, osc->sc_irq_res, osc->sc_intr_hdl);
		osc->sc_intr_hdl = NULL;
	}
	if (osc->sc_irq_res != NULL) {
		bus_release_resource(dev, SYS_RES_IRQ, 0, osc->sc_irq_res);
		osc->sc_irq_res = NULL;
	}
	if (osc->sc_io_res != NULL) {
		bus_release_resource(dev, SYS_RES_MEMORY, 0, osc->sc_io_res);
		osc->sc_io_res = NULL;
	}
	if (sc->hcd_mem_allocated) {
		usb_bus_mem_free_all(&osc->sc_bus, &ohci_iterate_hw_softc);
		sc->hcd_mem_allocated = false;
	}
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
	"ohci",
	rk3588_ohci_methods,
	sizeof(struct rk3588_ohci_softc),
};

DRIVER_MODULE(rk3588_ohci, simplebus, rk3588_ohci_driver, 0, 0);
