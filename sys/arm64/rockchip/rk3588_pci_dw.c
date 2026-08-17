/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/rman.h>
#include <sys/systm.h>

#include <machine/bus.h>

#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#include <dev/ofw/ofw_pci.h>
#include <dev/ofw/ofwpci.h>
#include <dev/pci/pcireg.h>
#include <dev/pci/pci_dw.h>
#include <dev/pci/pci_dw_soc.h>

#include <arm64/rockchip/rk3588_pci_dw.h>

#define	DBI_WR1(sc, reg, val)	pci_dw_dbi_wr1((sc)->dev, reg, val)
#define	DBI_WR2(sc, reg, val)	pci_dw_dbi_wr2((sc)->dev, reg, val)
#define	DBI_WR4(sc, reg, val)	pci_dw_dbi_wr4((sc)->dev, reg, val)
#define	DBI_RD1(sc, reg)	pci_dw_dbi_rd1((sc)->dev, reg)
#define	DBI_RD2(sc, reg)	pci_dw_dbi_rd2((sc)->dev, reg)
#define	DBI_RD4(sc, reg)	pci_dw_dbi_rd4((sc)->dev, reg)

#define	DW_DBI2_OFFSET		0x100000
#define	DBI2_WR4(sc, reg, val)	\
	    bus_write_4((sc)->dbi_res, DW_DBI2_OFFSET + (reg), (val))

#define	PCIE_LINK_CTL2_TARGET_SPEED	0x000f

#ifndef PCIEM_LINK_CAP_ASPM_L0S
#define	PCIEM_LINK_CAP_ASPM_L0S		0x00000400
#endif
#ifndef PCIZ_L1PM
#define	PCIZ_L1PM			0x001e
#endif
#ifndef PCIZ_VENDOR
#define	PCIZ_VENDOR			0x000b
#endif
#define	PCIER_L1PM_CAP			0x04
#define	 PCIEM_L1PM_CAP_PCIPM_L1_2	0x00000001
#define	 PCIEM_L1PM_CAP_PCIPM_L1_1	0x00000002
#define	 PCIEM_L1PM_CAP_ASPM_L1_2	0x00000004
#define	 PCIEM_L1PM_CAP_ASPM_L1_1	0x00000008
#define	 PCIEM_L1PM_CAP_L1_PM_SS	0x00000010

struct rk3588_pci_dw_data {
	bool	allow_dbi_ro;
	bool	allow_iatu;
};

static struct rk3588_pci_dw_data *
rk3588_pci_dw_data(struct pci_dw_softc *sc, bool alloc)
{
	struct rk3588_pci_dw_data *data;

	data = sc->soc_data;
	if (data == NULL && alloc) {
		data = malloc(sizeof(*data), M_DEVBUF, M_WAITOK | M_ZERO);
		sc->soc_data = data;
	}
	return (data);
}

static bool
rk3588_pci_dw_match(struct pci_dw_softc *sc)
{

	return (ofw_bus_node_is_compatible(sc->node, "rockchip,rk3588-pcie"));
}

static bool
rk3588_pci_dw_allow_dbi_ro(struct pci_dw_softc *sc)
{
	struct rk3588_pci_dw_data *data;

	data = rk3588_pci_dw_data(sc, false);
	return (data != NULL && data->allow_dbi_ro);
}

static bool
rk3588_pci_dw_allow_iatu(struct pci_dw_softc *sc)
{
	struct rk3588_pci_dw_data *data;

	data = rk3588_pci_dw_data(sc, false);
	return (data != NULL && data->allow_iatu);
}

static bool
rk3588_pci_dw_mem_range_prefetch(struct pci_dw_softc *sc,
    const struct ofw_pci_range *range)
{
	uint32_t space;

	space = range->pci_hi & OFW_PCI_PHYS_HI_SPACEMASK;
	return (space == OFW_PCI_PHYS_HI_SPACE_MEM64 ||
	    range->pci > UINT32_MAX);
}

static bool
rk3588_pci_dw_suppress_root_config(struct pci_dw_softc *sc)
{

	return (true);
}

static bool
rk3588_pci_dw_force_unroll(struct pci_dw_softc *sc, int atu_rv)
{

	return (true);
}

static bool
rk3588_pci_dw_force_atu_regions(struct pci_dw_softc *sc, bool unroll_mode,
    int atu_rid)
{

	return (unroll_mode && atu_rid < 0);
}

static bool
rk3588_pci_dw_prepare_dbi_protect(struct pci_dw_softc *sc,
    bool protect __unused)
{

	return (pci_dw_soc_allow_dbi_ro(sc));
}

static bool
rk3588_pci_dw_prepare_iatu_unroll(struct pci_dw_softc *sc)
{

	return (pci_dw_soc_allow_iatu(sc));
}

static bool
rk3588_pci_dw_has_dbi2(struct pci_dw_softc *sc)
{

	return (sc->dbi_res != NULL &&
	    rman_get_size(sc->dbi_res) >= DW_DBI2_OFFSET + PCIR_BAR(1) + 4);
}

static void
rk3588_pci_dw_root_config_done(struct pci_dw_softc *sc)
{

	DBI_WR4(sc, PCIR_BAR(0), 0);
	DBI_WR4(sc, PCIR_BAR(1), 0);

	if (!rk3588_pci_dw_has_dbi2(sc))
		return;

	DBI2_WR4(sc, PCIR_BAR(0), 0);
	DBI2_WR4(sc, PCIR_BAR(1), 0);
}

static int
rk3588_pci_dw_lane_field(int num_lanes)
{
	switch (num_lanes) {
	case 1:
		return (PORT_LINK_CAPABLE_1);
	case 2:
		return (PORT_LINK_CAPABLE_2);
	case 4:
		return (PORT_LINK_CAPABLE_4);
	case 8:
		return (PORT_LINK_CAPABLE_8);
	case 16:
		return (PORT_LINK_CAPABLE_16);
	case 32:
		return (PORT_LINK_CAPABLE_32);
	default:
		return (0);
	}
}

static int
rk3588_pci_dw_nfts_setup(struct pci_dw_softc *sc)
{
	uint32_t afr, gen2;

	afr = DBI_RD4(sc, DW_PORT_AFR);
	afr &= ~(PORT_AFR_N_FTS_MASK | PORT_AFR_CC_N_FTS_MASK);
	afr |= PORT_AFR_N_FTS(255) | PORT_AFR_CC_N_FTS(255);
	DBI_WR4(sc, DW_PORT_AFR, afr);

	gen2 = DBI_RD4(sc, DW_GEN2_CTRL);
	gen2 &= ~GEN2_CTRL_N_FTS_MASK;
	gen2 |= GEN2_CTRL_N_FTS(255);
	DBI_WR4(sc, DW_GEN2_CTRL, gen2);

	return (0);
}

static int
rk3588_pci_dw_link_width_setup(struct pci_dw_softc *sc)
{
	uint32_t gen2, plc;
	int lane_field;

	lane_field = rk3588_pci_dw_lane_field(sc->num_lanes);
	if (lane_field == 0) {
		device_printf(sc->dev,
		    "invalid number of lanes: %d\n",
		    sc->num_lanes);
		return (EINVAL);
	}

	/*
	 * Match Linux dw_pcie_link_set_max_link_width(): PORT_LINK_CTRL gets
	 * the real lane capability, while LINK_WIDTH_SPEED_CONTROL keeps the
	 * x1 value in its link-width field.  RK3588 still trains to x4 from
	 * PORT_LINK_CTRL; programming x4 here is not what the known-good
	 * Armbian path does.
	 */
	plc = DBI_RD4(sc, DW_PORT_LINK_CTRL);
	plc &= ~PORT_LINK_FAST_LINK_MODE;
	plc |= PORT_LINK_DLL_LINK_EN;
	plc &= ~PORT_LINK_CAPABLE(~0);
	plc |= PORT_LINK_CAPABLE(lane_field);
	DBI_WR4(sc, DW_PORT_LINK_CTRL, plc);

	gen2 = DBI_RD4(sc, DW_GEN2_CTRL);
	gen2 &= ~GEN2_CTRL_NUM_OF_LANES(~0);
	gen2 |= GEN2_CTRL_NUM_OF_LANES(GEN2_CTRL_NUM_OF_LANES_1);
	DBI_WR4(sc, DW_GEN2_CTRL, gen2);

	return (0);
}

static int
rk3588_pci_dw_pre_link_speed_setup(struct pci_dw_softc *sc)
{
	uint32_t gen2, lcap;
	uint16_t lctl2;
	uint8_t expcap;
	int rv, speed;

	rv = pci_dw_find_capability(sc, PCIY_EXPRESS, &expcap);
	if (rv != 0) {
		device_printf(sc->dev,
		    "cannot find PCIe capability: %d\n", rv);
		return (rv);
	}

	speed = sc->max_link_speed;
	if (speed <= 0 || speed > 4)
		speed = DBI_RD4(sc, expcap + PCIER_LINK_CAP) &
		    PCIEM_LINK_CAP_MAX_SPEED;
	if (speed <= 0 || speed > 4)
		speed = 1;

	lcap = DBI_RD4(sc, expcap + PCIER_LINK_CAP);
	lcap &= ~PCIEM_LINK_CAP_MAX_SPEED;
	lcap |= speed;
	lcap &= ~PCIEM_LINK_CAP_MAX_WIDTH;
	lcap |= (sc->num_lanes << 4) & PCIEM_LINK_CAP_MAX_WIDTH;
	DBI_WR4(sc, expcap + PCIER_LINK_CAP, lcap);

	lctl2 = DBI_RD2(sc, expcap + PCIER_LINK_CTL2);
	lctl2 &= ~PCIE_LINK_CTL2_TARGET_SPEED;
	lctl2 |= speed;
	DBI_WR2(sc, expcap + PCIER_LINK_CTL2, lctl2);

	gen2 = DBI_RD4(sc, DW_GEN2_CTRL);
	gen2 |= DIRECT_SPEED_CHANGE;
	DBI_WR4(sc, DW_GEN2_CTRL, gen2);

	return (0);
}

static int
rk3588_pci_dw_l0s_cap_setup(struct pci_dw_softc *sc)
{
	uint32_t lcap;
	uint8_t expcap;
	int force_l0s, rv;

	rv = pci_dw_find_capability(sc, PCIY_EXPRESS, &expcap);
	if (rv != 0) {
		device_printf(sc->dev,
		    "cannot find PCIe capability for L0s setup: %d\n", rv);
		return (rv);
	}

	force_l0s = 1;
	TUNABLE_INT_FETCH("hw.rk3588.pcie_force_l0s_cap", &force_l0s);
	lcap = DBI_RD4(sc, expcap + PCIER_LINK_CAP);
	if (force_l0s != 0 && (lcap & PCIEM_LINK_CAP_ASPM_L0S) == 0) {
		lcap |= PCIEM_LINK_CAP_ASPM_L0S;
		DBI_WR4(sc, expcap + PCIER_LINK_CAP, lcap);
	}
	return (0);
}

static int
rk3588_pci_dw_hide_l1ss(struct pci_dw_softc *sc)
{
	uint32_t l1ss_cap;
	uint16_t l1ss;
	int rv;

	rv = pci_dw_find_extcapability(sc, PCIZ_L1PM, &l1ss);
	if (rv != 0)
		return (0);

	l1ss_cap = DBI_RD4(sc, l1ss + PCIER_L1PM_CAP);
	l1ss_cap &= ~(PCIEM_L1PM_CAP_PCIPM_L1_1 |
	    PCIEM_L1PM_CAP_PCIPM_L1_2 | PCIEM_L1PM_CAP_ASPM_L1_1 |
	    PCIEM_L1PM_CAP_ASPM_L1_2 | PCIEM_L1PM_CAP_L1_PM_SS);
	DBI_WR4(sc, l1ss + PCIER_L1PM_CAP, l1ss_cap);
	return (0);
}

static int
rk3588_pci_dw_link_speed_setup(struct pci_dw_softc *sc)
{
	uint32_t gen2, lcap;
	uint16_t lctl, lctl2, lsta;
	uint8_t expcap;
	int lane_field, speed, i, rv;

	rv = pci_dw_find_capability(sc, PCIY_EXPRESS, &expcap);
	if (rv != 0) {
		device_printf(sc->dev,
		    "cannot find PCIe capability: %d\n", rv);
		return (rv);
	}

	speed = sc->max_link_speed;
	if (speed <= 0 || speed > 4)
		speed = DBI_RD4(sc, expcap + PCIER_LINK_CAP) &
		    PCIEM_LINK_CAP_MAX_SPEED;
	if (speed <= 0 || speed > 4)
		speed = 1;

	lane_field = rk3588_pci_dw_lane_field(sc->num_lanes);
	if (lane_field == 0) {
		device_printf(sc->dev,
		    "invalid number of lanes: %d\n",
		    sc->num_lanes);
		return (EINVAL);
	}

	lcap = DBI_RD4(sc, expcap + PCIER_LINK_CAP);
	lctl2 = DBI_RD2(sc, expcap + PCIER_LINK_CTL2);
	rv = rk3588_pci_dw_link_width_setup(sc);
	if (rv != 0)
		return (rv);

	gen2 = DBI_RD4(sc, DW_GEN2_CTRL);

	lcap &= ~PCIEM_LINK_CAP_MAX_SPEED;
	lcap |= speed;
	lcap &= ~PCIEM_LINK_CAP_MAX_WIDTH;
	lcap |= (sc->num_lanes << 4) & PCIEM_LINK_CAP_MAX_WIDTH;
	DBI_WR4(sc, expcap + PCIER_LINK_CAP, lcap);

	lctl2 &= ~PCIE_LINK_CTL2_TARGET_SPEED;
	lctl2 |= speed;
	DBI_WR2(sc, expcap + PCIER_LINK_CTL2, lctl2);

	gen2 = DBI_RD4(sc, DW_GEN2_CTRL);
	gen2 |= DIRECT_SPEED_CHANGE;
	DBI_WR4(sc, DW_GEN2_CTRL, gen2);

	lctl = DBI_RD2(sc, expcap + PCIER_LINK_CTL);
	lctl |= PCIEM_LINK_CTL_RETRAIN_LINK;
	DBI_WR2(sc, expcap + PCIER_LINK_CTL, lctl);

	for (i = 0; i < 100; i++) {
		DELAY(1000);
		lsta = DBI_RD2(sc, expcap + PCIER_LINK_STA);
		if ((lsta & PCIEM_LINK_STA_TRAINING) == 0)
			break;
	}

	return (0);
}

int
pci_dw_rk3588_setup_inbound(device_t dev)
{
	struct pci_dw_softc *sc;

	sc = device_get_softc(dev);
	if (!pci_dw_is_soc(dev))
		return (0);
	if (sc->iatu_ur_res == NULL)
		return (0);
	if (!pci_dw_soc_allow_iatu(sc))
		return (0);

	return (0);
}

int
pci_dw_rk3588_pre_link_setup(device_t dev)
{
	struct rk3588_pci_dw_data *data;
	struct pci_dw_softc *sc;

	sc = device_get_softc(dev);
	if (!pci_dw_is_soc(dev))
		return (0);

	data = rk3588_pci_dw_data(sc, true);
	data->allow_dbi_ro = true;
	pci_dw_dbi_protect(sc, false);
	rk3588_pci_dw_nfts_setup(sc);
	rk3588_pci_dw_link_width_setup(sc);
	rk3588_pci_dw_pre_link_speed_setup(sc);
	rk3588_pci_dw_l0s_cap_setup(sc);
	rk3588_pci_dw_hide_l1ss(sc);
	pci_dw_setup_root_config(sc);
	pci_dw_dbi_protect(sc, true);

	return (0);
}

int
pci_dw_rk3588_post_link_setup(device_t dev)
{
	struct rk3588_pci_dw_data *data;
	struct pci_dw_softc *sc;
	int postlink_retrain, rv;

	sc = device_get_softc(dev);
	if (!pci_dw_is_soc(dev))
		return (0);

	data = rk3588_pci_dw_data(sc, true);
	data->allow_dbi_ro = true;
	data->allow_iatu = true;

	pci_dw_disable_all_atu_unroll(sc);

	postlink_retrain = 0;
	TUNABLE_INT_FETCH("hw.rk3588.pcie_postlink_retrain",
	    &postlink_retrain);
	if (postlink_retrain != 0) {
		pci_dw_dbi_protect(sc, false);
		rk3588_pci_dw_link_speed_setup(sc);
		pci_dw_dbi_protect(sc, true);
	}

	rv = pci_dw_map_outbound_windows(sc);
	return (rv);
}

static void
rk3588_pci_dw_fini(struct pci_dw_softc *sc)
{

	if (sc->soc_data != NULL) {
		free(sc->soc_data, M_DEVBUF);
		sc->soc_data = NULL;
	}
}

static const struct pci_dw_soc_ops rk3588_pci_dw_soc_ops = {
	.match = rk3588_pci_dw_match,
	.allow_dbi_ro = rk3588_pci_dw_allow_dbi_ro,
	.allow_iatu = rk3588_pci_dw_allow_iatu,
	.mem_range_prefetch = rk3588_pci_dw_mem_range_prefetch,
	.suppress_root_config = rk3588_pci_dw_suppress_root_config,
	.force_unroll = rk3588_pci_dw_force_unroll,
	.force_atu_regions = rk3588_pci_dw_force_atu_regions,
	.prepare_dbi_protect = rk3588_pci_dw_prepare_dbi_protect,
	.prepare_iatu_unroll = rk3588_pci_dw_prepare_iatu_unroll,
	.root_config_done = rk3588_pci_dw_root_config_done,
	.fini = rk3588_pci_dw_fini,
};

PCI_DW_SOC_REGISTER(rk3588, rk3588_pci_dw_soc_ops);
