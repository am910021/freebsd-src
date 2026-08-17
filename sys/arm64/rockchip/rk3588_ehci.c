/*-
 * Copyright (c) 2012 Ganbold Tsagaankhuu <ganbold@freebsd.org>
 * Copyright (c) 2016 The FreeBSD Foundation
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 * All rights reserved.
 *
 * This software was developed by Andrew Turner under
 * sponsorship from the FreeBSD Foundation.
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
 */

#include "opt_bus.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/condvar.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/rman.h>

#include <machine/bus.h>
#include <machine/resource.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>

#include <dev/usb/usb_core.h>
#include <dev/usb/usb_busdma.h>
#include <dev/usb/usb_process.h>

#include <dev/usb/usb_controller.h>
#include <dev/usb/usb_bus.h>
#include <dev/usb/controller/ehci.h>

#include <dev/fdt/fdt_common.h>
#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include <dev/extres/clk/clk.h>
#include <dev/extres/hwreset/hwreset.h>
#include <dev/extres/phy/phy.h>
#include <dev/extres/phy/phy_usb.h>
#include <dev/extres/regulator/regulator.h>

#include <arm64/rockchip/rk3588_power.h>

#include <dev/usb/controller/generic_ehci.h>

#define	RK3588_EHCI_RETRY_SCHEDULED	0x00000001

#define	RK3588_EHCI_USBINTR	0x08

struct clk_list {
	TAILQ_ENTRY(clk_list)	next;
	clk_t			clk;
};

struct hwrst_list {
	TAILQ_ENTRY(hwrst_list)	next;
	hwreset_t		rst;
};

struct phy_list {
	TAILQ_ENTRY(phy_list)	next;
	phy_t			phy;
};

struct rk3588_ehci_softc {
	ehci_softc_t	ehci_sc;

	TAILQ_HEAD(, clk_list)	clk_list;
	TAILQ_HEAD(, hwrst_list)	rst_list;
	TAILQ_HEAD(, phy_list)		phy_list;
	bool				rk3588_ehci_initialized;
};

static device_probe_t rk3588_ehci_probe;
static device_attach_t rk3588_ehci_attach;
static device_detach_t rk3588_ehci_detach;

static void
rk3588_ehci_rk3588_retry_attach(void *arg)
{
	device_t dev;
	int error;

	dev = arg;
	error = device_probe_and_attach(dev);
	if (error != 0)
		device_printf(dev, "deferred attach failed: %d\n", error);
}

static int
rk3588_ehci_rk3588_enable_phy_supplies(device_t dev, bool enable)
{
	phandle_t node, phy_node;
	pcell_t *phys;
	regulator_t supply;
	int count, error, i, ncells;

	if (!ofw_bus_is_compatible(dev, "rockchip,rk3588-ehci"))
		return (0);

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
		if (error == 0) {
			if (enable) {
				error = regulator_enable(supply);
			}
			regulator_release(supply);
			if (error != 0) {
				device_printf(dev,
				    "Could not enable PHY supply: %d\n", error);
				free(phys, M_OFWPROP);
				return (error);
			}
		} else {
			if ((device_get_flags(dev) &
			    RK3588_EHCI_RETRY_SCHEDULED) == 0) {
				device_set_flags(dev, device_get_flags(dev) |
				    RK3588_EHCI_RETRY_SCHEDULED);
				if (bootverbose)
					device_printf(dev,
					    "PHY supply is not ready; deferring attach\n");
				config_intrhook_oneshot(
				    rk3588_ehci_rk3588_retry_attach, dev);
			}
			free(phys, M_OFWPROP);
			return (ENXIO);
		}

		i += ncells;
	}

	free(phys, M_OFWPROP);
	return (0);
}

static int
rk3588_ehci_enable_phys(device_t dev)
{
	struct rk3588_ehci_softc *sc;
	struct phy_list *phyp;
	phy_t phy;
	int err, off;

	sc = device_get_softc(dev);

	for (off = 0; phy_get_by_ofw_idx(dev, 0, off, &phy) == 0; off++) {
		err = phy_usb_set_mode(phy, PHY_USB_MODE_HOST);
		if (err != 0) {
			device_printf(dev, "Could not set phy to host mode\n");
			return (err);
		}
		err = phy_enable(phy);
		if (err != 0) {
			device_printf(dev, "Could not enable phy\n");
			return (err);
		}
		phyp = malloc(sizeof(*phyp), M_DEVBUF, M_WAITOK | M_ZERO);
		phyp->phy = phy;
		TAILQ_INSERT_TAIL(&sc->phy_list, phyp, next);
	}

	return (0);
}

static void
rk3588_ehci_rk3588_schedule_retry(device_t dev, const char *reason,
    int error)
{

	if (!ofw_bus_is_compatible(dev, "rockchip,rk3588-ehci"))
		return;
	if ((device_get_flags(dev) & RK3588_EHCI_RETRY_SCHEDULED) != 0)
		return;

	device_set_flags(dev, device_get_flags(dev) |
	    RK3588_EHCI_RETRY_SCHEDULED);
	if (bootverbose)
		device_printf(dev, "deferring attach after %s failed: %d\n",
		    reason, error);
	config_intrhook_oneshot(rk3588_ehci_rk3588_retry_attach, dev);
}

static int
rk3588_ehci_rk3588_openbsd_order_attach(device_t dev)
{
	struct rk3588_ehci_softc *sc;
	ehci_softc_t *esc;
	uint8_t caplen;
	int err, rid;

	sc = device_get_softc(dev);
	esc = &sc->ehci_sc;

	esc->sc_bus.parent = dev;
	esc->sc_bus.devices = esc->sc_devices;
	esc->sc_bus.devices_max = EHCI_MAX_DEVICES;
	esc->sc_bus.dma_bits = 32;

	if (usb_bus_mem_alloc_all(&esc->sc_bus, USB_GET_DMA_TAG(dev),
	    &ehci_iterate_hw_softc))
		return (ENOMEM);
	esc->sc_bus.dma_xfer_alloc_mode = USB_DMA_ALLOC_STREAMING_PAYLOAD;

	esc->sc_bus.usbrev = USB_REV_2_0;

	rid = 0;
	esc->sc_io_res = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid,
	    RF_ACTIVE);
	if (esc->sc_io_res == NULL) {
		device_printf(dev, "Could not map memory\n");
		return (ENXIO);
	}

	esc->sc_io_tag = rman_get_bustag(esc->sc_io_res);
	esc->sc_io_hdl = rman_get_bushandle(esc->sc_io_res);
	esc->sc_io_size = rman_get_size(esc->sc_io_res);

	caplen = bus_space_read_4(esc->sc_io_tag, esc->sc_io_hdl, 0) & 0xff;
	esc->sc_offs = caplen;
	bus_space_write_4(esc->sc_io_tag, esc->sc_io_hdl,
	    caplen + RK3588_EHCI_USBINTR, 0);

	rid = 0;
	esc->sc_irq_res = bus_alloc_resource_any(dev, SYS_RES_IRQ, &rid,
	    RF_SHAREABLE | RF_ACTIVE);
	if (esc->sc_irq_res == NULL) {
		device_printf(dev, "Could not allocate irq\n");
		return (ENXIO);
	}

	esc->sc_bus.bdev = device_add_child(dev, "usbus", -1);
	if (esc->sc_bus.bdev == NULL) {
		device_printf(dev, "Could not add USB device\n");
		return (ENXIO);
	}
	device_set_ivars(esc->sc_bus.bdev, &esc->sc_bus);
	strlcpy(esc->sc_vendor, "Generic", sizeof(esc->sc_vendor));

	err = bus_setup_intr(dev, esc->sc_irq_res,
	    INTR_TYPE_BIO | INTR_MPSAFE, NULL,
	    (driver_intr_t *)ehci_interrupt, esc, &esc->sc_intr_hdl);
	if (err != 0) {
		device_printf(dev, "Could not setup irq: %d\n", err);
		esc->sc_intr_hdl = NULL;
		return (err);
	}

	err = rk3588_ehci_enable_phys(dev);
	if (err != 0) {
		rk3588_ehci_rk3588_schedule_retry(dev, "phy-enable", err);
		return (err);
	}

	/*
	 * OpenBSD's RK3588 rkusbphy host enable performs the PHY side
	 * effects, then returns EINVAL, so ehci_init_phys() continues into
	 * the "phys" fallback.  Keep FreeBSD's phynode_enable() success
	 * semantics, but still run the same fallback boundary here.
	 */
	err = rk3588_ehci_rk3588_enable_phy_supplies(dev, true);
	if (err != 0) {
		rk3588_ehci_rk3588_schedule_retry(dev, "phys-fallback",
		    err);
		return (err);
	}

	esc->sc_init_quirks |= EHCI_INITQ_MMIO_BARRIER |
	    EHCI_INITQ_INTR_AFTER_RUN |
	    EHCI_INITQ_INTR_QH_FRAMELIST |
	    EHCI_INITQ_NRL_4;
	err = ehci_init(esc);
	if (err == 0)
		sc->rk3588_ehci_initialized = true;
	if (err == 0)
		err = device_probe_and_attach(esc->sc_bus.bdev);
	if (err != 0)
		return (err);

	return (0);
}

static int
rk3588_ehci_probe(device_t self)
{

	if (!ofw_bus_status_okay(self))
		return (ENXIO);

	if (!ofw_bus_is_compatible(self, "rockchip,rk3588-ehci"))
		return (ENXIO);

	device_set_desc(self, "Rockchip RK3588 EHCI Controller");
	return (BUS_PROBE_SPECIFIC);
}

static int
rk3588_ehci_attach(device_t dev)
{
	int err;
	struct rk3588_ehci_softc *sc;
	struct clk_list *clkp;
	clk_t clk;
	struct hwrst_list *rstp;
	hwreset_t rst;
	int off;

	sc = device_get_softc(dev);
	if (ofw_bus_is_compatible(dev, "rockchip,rk3588-ehci"))
		sc->rk3588_ehci_initialized = false;

	TAILQ_INIT(&sc->clk_list);
	TAILQ_INIT(&sc->phy_list);
	if (ofw_bus_is_compatible(dev, "rockchip,rk3588-ehci")) {
		err = rk3588_power_domain_enable_by_node(dev,
		    ofw_bus_get_node(dev));
		if (err != 0 && err != ENOENT) {
			device_printf(dev, "Could not enable power domain: %d\n",
			    err);
			goto error;
		}
		err = rk3588_ehci_rk3588_enable_phy_supplies(dev, false);
		if (err != 0)
			goto error;
	}

	/* Enable every clock declared by the DT, including usb480m_phy*. */
	for (off = 0; clk_get_by_ofw_index(dev, 0, off, &clk) == 0; off++) {
		err = clk_enable(clk);
		if (err != 0) {
			device_printf(dev, "Could not enable clock %s\n",
			    clk_get_name(clk));
			goto error;
		}
		clkp = malloc(sizeof(*clkp), M_DEVBUF, M_WAITOK | M_ZERO);
		clkp->clk = clk;
		TAILQ_INSERT_TAIL(&sc->clk_list, clkp, next);
	}
	/* De-assert reset */
	TAILQ_INIT(&sc->rst_list);
	for (off = 0; hwreset_get_by_ofw_idx(dev, 0, off, &rst) == 0; off++) {
		err = hwreset_deassert(rst);
		if (err != 0) {
			device_printf(dev, "Could not de-assert reset\n");
			goto error;
		}
		rstp = malloc(sizeof(*rstp), M_DEVBUF, M_WAITOK | M_ZERO);
		rstp->rst = rst;
		TAILQ_INSERT_TAIL(&sc->rst_list, rstp, next);
	}
	if (ofw_bus_is_compatible(dev, "rockchip,rk3588-ehci")) {
		err = rk3588_ehci_rk3588_openbsd_order_attach(dev);
		if (err != 0)
			goto error;
		return (0);
	}

	/* Enable USB PHY */
	err = rk3588_ehci_enable_phys(dev);
	if (err != 0)
		goto error;

	err = rk3588_ehci_rk3588_enable_phy_supplies(dev, true);
	if (err != 0)
		goto error;

	err = generic_ehci_attach(dev);
	if (err != 0)
		goto error;

	return (0);

error:
	rk3588_ehci_detach(dev);
	return (err);
}

static int
rk3588_ehci_detach(device_t dev)
{
	struct rk3588_ehci_softc *sc;
	ehci_softc_t *esc;
	struct clk_list *clk, *clk_tmp;
	struct hwrst_list *rst, *rst_tmp;
	struct phy_list *phy, *phy_tmp;
	int err;

	sc = device_get_softc(dev);
	esc = &sc->ehci_sc;

	if (ofw_bus_is_compatible(dev, "rockchip,rk3588-ehci") &&
	    esc->sc_io_res != NULL && !sc->rk3588_ehci_initialized) {
		if (esc->sc_intr_hdl != NULL) {
			bus_teardown_intr(dev, esc->sc_irq_res,
			    esc->sc_intr_hdl);
			esc->sc_intr_hdl = NULL;
		}
		if (esc->sc_irq_res != NULL) {
			bus_release_resource(dev, SYS_RES_IRQ, 0,
			    esc->sc_irq_res);
			esc->sc_irq_res = NULL;
		}
		if (esc->sc_io_res != NULL) {
			bus_release_resource(dev, SYS_RES_MEMORY, 0,
			    esc->sc_io_res);
			esc->sc_io_res = NULL;
		}
		usb_bus_mem_free_all(&esc->sc_bus, &ehci_iterate_hw_softc);
	} else {
		err = generic_ehci_detach(dev);
		if (err != 0)
			return (err);
	}

	/* Disable clock */
	TAILQ_FOREACH_SAFE(clk, &sc->clk_list, next, clk_tmp) {
		err = clk_disable(clk->clk);
		if (err != 0)
			device_printf(dev, "Could not disable clock %s\n",
			    clk_get_name(clk->clk));
		err = clk_release(clk->clk);
		if (err != 0)
			device_printf(dev, "Could not release clock %s\n",
			    clk_get_name(clk->clk));
		TAILQ_REMOVE(&sc->clk_list, clk, next);
		free(clk, M_DEVBUF);
	}

	/* Assert reset */
	TAILQ_FOREACH_SAFE(rst, &sc->rst_list, next, rst_tmp) {
		hwreset_assert(rst->rst);
		hwreset_release(rst->rst);
		TAILQ_REMOVE(&sc->rst_list, rst, next);
		free(rst, M_DEVBUF);
	}

	/* Disable phys */
	TAILQ_FOREACH_SAFE(phy, &sc->phy_list, next, phy_tmp) {
		err = phy_disable(phy->phy);
		if (err != 0)
			device_printf(dev, "Could not disable phy\n");
		phy_release(phy->phy);
		TAILQ_REMOVE(&sc->phy_list, phy, next);
		free(phy, M_DEVBUF);
	}

	return (0);
}

static device_method_t rk3588_ehci_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,		rk3588_ehci_probe),
	DEVMETHOD(device_attach,	rk3588_ehci_attach),
	DEVMETHOD(device_detach,	rk3588_ehci_detach),

	DEVMETHOD_END
};

DEFINE_CLASS_1(ehci, rk3588_ehci_driver, rk3588_ehci_methods,
    sizeof(struct rk3588_ehci_softc), generic_ehci_driver);

DRIVER_MODULE(rk3588_ehci, simplebus, rk3588_ehci_driver, 0, 0);
MODULE_DEPEND(rk3588_ehci, usb, 1, 1, 1);
