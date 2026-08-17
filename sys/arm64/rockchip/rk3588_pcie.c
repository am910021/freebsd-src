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
 *    notice, this list of conditions and the following disclaimer,
 *    without modification, immediately at the beginning of the file.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/endian.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/bus.h>
#include <sys/proc.h>
#include <sys/rman.h>
#include <sys/intr.h>
#include <sys/mutex.h>
#include <sys/gpio.h>

#include <dev/gpio/gpiobusvar.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#include <dev/ofw/ofw_pci.h>
#include <dev/ofw/ofwpci.h>

#include <dev/pci/pcivar.h>
#include <dev/pci/pcireg.h>
#include <dev/pci/pcib_private.h>
#include <dev/pci/pci_dw.h>

#include <arm64/rockchip/rk3588_pci_dw.h>

#include <dev/extres/clk/clk.h>
#include <dev/extres/phy/phy.h>
#include <dev/extres/regulator/regulator.h>
#include <dev/extres/hwreset/hwreset.h>

#include <machine/bus.h>
#include <machine/intr.h>

#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_kern.h>
#include <vm/pmap.h>

#include "rk3588_power.h"
#include "pcib_if.h"

/* APB Registers */
#define	PCIE_CLIENT_GENERAL_CON		0x0000
#define	 DEVICE_TYPE_MASK		0x00f0
#define	 DEVICE_TYPE_RC			(1<<6)
#define	 LTSSM_ENABLE			(1<<2)
#define	PCIE_CLIENT_INTR_MASK_MSG_RX	0x0018
#define	PCIE_CLIENT_INTR_MASK_LEGACY	0x001c
#define	PCIE_CLIENT_INTR_MASK_ERR	0x0020
#define	PCIE_CLIENT_POWER_CON		0x002c
#define	 PCIE_CLKREQ_READY		((1U << 16) | (1U << 0))
#define	 PCIE_CLKREQ_NOT_READY		(1U << 16)
#define	 PCIE_CLKREQ_PULL_DOWN		((0x3U << (12 + 16)) | (0x1U << 12))
#define	PCIE_CLIENT_HOT_RESET_CTRL	0x0180
#define	 APP_LSSTM_ENABLE_ENHANCE	(1<<4)
#define	PCIE_CLIENT_LTSSM_STATUS	0x0300
#define	 RDLH_LINK_UP			(1<<17)
#define	 SMLH_LINK_UP			(1<<16)
#define	 SMLH_LTSSM_STATE_MASK		0x003f
#define	 SMLH_LTSSM_STATE_LINK_UP	((1<<4) | (1<<0))

#define	RK3588_CRU_BASE			0xfd7c0000
#define	RK3588_CRU_SIZE			0x9000
#define	RK3588_CRU_CLKSEL_CON80		0x440
#define	RK3588_CRU_GATE_CON32		0x880
#define	RK3588_CRU_GATE_CON34		0x888
#define	RK3588_PHP_GATE_CON0		0x8800
#define	RK3588_GATE_HIWORD_ENABLE(mask)	((mask) << 16)
#define	RK3588_PCLK_PHP_ROOT_GATE	(1U << 0)
#define	RK3588_ACLK_PCIE_ROOT_GATE	(1U << 6)
#define	RK3588_ACLK_PHP_ROOT_GATE	(1U << 7)
#define	RK3588_ACLK_PCIE_BRIDGE_GATE	(1U << 8)
#define	RK3588_ACLK_MMU_PCIE_GATE	(1U << 7)
#define	RK3588_PCLK_PCIE_COMBO_PIPE_PHY_GATE	(1U << 8)
#define	RK3588_PCIE_ROOT_CLOCK_MASK	0x3fffU
#define	RK3588_PCIE_ROOT_CLOCK_LINUX	0x0288U
#define	RK3588_HIWORD_UPDATE(val, mask) \
	(((mask) << 16) | ((val) & (mask)))

#define	RK3588_PCIE_RETRY_SCHEDULED		(1U << 30)

struct rk3588_pcie_softc {
	struct pci_dw_softc		dw_sc;  /* Must be first */
	int				apb_rid;
	struct resource			*apb_res;
	int				dbi_rid;
	struct resource			*dbi_res;
	int				irq_rid;
	struct resource			*irq_res;
	void				*irq_handle;
	phandle_t			node;
	struct gpiobus_pin		*reset_gpio;
	clk_t				aclk_mst, aclk_slv, aclk_dbi, pclk, aux;
	clk_t				pipe_clk;
	regulator_t			regulator;
	hwreset_t			hwreset;
	hwreset_t			pwr_reset;
	phy_t				phy;
};

static struct ofw_compat_data compat_data[] = {
	{"rockchip,rk3588-pcie",	1},
	{NULL,				0}
};

static void
rk3588_pcie_retry_attach(void *arg)
{
	(void)device_probe_and_attach(arg);
}

static void
rk3588_intr(void *data)
{

	(void)data;
}

static int
rk3588_pcie_get_link(device_t dev, bool *status)
{
	struct rk3588_pcie_softc *sc = device_get_softc(dev);
	uint32_t val;

	val = bus_read_4(sc->apb_res, PCIE_CLIENT_LTSSM_STATUS);
	if (((val & (RDLH_LINK_UP | SMLH_LINK_UP)) ==
	    (RDLH_LINK_UP | SMLH_LINK_UP)) &&
	    ((val & SMLH_LTSSM_STATE_MASK) == SMLH_LTSSM_STATE_LINK_UP))
		*status = true;
	else
		*status = false;
	return (0);
}

static void
rk3588_pcie_enable_power_domains(device_t dev)
{
	int error;

	error = rk3588_power_domain_enable(dev, RK3588_PD_PCIE);
	if (error != 0)
		device_printf(dev, "failed to enable PD_PCIE: %d\n", error);
}

static void
rk3588_pcie_enable_domain_clocks(device_t dev)
{
	void *cru_mem;
	volatile uint32_t *cru;
	uint32_t con32_mask, con34_mask;

	cru_mem = pmap_mapdev(RK3588_CRU_BASE, RK3588_CRU_SIZE);
	if (cru_mem == NULL) {
		device_printf(dev, "failed to map CRU domain clocks\n");
		return;
	}
	cru = (volatile uint32_t *)cru_mem;

	/* Match Linux's 500 MHz PCIe and 396 MHz PHP AXI root clocks. */
	cru[RK3588_CRU_CLKSEL_CON80 / sizeof(uint32_t)] =
	    RK3588_HIWORD_UPDATE(RK3588_PCIE_ROOT_CLOCK_LINUX,
	    RK3588_PCIE_ROOT_CLOCK_MASK);

	/*
	 * Linux genpd enables the PD_PCIE domain clocks
	 * PCLK_PHP_ROOT, ACLK_PCIE_ROOT and ACLK_PHP_ROOT while powering the
	 * domain.  FreeBSD does not yet have that power-domain provider.  Also
	 * ungate the immediate PCIe bridge/MMU parent clocks because the
	 * temporary CRU tree models some of those parents as fixed-rate leaves.
	 */
	con32_mask = RK3588_PCLK_PHP_ROOT_GATE |
	    RK3588_ACLK_PCIE_ROOT_GATE |
	    RK3588_ACLK_PHP_ROOT_GATE |
	    RK3588_ACLK_PCIE_BRIDGE_GATE;
	con34_mask = RK3588_ACLK_MMU_PCIE_GATE;
	cru[RK3588_CRU_GATE_CON32 / sizeof(uint32_t)] =
	    RK3588_GATE_HIWORD_ENABLE(con32_mask);
	cru[RK3588_CRU_GATE_CON34 / sizeof(uint32_t)] =
	    RK3588_GATE_HIWORD_ENABLE(con34_mask);
	cru[RK3588_PHP_GATE_CON0 / sizeof(uint32_t)] =
	    RK3588_GATE_HIWORD_ENABLE(RK3588_PCLK_PCIE_COMBO_PIPE_PHY_GATE);

	pmap_unmapdev(cru_mem, RK3588_CRU_SIZE);
}

static int
rk3588_pcie_wait_link(device_t dev)
{
	bool status;
	int count;

	status = false;
	for (count = 0; count < 20; count++) {
		rk3588_pcie_get_link(dev, &status);
		if (status)
			return (0);
		DELAY(100000);
	}

	device_printf(dev, "link up timeout\n");
	return (ETIMEDOUT);
}

static void
rk3588_pcie_wait_endpoint_config(device_t dev)
{
	struct rk3588_pcie_softc *sc = device_get_softc(dev);
	uint32_t id;
	int i;

	for (i = 0; i < 50; i++) {
		id = PCIB_READ_CONFIG(dev, sc->dw_sc.sub_bus, 0, 0,
		    PCIR_VENDOR, 4);
		if (id != 0 && id != 0xffffffff &&
		    (id & 0xffff) != 0xffff && (id & 0xffff) != 0)
			return;
		DELAY(20000);
	}

	device_printf(dev, "endpoint config space not ready\n");
}

static void
rk3588_pcie_configure_clkreq(device_t dev)
{
	struct rk3588_pcie_softc *sc = device_get_softc(dev);

	if (!ofw_bus_is_compatible(dev, "rockchip,rk3588-pcie"))
		return;

	if (OF_hasprop(sc->node, "supports-clkreq")) {
		bus_write_4(sc->apb_res, PCIE_CLIENT_POWER_CON,
		    PCIE_CLKREQ_READY);
	} else {
		bus_write_4(sc->apb_res, PCIE_CLIENT_POWER_CON,
		    PCIE_CLKREQ_PULL_DOWN | PCIE_CLKREQ_NOT_READY);
	}
}

static int
rk3588_pcie_init_soc(device_t dev)
{
	struct rk3588_pcie_softc *sc = device_get_softc(dev);
	int err;

	rk3588_pcie_enable_domain_clocks(dev);
	rk3588_pcie_enable_power_domains(dev);

	/* Assert reset */
	if (sc->pwr_reset != NULL) {
		if (hwreset_assert(sc->pwr_reset))
			device_printf(dev, "Could not assert pwr reset\n");
	}
	if (hwreset_assert(sc->hwreset))
		device_printf(dev, "Could not assert reset\n");
	DELAY(10);

	/* Powerup PCIe */
	if (sc->regulator != NULL) {
		if (regulator_enable(sc->regulator))
			device_printf(dev, "Cannot enable regulator\n");
	}

	/* Linux enables all controller clocks while resets remain asserted. */
	if ((err = clk_enable(sc->aclk_mst))) {
		device_printf(dev, "Could not enable aclk_mst clk\n");
		return (ENXIO);
	}
	if ((err = clk_enable(sc->aclk_slv))) {
		device_printf(dev, "Could not enable aclk_slv clk\n");
		return (ENXIO);
	}
	if ((err = clk_enable(sc->aclk_dbi))) {
		device_printf(dev, "Could not enable aclk_dbi clk\n");
		return (ENXIO);
	}
	if ((err = clk_enable(sc->pclk))) {
		device_printf(dev, "Could not enable pclk clk\n");
		return (ENXIO);
	}
	if ((err = clk_enable(sc->aux))) {
		device_printf(dev, "Could not enable aux clk\n");
		return (ENXIO);
	}
	if (sc->pipe_clk != NULL) {
		if ((err = clk_enable(sc->pipe_clk))) {
			device_printf(dev, "Could not enable pipe clk\n");
			return (ENXIO);
		}
	}

	/* Enable PHY */
	if (phy_enable(sc->phy))
		device_printf(dev, "Cannot enable phy\n");

	/* Deassert reset */
	if (sc->pwr_reset != NULL) {
		if (hwreset_deassert(sc->pwr_reset))
			device_printf(dev, "Could not deassert pwr reset\n");
	}
	if (hwreset_deassert(sc->hwreset))
		device_printf(dev, "Could not deassert reset\n");

	/* Set Root Complex (RC) mode */
	bus_write_4(sc->apb_res, PCIE_CLIENT_HOT_RESET_CTRL,
	    (APP_LSSTM_ENABLE_ENHANCE << 16) | APP_LSSTM_ENABLE_ENHANCE);
	bus_write_4(sc->apb_res, PCIE_CLIENT_GENERAL_CON,
	    (DEVICE_TYPE_MASK << 16) | DEVICE_TYPE_RC);
	rk3588_pcie_configure_clkreq(dev);
	if ((err = pci_dw_init_core(dev))) {
		device_printf(dev, "cannot initialize DesignWare core: %d\n", err);
		return (ENXIO);
	}
	if ((err = pci_dw_rk3588_pre_link_setup(dev)) != 0) {
		device_printf(dev, "pre-link setup failed: %d\n", err);
		return (ENXIO);
	}

	/* Assert reset PCIe */
	if (sc->reset_gpio != NULL) {
		if ((err = gpio_pin_set_active(sc->reset_gpio, false)))
			device_printf(dev, "reset_gpio set failed\n");
	}

	/* Start Link Training and Status State Machine (LTSSM) */
	/*
	 * Linux rockchip_pcie_enable_ltssm() only writes LTSSM_ENABLE here.
	 * Do not set LD_RQ_RST_GRT while releasing PERST, or LTSSM may stay
	 * in detect state 0 on RK3588.
	 */
	bus_write_4(sc->apb_res, PCIE_CLIENT_GENERAL_CON,
	    LTSSM_ENABLE << 16 | LTSSM_ENABLE);
	DELAY(100000);

	/* Release reset */
	if (sc->reset_gpio != NULL) {
		if ((err = gpio_pin_set_active(sc->reset_gpio, true)))
			device_printf(dev, "reset_gpio release failed\n");
	}

	/* Wait for link up/stable */
	if ((err = rk3588_pcie_wait_link(dev)) != 0)
		return (ENXIO);

	/* Delay to have things settle */
	DELAY(100000);

	if ((err = pci_dw_rk3588_post_link_setup(dev)) != 0) {
		device_printf(dev, "post-link setup failed: %d\n", err);
		return (ENXIO);
	}
	if ((err = pci_dw_rk3588_setup_inbound(dev)) != 0) {
		device_printf(dev, "inbound setup failed: %d\n", err);
		return (ENXIO);
	}

	/* Enable all MSG interrupts */
	bus_write_4(sc->apb_res, PCIE_CLIENT_INTR_MASK_MSG_RX, 0x7fff0000);

	/* Enable all Legacy interrupts */
	bus_write_4(sc->apb_res, PCIE_CLIENT_INTR_MASK_LEGACY, 0x00ff0000);

	/* Enable all Error interrupts */
	bus_write_4(sc->apb_res, PCIE_CLIENT_INTR_MASK_ERR, 0x0fff0000);

	rk3588_pcie_wait_endpoint_config(dev);

	if ((err = pci_dw_add_child(dev)))
		return (ENXIO);

	return (0);
}

static int
rk3588_pcie_detach(device_t dev)
{
	struct rk3588_pcie_softc *sc = device_get_softc(dev);

	/* Release allocated resources */
	if (sc->irq_handle)
		bus_teardown_intr(dev, sc->irq_res, sc->irq_handle);
	pci_dw_fini_core(dev);
	if (sc->phy)
		phy_release(sc->phy);
	if (sc->aux)
		clk_release(sc->aux);
	if (sc->pipe_clk)
		clk_release(sc->pipe_clk);
	if (sc->pclk)
		clk_release(sc->pclk);
	if (sc->aclk_dbi)
		clk_release(sc->aclk_dbi);
	if (sc->aclk_slv)
		clk_release(sc->aclk_slv);
	if (sc->aclk_mst)
		clk_release(sc->aclk_mst);
	if (sc->hwreset)
		hwreset_release(sc->hwreset);
	if (sc->pwr_reset)
		hwreset_release(sc->pwr_reset);
	if (sc->regulator)
		regulator_release(sc->regulator);
	if (sc->irq_res)
		bus_release_resource(dev, SYS_RES_IRQ, sc->irq_rid,
		    sc->irq_res);
	if (sc->dbi_res)
		bus_release_resource(dev, SYS_RES_MEMORY, sc->dbi_rid,
		    sc->dbi_res);
	if (sc->apb_res)
		bus_release_resource(dev, SYS_RES_MEMORY, sc->apb_rid,
		    sc->apb_res);
	return (0);
}

static int
rk3588_pcie_attach(device_t dev)
{
	struct rk3588_pcie_softc *sc = device_get_softc(dev);
	int error;

	sc->node = ofw_bus_get_node(dev);

	/* Setup resources */
	if ((error = ofw_bus_find_string_index(sc->node, "reg-names", "apb",
	    &sc->apb_rid))) {
		device_printf(dev, "Cannot get APB memory: %d\n", error);
		goto fail;
	}
	if (!(sc->apb_res = bus_alloc_resource_any(dev, SYS_RES_MEMORY,
	    &sc->apb_rid, RF_ACTIVE))) {
		device_printf(dev, "Cannot allocate APB resource\n");
		goto fail;
	}
	if ((error = ofw_bus_find_string_index(sc->node, "reg-names", "dbi",
	    &sc->dbi_rid))) {
		device_printf(dev, "Cannot get DBI memory: %d\n", error);
		goto fail;
	}
	if (!(sc->dw_sc.dbi_res = bus_alloc_resource_any(dev, SYS_RES_MEMORY,
	    &sc->dbi_rid, RF_ACTIVE))) {
		device_printf(dev, "Cannot allocate DBI resource\n");
		goto fail;
	}
	sc->dbi_res = sc->dw_sc.dbi_res;

	if (!(sc->irq_res = bus_alloc_resource_any(dev, SYS_RES_IRQ,
	    &sc->irq_rid, RF_ACTIVE | RF_SHAREABLE))) {
		device_printf(dev, "Cannot allocate IRQ resource\n");
		goto fail;
	}

	/* Get regulator if present */
	error = regulator_get_by_ofw_property(dev, 0, "vpcie3v3-supply",
	    &sc->regulator);
	if (error != 0) {
		sc->regulator = NULL;
		if (OF_hasprop(sc->node, "vpcie3v3-supply")) {
			device_printf(dev,
			    "vpcie3v3-supply unavailable: %d\n", error);
			if ((device_get_flags(dev) &
			    RK3588_PCIE_RETRY_SCHEDULED) == 0) {
				device_set_flags(dev, device_get_flags(dev) |
				    RK3588_PCIE_RETRY_SCHEDULED);
				config_intrhook_oneshot(rk3588_pcie_retry_attach,
				    dev);
			}
			goto fail;
		}
	}

	/* Get reset */
	if (ofw_bus_is_compatible(dev, "rockchip,rk3588-pcie")) {
		if (hwreset_get_by_ofw_name(dev, 0, "pwr", &sc->pwr_reset)) {
			device_printf(dev, "Can not get pwr reset\n");
			goto fail;
		}
	}
	if (hwreset_get_by_ofw_name(dev, 0, "pipe", &sc->hwreset)) {
		device_printf(dev, "Can not get reset\n");
		goto fail;
	}

	/* Get GPIO reset */
	if (OF_hasprop(sc->node, "reset-gpios")) {
		error = gpio_pin_get_by_ofw_property(dev, sc->node,
		    "reset-gpios", &sc->reset_gpio);
		if (error != 0) {
			sc->reset_gpio = NULL;
			device_printf(dev,
			    "optional reset-gpios unavailable: %d\n", error);
		} else {
			gpio_pin_setflags(sc->reset_gpio, GPIO_PIN_OUTPUT);
			gpio_pin_set_active(sc->reset_gpio, false);
		}
	}

	/* Get clocks */
	if (clk_get_by_ofw_name(dev, 0, "aclk_mst", &sc->aclk_mst)) {
		device_printf(dev, "Can not get aclk_mst clk\n");
		goto fail;
	}
	if (clk_get_by_ofw_name(dev, 0, "aclk_slv", &sc->aclk_slv)) {
		device_printf(dev, "Can not get aclk_slv clk\n");
		goto fail;
	}
	if (clk_get_by_ofw_name(dev, 0, "aclk_dbi", &sc->aclk_dbi)) {
		device_printf(dev, "Can not get aclk_dbi clk\n");
		goto fail;
	}
	if (clk_get_by_ofw_name(dev, 0, "pclk", &sc->pclk)) {
		device_printf(dev, "Can not get pclk clk\n");
		goto fail;
	}
	if (clk_get_by_ofw_name(dev, 0, "aux", &sc->aux)) {
		device_printf(dev, "Can not get aux clk\n");
		goto fail;
	}
	if (clk_get_by_ofw_name(dev, 0, "pipe", &sc->pipe_clk)) {
		if (ofw_bus_is_compatible(dev, "rockchip,rk3588-pcie")) {
			device_printf(dev, "Can not get pipe clk\n");
			goto fail;
		}
		device_printf(dev, "No pipe clk, continuing\n");
	}

	/* Get PHY */
	if (phy_get_by_ofw_name(dev, 0, "pcie-phy", &sc->phy)) {
		device_printf(dev, "Cannot get 'pcie-phy'\n");
		goto fail;
	}

	if ((error = rk3588_pcie_init_soc(dev)))
		goto fail;

	/* Enable interrupt */
	if ((bus_setup_intr(dev, sc->irq_res, INTR_TYPE_MISC | INTR_MPSAFE,
	    NULL, rk3588_intr, sc, &sc->irq_handle))) {
		device_printf(dev, "unable to setup interrupt\n");
		goto fail;
	}

	return (bus_generic_attach(dev));
fail:
	rk3588_pcie_detach(dev);
	return (ENXIO);
}

static int
rk3588_pcie_probe(device_t dev)
{

	if (!ofw_bus_status_okay(dev))
		return (ENXIO);
	if (!ofw_bus_search_compatible(dev, compat_data)->ocd_data)
		return (ENXIO);
	device_set_desc(dev, "RockChip RK3588 PCI-express controller");
	return (BUS_PROBE_DEFAULT);
}

static device_method_t rk3588_pcie_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,		rk3588_pcie_probe),
	DEVMETHOD(device_attach,	rk3588_pcie_attach),
	DEVMETHOD(device_detach,	rk3588_pcie_detach),

	/* PCI DW interface */
	DEVMETHOD(pci_dw_get_link,	rk3588_pcie_get_link),

	DEVMETHOD_END
};

DEFINE_CLASS_1(pcib, rk3588_pcie_driver, rk3588_pcie_methods,
    sizeof(struct rk3588_pcie_softc), pci_dw_driver);
DRIVER_MODULE(rk3588_pcie, simplebus, rk3588_pcie_driver, NULL, NULL);
