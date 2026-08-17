/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/rman.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

#include <machine/bus.h>

#include <dev/extres/clk/clk.h>
#include <dev/extres/hwreset/hwreset.h>
#include <dev/extres/phy/phy.h>
#include <dev/extres/syscon/syscon.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#include <dev/videomode/videomode.h>

#include <contrib/device-tree/include/dt-bindings/phy/phy.h>

#include "phynode_if.h"
#include "syscon_if.h"

#include "rk3588_hdmi_mode.h"

#define	RK3588_HDPTX0_BASE		0xfed60000u
#define	RK3588_HDPTX1_BASE		0xfed70000u
#define	RK3588_HDPTX_GRF_CON0		0x0000
#define	RK3588_HDPTX_GRF_STATUS		0x0080
#define	RK3588_HDPTX_LC_REF_CLK_SEL	(1u << 11)
#define	RK3588_HDPTX_I_PLL_EN		(1u << 7)
#define	RK3588_HDPTX_I_BIAS_EN		(1u << 6)
#define	RK3588_HDPTX_I_BGR_EN		(1u << 5)
#define	RK3588_HDPTX_O_PLL_LOCK_DONE	(1u << 3)
#define	RK3588_HDPTX_O_PHY_CLK_RDY	(1u << 2)
#define	RK3588_HDPTX_O_PHY_RDY		(1u << 1)
#define	RK3588_HDPTX_CMN_REG0030	(0x030 * 4)
#define	RK3588_HDPTX_CLOCK_READY	(RK3588_HDPTX_O_PLL_LOCK_DONE | \
	    RK3588_HDPTX_O_PHY_CLK_RDY)

struct rk3588_hdptx_reg_probe {
	const char	*name;
	bus_size_t	reg;
};

struct rk3588_hdptx_regval {
	bus_size_t	reg;
	uint32_t	val;
};

struct rk3588_hdptx_ropll_config {
	uint64_t	rate;
	uint8_t		mdiv;
	uint8_t		sdiv;
	uint8_t		sdm_deno;
	uint8_t		sdm_num_sign;
	uint8_t		sdm_num;
	uint8_t		sdc_n;
	uint8_t		sdc_num;
	uint8_t		sdc_deno;
};

static const struct rk3588_hdptx_ropll_config rk3588_hdptx_ropll[] = {
	{ 297000000, 124, 1, 62, 1, 16, 5, 0, 1 },
	{ 154000000, 193, 5, 193, 1, 32, 2, 1, 1 },
	{ 148500000, 0x7b, 3, 4, 0, 3, 5, 5, 0x10 },
};

static const struct rk3588_hdptx_ropll_config *
rk3588_hdptx_find_ropll(uint64_t rate)
{
	int i;

	for (i = 0; i < nitems(rk3588_hdptx_ropll); i++)
		if (rk3588_hdptx_ropll[i].rate == rate)
			return (&rk3588_hdptx_ropll[i]);
	return (NULL);
}

static const struct rk3588_hdptx_reg_probe rk3588_hdptx_cmn_probes[] = {
	{ "cmn_0008", 0x008 * 4 },
	{ "cmn_0051", 0x051 * 4 },
	{ "cmn_0055", 0x055 * 4 },
	{ "cmn_0059", 0x059 * 4 },
	{ "cmn_005a", 0x05a * 4 },
	{ "cmn_005e", 0x05e * 4 },
	{ "cmn_0060", 0x060 * 4 },
	{ "cmn_0064", 0x064 * 4 },
	{ "cmn_0065", 0x065 * 4 },
	{ "cmn_0069", 0x069 * 4 },
	{ "cmn_006c", 0x06c * 4 },
	{ "cmn_0070", 0x070 * 4 },
	{ "cmn_0086", 0x086 * 4 },
};

/* Values are also present in the BSD-2-Clause-Patent RK3588 EDK2 port. */
static const struct rk3588_hdptx_regval rk3588_hdptx_common_cmn[] = {
	{ 0x009 * 4, 0x0c }, { 0x00a * 4, 0x83 },
	{ 0x00b * 4, 0x06 }, { 0x00c * 4, 0x20 },
	{ 0x00d * 4, 0xb8 }, { 0x00e * 4, 0x0f },
	{ 0x00f * 4, 0x0f }, { 0x010 * 4, 0x04 },
	{ 0x011 * 4, 0x00 }, { 0x012 * 4, 0x26 },
	{ 0x013 * 4, 0x22 }, { 0x014 * 4, 0x24 },
	{ 0x015 * 4, 0x77 }, { 0x016 * 4, 0x08 },
	{ 0x017 * 4, 0x00 }, { 0x018 * 4, 0x04 },
	{ 0x019 * 4, 0x48 }, { 0x01a * 4, 0x01 },
	{ 0x01b * 4, 0x00 }, { 0x01c * 4, 0x01 },
	{ 0x01d * 4, 0x64 }, { 0x01f * 4, 0x00 },
	{ 0x026 * 4, 0x53 }, { 0x029 * 4, 0x01 },
	{ 0x030 * 4, 0x00 }, { 0x031 * 4, 0x20 },
	{ 0x032 * 4, 0x30 }, { 0x033 * 4, 0x0b },
	{ 0x034 * 4, 0x23 }, { 0x035 * 4, 0x00 },
	{ 0x038 * 4, 0x00 }, { 0x039 * 4, 0x00 },
	{ 0x03a * 4, 0x00 }, { 0x03b * 4, 0x00 },
	{ 0x03c * 4, 0x80 }, { 0x03e * 4, 0x0c },
	{ 0x03f * 4, 0x83 }, { 0x040 * 4, 0x06 },
	{ 0x041 * 4, 0x20 }, { 0x042 * 4, 0xb8 },
	{ 0x043 * 4, 0x00 }, { 0x044 * 4, 0x46 },
	{ 0x045 * 4, 0x24 }, { 0x047 * 4, 0x00 },
	{ 0x049 * 4, 0xfa }, { 0x04a * 4, 0x08 },
	{ 0x04b * 4, 0x00 }, { 0x04c * 4, 0x01 },
	{ 0x04d * 4, 0x64 }, { 0x04e * 4, 0x14 },
	{ 0x04f * 4, 0x00 }, { 0x050 * 4, 0x00 },
	{ 0x05d * 4, 0x0c }, { 0x05f * 4, 0x01 },
	{ 0x06b * 4, 0x04 }, { 0x073 * 4, 0x30 },
	{ 0x074 * 4, 0x00 }, { 0x075 * 4, 0x20 },
	{ 0x076 * 4, 0x30 }, { 0x077 * 4, 0x08 },
	{ 0x078 * 4, 0x0c }, { 0x079 * 4, 0x00 },
	{ 0x07b * 4, 0x00 }, { 0x07c * 4, 0x00 },
	{ 0x07d * 4, 0x00 }, { 0x07e * 4, 0x00 },
	{ 0x07f * 4, 0x00 }, { 0x080 * 4, 0x00 },
	{ 0x081 * 4, 0x09 }, { 0x082 * 4, 0x04 },
	{ 0x083 * 4, 0x24 }, { 0x084 * 4, 0x20 },
	{ 0x085 * 4, 0x03 }, { 0x086 * 4, 0x01 },
	{ 0x087 * 4, 0x0c }, { 0x08a * 4, 0x55 },
	{ 0x08b * 4, 0x25 }, { 0x08c * 4, 0x2c },
	{ 0x08d * 4, 0x22 }, { 0x08e * 4, 0x14 },
	{ 0x08f * 4, 0x20 }, { 0x090 * 4, 0x00 },
	{ 0x091 * 4, 0x00 }, { 0x092 * 4, 0x00 },
	{ 0x093 * 4, 0x00 }, { 0x09a * 4, 0x11 },
	{ 0x09b * 4, 0x10 },
};

static const struct rk3588_hdptx_regval rk3588_hdptx_tmds_cmn[] = {
	{ 0x008 * 4, 0x00 }, { 0x011 * 4, 0x01 },
	{ 0x017 * 4, 0x20 }, { 0x01e * 4, 0x14 },
	{ 0x020 * 4, 0x00 }, { 0x021 * 4, 0x00 },
	{ 0x022 * 4, 0x11 }, { 0x023 * 4, 0x00 },
	{ 0x024 * 4, 0x00 }, { 0x025 * 4, 0x53 },
	{ 0x026 * 4, 0x00 }, { 0x027 * 4, 0x00 },
	{ 0x028 * 4, 0x01 }, { 0x02a * 4, 0x00 },
	{ 0x02b * 4, 0x00 }, { 0x02c * 4, 0x00 },
	{ 0x02d * 4, 0x00 }, { 0x02e * 4, 0x04 },
	{ 0x02f * 4, 0x00 }, { 0x030 * 4, 0x20 },
	{ 0x031 * 4, 0x30 }, { 0x032 * 4, 0x0b },
	{ 0x033 * 4, 0x23 }, { 0x034 * 4, 0x00 },
	{ 0x03d * 4, 0x40 }, { 0x042 * 4, 0x78 },
	{ 0x046 * 4, 0xdd }, { 0x048 * 4, 0x11 },
	{ 0x04e * 4, 0x34 }, { 0x05c * 4, 0x25 },
	{ 0x05e * 4, 0x4f }, { 0x074 * 4, 0x04 },
	{ 0x081 * 4, 0x01 }, { 0x087 * 4, 0x04 },
	{ 0x089 * 4, 0x00 }, { 0x095 * 4, 0x00 },
	{ 0x097 * 4, 0x02 }, { 0x099 * 4, 0x04 },
	{ 0x09b * 4, 0x00 },
};

static const struct rk3588_hdptx_regval rk3588_hdptx_tmds_sb[] = {
	{ 0x114 * 4, 0x00 }, { 0x115 * 4, 0x00 },
	{ 0x116 * 4, 0x00 }, { 0x117 * 4, 0x00 },
};

/* All supported rates use Linux's <= 340 MHz clock path. */
static const struct rk3588_hdptx_regval rk3588_hdptx_tmds_lntop[] = {
	{ 0x200 * 4, 0x06 }, { 0x201 * 4, 0x07 },
	{ 0x202 * 4, 0xc1 }, { 0x203 * 4, 0xf0 },
	{ 0x204 * 4, 0x7c }, { 0x205 * 4, 0x1f },
	{ 0x206 * 4, 0x07 },
};

static const struct rk3588_hdptx_regval rk3588_hdptx_lane_prefix[] = {
	{ 0x03 * 4, 0x0c }, { 0x07 * 4, 0x20 },
	{ 0x0a * 4, 0x17 }, { 0x0b * 4, 0x77 },
	{ 0x0c * 4, 0x77 }, { 0x0d * 4, 0x77 },
	{ 0x0e * 4, 0x38 }, { 0x10 * 4, 0x03 },
	{ 0x11 * 4, 0x0f }, { 0x12 * 4, 0x00 },
	{ 0x16 * 4, 0x02 }, { 0x1b * 4, 0x01 },
};

static const struct rk3588_hdptx_regval rk3588_hdptx_lane_tail[] = {
	{ 0x1f * 4, 0x15 }, { 0x20 * 4, 0xa0 },
};

/* Unique final values, excluding the lane-specific 0x1e skew setting. */
static const struct rk3588_hdptx_regval rk3588_hdptx_lane_final[] = {
	{ 0x03 * 4, 0x2f }, { 0x05 * 4, 0x03 },
	{ 0x06 * 4, 0x1c }, { 0x07 * 4, 0x20 },
	{ 0x0a * 4, 0x17 }, { 0x0b * 4, 0x77 },
	{ 0x0c * 4, 0x77 }, { 0x0d * 4, 0x77 },
	{ 0x0e * 4, 0x38 }, { 0x10 * 4, 0x03 },
	{ 0x11 * 4, 0x0f }, { 0x12 * 4, 0x00 },
	{ 0x16 * 4, 0x02 }, { 0x1b * 4, 0x01 },
	{ 0x1f * 4, 0x15 }, { 0x20 * 4, 0xa0 },
};

static const char * const rk3588_hdptx_clock_names[] = {
	"ref", "apb"
};

static const char * const rk3588_hdptx_pixel_clock_names[] = {
	"clk_hdmiphy_pixel0", "clk_hdmiphy_pixel1"
};

/* Linux probe deasserts these three in this exact order and leaves lane alone. */
static const char * const rk3588_hdptx_reset_names[] = {
	"apb", "cmn", "init", "lane"
};

struct rk3588_hdptx_phy_softc {
	device_t	dev;
	phandle_t	node;
	struct syscon	*grf;
	struct phynode	*phynode;
	struct clkdom	*pixel_clkdom;
	struct resource	*mem;
	clk_t		clocks[nitems(rk3588_hdptx_clock_names)];
	hwreset_t	resets[nitems(rk3588_hdptx_reset_names)];
	uint64_t	clock_rate[nitems(rk3588_hdptx_clock_names)];
	uint64_t	base;
	uint32_t	clock_mask;
	uint32_t	clock_enable_mask;
	uint32_t	cmn_snapshot[nitems(rk3588_hdptx_cmn_probes)];
	uint32_t	pll_cmn_snapshot[nitems(rk3588_hdptx_cmn_probes)];
	uint32_t	reset_get_mask;
	uint32_t	reset_before_asserted;
	uint32_t	reset_deassert_mask;
	uint32_t	reset_after_asserted;
	uint32_t	grf_status_before;
	uint32_t	grf_status_after;
	uint32_t	pll_grf_status_before;
	uint32_t	pll_grf_status_after;
	uint32_t	pll_grf_con0_before;
	uint32_t	pll_grf_con0_after;
	uint32_t	pll_grf_con0_final;
	uint32_t	pll_reset_asserted;
	uint32_t	pll_write_count;
	uint32_t	pll_verify_count;
	uint32_t	pll_verify_errors;
	uint32_t	pll_advisory_mismatches;
	uint32_t	pll_reg0030_expected;
	uint32_t	pll_reg0030_actual;
	uint32_t	pll_first_bad_reg;
	uint32_t	pll_first_bad_expected;
	uint32_t	pll_first_bad_actual;
	uint32_t	pll_poll_count;
	uint32_t	pll_ready;
	uint32_t	pll_attempted;
	int		pll_error;
	uint32_t	lane_grf_status_before;
	uint32_t	lane_grf_status_after;
	uint32_t	lane_grf_con0_final;
	uint32_t	lane_write_count;
	uint32_t	lane_verify_count;
	uint32_t	lane_verify_errors;
	uint32_t	lane_first_bad_reg;
	uint32_t	lane_first_bad_expected;
	uint32_t	lane_first_bad_actual;
	uint32_t	lane_poll_count;
	uint32_t	lane_ready;
	uint32_t	lane_attempted;
	uint32_t	lane_reset_before_asserted;
	uint32_t	lane_reset_after_asserted;
	uint32_t	lane_lntop_0_3;
	uint32_t	lane_lntop_4_7;
	uint32_t	lane_tx_ctrl;
	uint32_t	lane_skew;
	uint32_t	lane_drive_05;
	uint32_t	lane_drive_06;
	uint64_t	pixel_clock_rate;
	uint64_t	tmds_rate;
	uint32_t	pixel_clock_registered;
	const char	*pixel_clock_parent;
	int		lane_error;
	int		port;
};

static struct rk3588_hdptx_phy_softc *rk3588_hdptx_sc[2];

struct rk3588_hdptx_clk_softc {
	struct rk3588_hdptx_phy_softc *phy;
};

static bool
rk3588_hdptx_clk_ready(struct rk3588_hdptx_phy_softc *sc)
{
	uint32_t status;

	if (sc->port != 0 || sc->pll_error != 0)
		return (false);
	status = SYSCON_READ_4(sc->grf, RK3588_HDPTX_GRF_STATUS);
	return ((status & RK3588_HDPTX_CLOCK_READY) ==
	    RK3588_HDPTX_CLOCK_READY);
}

static int
rk3588_hdptx_clk_init(struct clknode *clk, device_t dev)
{

	(void)dev;
	clknode_init_parent_idx(clk, 0);
	return (0);
}

static int
rk3588_hdptx_clk_recalc(struct clknode *clk, uint64_t *freq)
{
	struct rk3588_hdptx_clk_softc *clk_sc;

	clk_sc = clknode_get_softc(clk);
	*freq = rk3588_hdptx_clk_ready(clk_sc->phy) ?
	    clk_sc->phy->tmds_rate : 0;
	return (0);
}

static int
rk3588_hdptx_clk_set_freq(struct clknode *clk, uint64_t fin, uint64_t *fout,
    int flags, int *done)
{
	struct rk3588_hdptx_clk_softc *clk_sc;

	(void)fin;
	(void)flags;
	clk_sc = clknode_get_softc(clk);
	*done = 1;
	if (*fout != clk_sc->phy->tmds_rate)
		return (ERANGE);
	if (!rk3588_hdptx_clk_ready(clk_sc->phy))
		return (EIO);
	*fout = clk_sc->phy->tmds_rate;
	return (0);
}

static int
rk3588_hdptx_clk_set_gate(struct clknode *clk, bool enable)
{
	struct rk3588_hdptx_clk_softc *clk_sc;

	clk_sc = clknode_get_softc(clk);
	if (enable && !rk3588_hdptx_clk_ready(clk_sc->phy))
		return (EIO);
	return (0);
}

static clknode_method_t rk3588_hdptx_clk_methods[] = {
	CLKNODEMETHOD(clknode_init,		rk3588_hdptx_clk_init),
	CLKNODEMETHOD(clknode_recalc_freq,	rk3588_hdptx_clk_recalc),
	CLKNODEMETHOD(clknode_set_freq,	rk3588_hdptx_clk_set_freq),
	CLKNODEMETHOD(clknode_set_gate,	rk3588_hdptx_clk_set_gate),
	CLKNODEMETHOD_END
};

DEFINE_CLASS_1(rk3588_hdptx_clk, rk3588_hdptx_clk_class,
    rk3588_hdptx_clk_methods, sizeof(struct rk3588_hdptx_clk_softc),
    clknode_class);

static int
rk3588_hdptx_clk_ofw_map(struct clkdom *clkdom, uint32_t ncells,
    phandle_t *cells, struct clknode **clk)
{

	if (ncells != 0)
		return (ERANGE);
	(void)cells;
	*clk = clknode_find_by_id(clkdom, 0);
	return (*clk == NULL ? ENXIO : 0);
}

static int
rk3588_hdptx_register_pixel_clock(struct rk3588_hdptx_phy_softc *sc)
{
	struct clknode_init_def def;
	struct rk3588_hdptx_clk_softc *clk_sc;
	struct clknode *clk;

	sc->pixel_clock_parent = clk_get_name(sc->clocks[0]);
	sc->pixel_clkdom = clkdom_create(sc->dev);
	if (sc->pixel_clkdom == NULL)
		return (ENOMEM);
	clkdom_set_ofw_mapper(sc->pixel_clkdom, rk3588_hdptx_clk_ofw_map);

	bzero(&def, sizeof(def));
	def.id = 0;
	def.name = rk3588_hdptx_pixel_clock_names[sc->port];
	def.parent_names = &sc->pixel_clock_parent;
	def.parent_cnt = 1;
	clk = clknode_create(sc->pixel_clkdom, &rk3588_hdptx_clk_class, &def);
	if (clk == NULL)
		return (ENXIO);
	clk_sc = clknode_get_softc(clk);
	clk_sc->phy = sc;
	clknode_register(sc->pixel_clkdom, clk);
	if (clkdom_finit(sc->pixel_clkdom) != 0)
		return (ENXIO);

	sc->pixel_clock_registered = 1;
	rk3588_hdptx_clk_recalc(clk, &sc->pixel_clock_rate);
	return (0);
}

static int
rk3588_hdptx_phy_enable(struct phynode *phynode, bool enable)
{

	/* Do not report success until the HDMI-QP consumer pipeline is implemented. */
	return (enable ? EOPNOTSUPP : 0);
}

static int
rk3588_hdptx_phy_status(struct phynode *phynode, int *status)
{

	*status = 0;
	return (0);
}

static phynode_method_t rk3588_hdptx_phynode_methods[] = {
	PHYNODEMETHOD(phynode_enable,	rk3588_hdptx_phy_enable),
	PHYNODEMETHOD(phynode_status,	rk3588_hdptx_phy_status),

	PHYNODEMETHOD_END
};
DEFINE_CLASS_1(rk3588_hdptx_phynode, rk3588_hdptx_phynode_class,
    rk3588_hdptx_phynode_methods, 0, phynode_class);

static int
rk3588_hdptx_live_status_sysctl(SYSCTL_HANDLER_ARGS)
{
	struct rk3588_hdptx_phy_softc *sc;
	uint32_t value;

	sc = arg1;
	value = SYSCON_READ_4(sc->grf, RK3588_HDPTX_GRF_STATUS);
	return (sysctl_handle_32(oidp, &value, 0, req));
}

static void
rk3588_hdptx_add_sysctls(struct rk3588_hdptx_phy_softc *sc)
{
	struct sysctl_ctx_list *ctx;
	struct sysctl_oid *lane;
	struct sysctl_oid *pll;
	struct sysctl_oid_list *children;
	struct sysctl_oid_list *lane_children;
	struct sysctl_oid_list *pll_children;
	int i;

	ctx = device_get_sysctl_ctx(sc->dev);
	children = SYSCTL_CHILDREN(device_get_sysctl_tree(sc->dev));
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "port", CTLFLAG_RD,
	    &sc->port, 0, "RK3588 HDPTX PHY port index");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "clock_mask", CTLFLAG_RD,
	    &sc->clock_mask, 0, "Resolved ref/apb clocks");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "clock_enable_mask",
	    CTLFLAG_RD, &sc->clock_enable_mask, 0, "Enabled ref/apb clocks");
	SYSCTL_ADD_U64(ctx, children, OID_AUTO, "mmio_base", CTLFLAG_RD,
	    &sc->base, 0, "HDPTX PHY MMIO base");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "pixel_clock_registered",
	    CTLFLAG_RD, &sc->pixel_clock_registered, 0,
	    "HDPTX pixel-clock provider registered");
	SYSCTL_ADD_U64(ctx, children, OID_AUTO, "pixel_clock_rate",
	    CTLFLAG_RD, &sc->pixel_clock_rate, 0,
	    "Validated HDPTX pixel-clock rate");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "reset_get_mask", CTLFLAG_RD,
	    &sc->reset_get_mask, 0, "Resolved apb/cmn/init/lane resets");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "reset_before_asserted",
	    CTLFLAG_RD, &sc->reset_before_asserted, 0,
	    "Asserted resets before Linux probe sequence");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "reset_deassert_mask",
	    CTLFLAG_RD, &sc->reset_deassert_mask, 0,
	    "Successfully deasserted apb/cmn/init resets");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "reset_after_asserted",
	    CTLFLAG_RD, &sc->reset_after_asserted, 0,
	    "Asserted resets after Linux probe sequence");
	SYSCTL_ADD_PROC(ctx, children, OID_AUTO, "grf_status",
	    CTLTYPE_UINT | CTLFLAG_RD, sc, 0,
	    rk3588_hdptx_live_status_sysctl, "IU",
	    "Live HDPTX GRF readiness status");
	for (i = 0; i < nitems(rk3588_hdptx_clock_names); i++)
		SYSCTL_ADD_U64(ctx, children, OID_AUTO,
		    rk3588_hdptx_clock_names[i], CTLFLAG_RD,
		    &sc->clock_rate[i], 0, "Resolved clock frequency in Hz");
	for (i = 0; i < nitems(rk3588_hdptx_cmn_probes); i++)
		SYSCTL_ADD_U32(ctx, children, OID_AUTO,
		    rk3588_hdptx_cmn_probes[i].name, CTLFLAG_RD,
		    &sc->cmn_snapshot[i], 0,
		    "Read-only HDPTX common PLL register snapshot");

	pll = SYSCTL_ADD_NODE(ctx, children, OID_AUTO, "pll", CTLFLAG_RD,
	    NULL, "Selected TMDS common PLL diagnostic");
	pll_children = SYSCTL_CHILDREN(pll);
	SYSCTL_ADD_U64(ctx, pll_children, OID_AUTO, "rate", CTLFLAG_RD,
	    &sc->tmds_rate, 0, "Selected TMDS character rate");
	SYSCTL_ADD_U32(ctx, pll_children, OID_AUTO, "attempted", CTLFLAG_RD,
	    &sc->pll_attempted, 0, "HDPTX0 PLL programming was attempted");
	SYSCTL_ADD_INT(ctx, pll_children, OID_AUTO, "error", CTLFLAG_RD,
	    &sc->pll_error, 0, "PLL programming result");
	SYSCTL_ADD_U32(ctx, pll_children, OID_AUTO, "ready", CTLFLAG_RD,
	    &sc->pll_ready, 0, "GRF PHY_CLK_RDY observed");
	SYSCTL_ADD_U32(ctx, pll_children, OID_AUTO, "write_count", CTLFLAG_RD,
	    &sc->pll_write_count, 0, "CMN register writes performed");
	SYSCTL_ADD_U32(ctx, pll_children, OID_AUTO, "verify_count", CTLFLAG_RD,
	    &sc->pll_verify_count, 0, "CMN register readbacks checked");
	SYSCTL_ADD_U32(ctx, pll_children, OID_AUTO, "verify_errors", CTLFLAG_RD,
	    &sc->pll_verify_errors, 0, "CMN register readback mismatches");
	SYSCTL_ADD_U32(ctx, pll_children, OID_AUTO, "advisory_mismatches",
	    CTLFLAG_RD, &sc->pll_advisory_mismatches, 0,
	    "Non-fatal reference-driver readback mismatches");
	SYSCTL_ADD_U32(ctx, pll_children, OID_AUTO, "reg0030_expected",
	    CTLFLAG_RD, &sc->pll_reg0030_expected, 0,
	    "CMN_REG0030 value written by the reference sequence");
	SYSCTL_ADD_U32(ctx, pll_children, OID_AUTO, "reg0030_actual",
	    CTLFLAG_RD, &sc->pll_reg0030_actual, 0,
	    "CMN_REG0030 value read after the reference sequence");
	SYSCTL_ADD_U32(ctx, pll_children, OID_AUTO, "first_bad_reg", CTLFLAG_RD,
	    &sc->pll_first_bad_reg, 0, "First mismatched CMN byte index");
	SYSCTL_ADD_U32(ctx, pll_children, OID_AUTO, "first_bad_expected",
	    CTLFLAG_RD, &sc->pll_first_bad_expected, 0,
	    "Expected value for first CMN mismatch");
	SYSCTL_ADD_U32(ctx, pll_children, OID_AUTO, "first_bad_actual",
	    CTLFLAG_RD, &sc->pll_first_bad_actual, 0,
	    "Actual value for first CMN mismatch");
	SYSCTL_ADD_U32(ctx, pll_children, OID_AUTO, "poll_count", CTLFLAG_RD,
	    &sc->pll_poll_count, 0, "GRF PHY_CLK_RDY poll reads");
	SYSCTL_ADD_U32(ctx, pll_children, OID_AUTO, "grf_before", CTLFLAG_RD,
	    &sc->pll_grf_status_before, 0, "GRF status before PLL sequence");
	SYSCTL_ADD_U32(ctx, pll_children, OID_AUTO, "grf_after", CTLFLAG_RD,
	    &sc->pll_grf_status_after, 0, "GRF status after PLL sequence");
	SYSCTL_ADD_U32(ctx, pll_children, OID_AUTO, "con0_before", CTLFLAG_RD,
	    &sc->pll_grf_con0_before, 0, "GRF CON0 before ref-clock select");
	SYSCTL_ADD_U32(ctx, pll_children, OID_AUTO, "con0_after", CTLFLAG_RD,
	    &sc->pll_grf_con0_after, 0, "GRF CON0 after ref-clock select");
	SYSCTL_ADD_U32(ctx, pll_children, OID_AUTO, "con0_final", CTLFLAG_RD,
	    &sc->pll_grf_con0_final, 0, "GRF CON0 after PLL sequence");
	SYSCTL_ADD_U32(ctx, pll_children, OID_AUTO, "reset_asserted",
	    CTLFLAG_RD, &sc->pll_reset_asserted, 0,
	    "Asserted apb/cmn/init/lane resets after PLL sequence");
	for (i = 0; i < nitems(rk3588_hdptx_cmn_probes); i++)
		SYSCTL_ADD_U32(ctx, pll_children, OID_AUTO,
		    rk3588_hdptx_cmn_probes[i].name, CTLFLAG_RD,
		    &sc->pll_cmn_snapshot[i], 0,
		    "CMN register snapshot after PLL programming");

	lane = SYSCTL_ADD_NODE(ctx, children, OID_AUTO, "lane", CTLFLAG_RD,
	    NULL, "Selected TMDS lane diagnostic");
	lane_children = SYSCTL_CHILDREN(lane);
	SYSCTL_ADD_U32(ctx, lane_children, OID_AUTO, "attempted", CTLFLAG_RD,
	    &sc->lane_attempted, 0, "HDPTX0 lane programming was attempted");
	SYSCTL_ADD_INT(ctx, lane_children, OID_AUTO, "error", CTLFLAG_RD,
	    &sc->lane_error, 0, "Lane programming result");
	SYSCTL_ADD_U32(ctx, lane_children, OID_AUTO, "ready", CTLFLAG_RD,
	    &sc->lane_ready, 0, "GRF PHY_RDY and PLL_LOCK_DONE observed");
	SYSCTL_ADD_U32(ctx, lane_children, OID_AUTO, "write_count", CTLFLAG_RD,
	    &sc->lane_write_count, 0, "Lane register writes performed");
	SYSCTL_ADD_U32(ctx, lane_children, OID_AUTO, "verify_count", CTLFLAG_RD,
	    &sc->lane_verify_count, 0, "Lane register readbacks checked");
	SYSCTL_ADD_U32(ctx, lane_children, OID_AUTO, "verify_errors", CTLFLAG_RD,
	    &sc->lane_verify_errors, 0, "Lane register readback mismatches");
	SYSCTL_ADD_U32(ctx, lane_children, OID_AUTO, "first_bad_reg", CTLFLAG_RD,
	    &sc->lane_first_bad_reg, 0, "First mismatched lane byte index");
	SYSCTL_ADD_U32(ctx, lane_children, OID_AUTO, "first_bad_expected",
	    CTLFLAG_RD, &sc->lane_first_bad_expected, 0,
	    "Expected value for first lane mismatch");
	SYSCTL_ADD_U32(ctx, lane_children, OID_AUTO, "first_bad_actual",
	    CTLFLAG_RD, &sc->lane_first_bad_actual, 0,
	    "Actual value for first lane mismatch");
	SYSCTL_ADD_U32(ctx, lane_children, OID_AUTO, "poll_count", CTLFLAG_RD,
	    &sc->lane_poll_count, 0, "GRF lane readiness poll reads");
	SYSCTL_ADD_U32(ctx, lane_children, OID_AUTO, "grf_before", CTLFLAG_RD,
	    &sc->lane_grf_status_before, 0, "GRF status before lane sequence");
	SYSCTL_ADD_U32(ctx, lane_children, OID_AUTO, "grf_after", CTLFLAG_RD,
	    &sc->lane_grf_status_after, 0, "GRF status after lane sequence");
	SYSCTL_ADD_U32(ctx, lane_children, OID_AUTO, "con0_final", CTLFLAG_RD,
	    &sc->lane_grf_con0_final, 0, "GRF CON0 after lane sequence");
	SYSCTL_ADD_U32(ctx, lane_children, OID_AUTO, "reset_before", CTLFLAG_RD,
	    &sc->lane_reset_before_asserted, 0,
	    "Lane reset asserted before programming");
	SYSCTL_ADD_U32(ctx, lane_children, OID_AUTO, "reset_after", CTLFLAG_RD,
	    &sc->lane_reset_after_asserted, 0,
	    "Lane reset asserted after programming");
	SYSCTL_ADD_U32(ctx, lane_children, OID_AUTO, "lntop_0_3", CTLFLAG_RD,
	    &sc->lane_lntop_0_3, 0, "Packed LNTOP_REG0200 through 0203");
	SYSCTL_ADD_U32(ctx, lane_children, OID_AUTO, "lntop_4_7", CTLFLAG_RD,
	    &sc->lane_lntop_4_7, 0, "Packed LNTOP_REG0204 through 0207");
	SYSCTL_ADD_U32(ctx, lane_children, OID_AUTO, "tx_ctrl", CTLFLAG_RD,
	    &sc->lane_tx_ctrl, 0, "Packed lane TX control values");
	SYSCTL_ADD_U32(ctx, lane_children, OID_AUTO, "skew", CTLFLAG_RD,
	    &sc->lane_skew, 0, "Packed Linux inter-pair skew values");
	SYSCTL_ADD_U32(ctx, lane_children, OID_AUTO, "drive_05", CTLFLAG_RD,
	    &sc->lane_drive_05, 0, "Packed lane register 05 values");
	SYSCTL_ADD_U32(ctx, lane_children, OID_AUTO, "drive_06", CTLFLAG_RD,
	    &sc->lane_drive_06, 0, "Packed lane register 06 values");
}

static int
rk3588_hdptx_get_clocks(struct rk3588_hdptx_phy_softc *sc)
{
	int error, i;

	for (i = 0; i < nitems(rk3588_hdptx_clock_names); i++) {
		error = clk_get_by_ofw_name(sc->dev, sc->node,
		    rk3588_hdptx_clock_names[i], &sc->clocks[i]);
		if (error != 0) {
			device_printf(sc->dev, "Cannot get %s clock: %d\n",
			    rk3588_hdptx_clock_names[i], error);
			return (error);
		}
		sc->clock_mask |= 1u << i;
		error = clk_get_freq(sc->clocks[i], &sc->clock_rate[i]);
		if (error != 0) {
			device_printf(sc->dev, "Cannot read %s clock rate: %d\n",
			    rk3588_hdptx_clock_names[i], error);
			return (error);
		}
	}
	return (0);
}

static void
rk3588_hdptx_disable_clocks(struct rk3588_hdptx_phy_softc *sc)
{
	int i;

	for (i = nitems(rk3588_hdptx_clock_names) - 1; i >= 0; i--) {
		if ((sc->clock_enable_mask & (1u << i)) == 0)
			continue;
		clk_disable(sc->clocks[i]);
		sc->clock_enable_mask &= ~(1u << i);
	}
}

static int
rk3588_hdptx_enable_clocks(struct rk3588_hdptx_phy_softc *sc)
{
	int error, i;

	for (i = 0; i < nitems(rk3588_hdptx_clock_names); i++) {
		error = clk_enable(sc->clocks[i]);
		if (error != 0) {
			device_printf(sc->dev, "Cannot enable %s clock: %d\n",
			    rk3588_hdptx_clock_names[i], error);
			rk3588_hdptx_disable_clocks(sc);
			return (error);
		}
		sc->clock_enable_mask |= 1u << i;
	}
	return (0);
}

static void
rk3588_hdptx_snapshot_cmn(struct rk3588_hdptx_phy_softc *sc,
    uint32_t *snapshot)
{
	int i;

	for (i = 0; i < nitems(rk3588_hdptx_cmn_probes); i++)
		snapshot[i] = bus_read_4(sc->mem,
		    rk3588_hdptx_cmn_probes[i].reg);
}

static void
rk3588_hdptx_verify_reg(struct rk3588_hdptx_phy_softc *sc,
    bus_size_t reg, uint32_t expected, uint32_t mask)
{
	uint32_t actual;

	actual = bus_read_4(sc->mem, reg) & mask;
	expected &= mask;
	sc->pll_verify_count++;
	if (actual == expected)
		return;
	if (sc->pll_verify_errors == 0) {
		sc->pll_first_bad_reg = reg / 4;
		sc->pll_first_bad_expected = expected;
		sc->pll_first_bad_actual = actual;
	}
	sc->pll_verify_errors++;
}

static void
rk3588_hdptx_write_sequence(struct rk3588_hdptx_phy_softc *sc,
    const struct rk3588_hdptx_regval *seq, int count)
{
	int i;

	for (i = 0; i < count; i++) {
		bus_write_4(sc->mem, seq[i].reg, seq[i].val);
		sc->pll_write_count++;
	}
	bus_barrier(sc->mem, 0, rman_get_size(sc->mem),
	    BUS_SPACE_BARRIER_WRITE);
	for (i = 0; i < count; i++) {
		/* Linux and EDK2 validate this write through PLL readiness. */
		if (seq[i].reg == RK3588_HDPTX_CMN_REG0030 &&
		    seq[i].val == 0x20) {
			sc->pll_reg0030_expected = seq[i].val;
			sc->pll_reg0030_actual = bus_read_4(sc->mem,
			    seq[i].reg) & 0xff;
			sc->pll_verify_count++;
			if (sc->pll_reg0030_actual != seq[i].val)
				sc->pll_advisory_mismatches++;
			continue;
		}
		rk3588_hdptx_verify_reg(sc, seq[i].reg, seq[i].val, 0xff);
	}
}

static void
rk3588_hdptx_write_reg(struct rk3588_hdptx_phy_softc *sc,
    bus_size_t reg, uint32_t val)
{

	bus_write_4(sc->mem, reg, val);
	sc->pll_write_count++;
}

static void
rk3588_hdptx_update_reg(struct rk3588_hdptx_phy_softc *sc,
    bus_size_t reg, uint32_t mask, uint32_t val)
{
	uint32_t old;

	old = bus_read_4(sc->mem, reg);
	bus_write_4(sc->mem, reg, (old & ~mask) | (val & mask));
	sc->pll_write_count++;
}

static void
rk3588_hdptx_lane_write(struct rk3588_hdptx_phy_softc *sc,
    bus_size_t reg, uint32_t val)
{

	bus_write_4(sc->mem, reg, val);
	sc->lane_write_count++;
}

static void
rk3588_hdptx_lane_write_sequence(struct rk3588_hdptx_phy_softc *sc,
    bus_size_t base, const struct rk3588_hdptx_regval *seq, int count)
{
	int i;

	for (i = 0; i < count; i++)
		rk3588_hdptx_lane_write(sc, base + seq[i].reg, seq[i].val);
}

static void
rk3588_hdptx_lane_verify_reg(struct rk3588_hdptx_phy_softc *sc,
    bus_size_t reg, uint32_t expected)
{
	uint32_t actual;

	actual = bus_read_4(sc->mem, reg) & 0xff;
	sc->lane_verify_count++;
	if (actual == expected)
		return;
	if (sc->lane_verify_errors == 0) {
		sc->lane_first_bad_reg = reg / 4;
		sc->lane_first_bad_expected = expected;
		sc->lane_first_bad_actual = actual;
	}
	sc->lane_verify_errors++;
}

static void
rk3588_hdptx_verify_lane_table(struct rk3588_hdptx_phy_softc *sc,
    bool enabled)
{
	bus_size_t base;
	uint32_t skew;
	int i, lane;

	bus_barrier(sc->mem, 0, rman_get_size(sc->mem),
	    BUS_SPACE_BARRIER_READ | BUS_SPACE_BARRIER_WRITE);
	for (i = 0; i < nitems(rk3588_hdptx_tmds_sb); i++)
		rk3588_hdptx_lane_verify_reg(sc, rk3588_hdptx_tmds_sb[i].reg,
		    rk3588_hdptx_tmds_sb[i].val);
	for (i = 0; i < nitems(rk3588_hdptx_tmds_lntop); i++)
		rk3588_hdptx_lane_verify_reg(sc,
		    rk3588_hdptx_tmds_lntop[i].reg,
		    rk3588_hdptx_tmds_lntop[i].val);
	if (enabled)
		rk3588_hdptx_lane_verify_reg(sc, 0x207 * 4, 0x0f);
	for (lane = 3; lane <= 6; lane++) {
		base = (lane << 8) * 4;
		for (i = 0; i < nitems(rk3588_hdptx_lane_final); i++)
			rk3588_hdptx_lane_verify_reg(sc,
			    base + rk3588_hdptx_lane_final[i].reg,
			    rk3588_hdptx_lane_final[i].val);
		skew = lane == 6 ? 0x0a : 0x02;
		rk3588_hdptx_lane_verify_reg(sc, base + 0x1e * 4, skew);
	}
}

static uint32_t
rk3588_hdptx_pack_lntop(struct rk3588_hdptx_phy_softc *sc, int first)
{
	uint32_t value;
	int i;

	value = 0;
	for (i = 0; i < 4; i++)
		value |= (bus_read_4(sc->mem, (first + i) * 4) & 0xff) <<
		    (i * 8);
	return (value);
}

static uint32_t
rk3588_hdptx_pack_lanes(struct rk3588_hdptx_phy_softc *sc, int reg)
{
	uint32_t value;
	int lane;

	value = 0;
	for (lane = 3; lane <= 6; lane++)
		value |= (bus_read_4(sc->mem, ((lane << 8) + reg) * 4) & 0xff) <<
		    ((lane - 3) * 8);
	return (value);
}

static void
rk3588_hdptx_program_tmds_lanes(struct rk3588_hdptx_phy_softc *sc)
{
	bool asserted;
	bus_size_t base;
	uint32_t mask, ready_mask, status;
	int error, i, lane, reset_error;

	sc->lane_attempted = 1;
	sc->lane_grf_status_before = SYSCON_READ_4(sc->grf,
	    RK3588_HDPTX_GRF_STATUS);
	error = sc->pll_error;
	if (error != 0)
		goto out;
	error = hwreset_is_asserted(sc->resets[3], &asserted);
	if (error != 0)
		goto out;
	sc->lane_reset_before_asserted = asserted;
	if (!asserted) {
		error = EIO;
		goto out;
	}

	rk3588_hdptx_lane_write_sequence(sc, 0, rk3588_hdptx_tmds_sb,
	    nitems(rk3588_hdptx_tmds_sb));
	rk3588_hdptx_lane_write_sequence(sc, 0, rk3588_hdptx_tmds_lntop,
	    nitems(rk3588_hdptx_tmds_lntop));
	for (lane = 3; lane <= 5; lane++) {
		base = (lane << 8) * 4;
		rk3588_hdptx_lane_write_sequence(sc, base,
		    rk3588_hdptx_lane_prefix,
		    nitems(rk3588_hdptx_lane_prefix));
		rk3588_hdptx_lane_write_sequence(sc, base,
		    rk3588_hdptx_lane_tail, nitems(rk3588_hdptx_lane_tail));
	}
	base = (6 << 8) * 4;
	rk3588_hdptx_lane_write_sequence(sc, base, rk3588_hdptx_lane_prefix,
	    nitems(rk3588_hdptx_lane_prefix));
	rk3588_hdptx_lane_write(sc, base + 0x1e * 4, 0x08);
	for (lane = 3; lane <= 6; lane++)
		rk3588_hdptx_lane_write(sc, ((lane << 8) + 0x1e) * 4,
		    lane == 6 ? 0x0a : 0x02);
	rk3588_hdptx_lane_write_sequence(sc, base, rk3588_hdptx_lane_tail,
	    nitems(rk3588_hdptx_lane_tail));
	for (lane = 3; lane <= 6; lane++)
		rk3588_hdptx_lane_write(sc, ((lane << 8) + 0x03) * 4, 0x2f);
	for (lane = 3; lane <= 6; lane++)
		rk3588_hdptx_lane_write(sc, ((lane << 8) + 0x05) * 4, 0x03);
	for (lane = 3; lane <= 6; lane++)
		rk3588_hdptx_lane_write(sc, ((lane << 8) + 0x06) * 4, 0x1c);
	rk3588_hdptx_verify_lane_table(sc, false);
	if (sc->lane_verify_errors != 0) {
		error = EIO;
		goto out;
	}

	error = hwreset_deassert(sc->resets[3]);
	if (error != 0)
		goto out;
	mask = RK3588_HDPTX_I_BIAS_EN | RK3588_HDPTX_I_BGR_EN;
	SYSCON_WRITE_4(sc->grf, RK3588_HDPTX_GRF_CON0,
	    (mask << 16) | mask);
	rk3588_hdptx_lane_write(sc, 0x207 * 4, 0x0f);

	ready_mask = RK3588_HDPTX_O_PHY_RDY |
	    RK3588_HDPTX_O_PLL_LOCK_DONE;
	error = ETIMEDOUT;
	for (i = 0; i < 50; i++) {
		status = SYSCON_READ_4(sc->grf, RK3588_HDPTX_GRF_STATUS);
		sc->lane_poll_count++;
		if ((status & ready_mask) == ready_mask) {
			error = 0;
			break;
		}
		if (i != 49)
			DELAY(100);
	}
	rk3588_hdptx_verify_lane_table(sc, true);
	if (error == 0 && sc->lane_verify_errors != 0)
		error = EIO;

out:
	sc->lane_grf_con0_final = SYSCON_READ_4(sc->grf,
	    RK3588_HDPTX_GRF_CON0);
	sc->lane_grf_status_after = SYSCON_READ_4(sc->grf,
	    RK3588_HDPTX_GRF_STATUS);
	ready_mask = RK3588_HDPTX_O_PHY_RDY |
	    RK3588_HDPTX_O_PLL_LOCK_DONE;
	sc->lane_ready = (sc->lane_grf_status_after & ready_mask) == ready_mask;
	reset_error = hwreset_is_asserted(sc->resets[3], &asserted);
	if (reset_error == 0)
		sc->lane_reset_after_asserted = asserted;
	else if (error == 0)
		error = reset_error;
	if (error == 0 && (!sc->lane_ready || sc->lane_reset_after_asserted))
		error = EIO;
	sc->lane_lntop_0_3 = rk3588_hdptx_pack_lntop(sc, 0x200);
	sc->lane_lntop_4_7 = rk3588_hdptx_pack_lntop(sc, 0x204);
	sc->lane_tx_ctrl = rk3588_hdptx_pack_lanes(sc, 0x03);
	sc->lane_skew = rk3588_hdptx_pack_lanes(sc, 0x1e);
	sc->lane_drive_05 = rk3588_hdptx_pack_lanes(sc, 0x05);
	sc->lane_drive_06 = rk3588_hdptx_pack_lanes(sc, 0x06);
	sc->lane_error = error;
}

static int
rk3588_hdptx_pre_power_up(struct rk3588_hdptx_phy_softc *sc)
{
	uint32_t mask;
	int error;

	error = hwreset_assert(sc->resets[0]);
	if (error != 0)
		return (error);
	DELAY(20);
	error = hwreset_deassert(sc->resets[0]);
	if (error != 0)
		return (error);
	error = hwreset_assert(sc->resets[3]);
	if (error != 0)
		return (error);
	error = hwreset_assert(sc->resets[1]);
	if (error != 0)
		return (error);
	error = hwreset_assert(sc->resets[2]);
	if (error != 0)
		return (error);

	mask = RK3588_HDPTX_I_PLL_EN | RK3588_HDPTX_I_BIAS_EN |
	    RK3588_HDPTX_I_BGR_EN;
	SYSCON_WRITE_4(sc->grf, RK3588_HDPTX_GRF_CON0, mask << 16);
	return (0);
}

static void
rk3588_hdptx_program_tmds(struct rk3588_hdptx_phy_softc *sc,
    const struct rk3588_hdptx_ropll_config *cfg)
{
	bool asserted;
	uint32_t expected[nitems(rk3588_hdptx_cmn_probes)];
	uint32_t mask, status;
	int error, i, reset_error;

	sc->pll_attempted = 1;
	sc->pll_grf_status_before = SYSCON_READ_4(sc->grf,
	    RK3588_HDPTX_GRF_STATUS);
	error = rk3588_hdptx_pre_power_up(sc);
	if (error != 0)
		goto out;
	sc->pll_grf_con0_before = SYSCON_READ_4(sc->grf,
	    RK3588_HDPTX_GRF_CON0);
	mask = RK3588_HDPTX_LC_REF_CLK_SEL;
	SYSCON_WRITE_4(sc->grf, RK3588_HDPTX_GRF_CON0, mask << 16);
	sc->pll_grf_con0_after = SYSCON_READ_4(sc->grf,
	    RK3588_HDPTX_GRF_CON0);

	rk3588_hdptx_write_sequence(sc, rk3588_hdptx_common_cmn,
	    nitems(rk3588_hdptx_common_cmn));
	if (sc->pll_verify_errors != 0) {
		error = EIO;
		goto out;
	}
	rk3588_hdptx_write_sequence(sc, rk3588_hdptx_tmds_cmn,
	    nitems(rk3588_hdptx_tmds_cmn));
	if (sc->pll_verify_errors != 0) {
		error = EIO;
		goto out;
	}

	/* Exact Linux table parameters for the selected RGB 8-bpc rate. */
	rk3588_hdptx_write_reg(sc, 0x051 * 4, cfg->mdiv);
	rk3588_hdptx_write_reg(sc, 0x055 * 4, cfg->mdiv);
	rk3588_hdptx_write_reg(sc, 0x059 * 4, 0x11);
	rk3588_hdptx_write_reg(sc, 0x05a * 4, cfg->sdiv << 4);
	rk3588_hdptx_update_reg(sc, 0x05e * 4, 1u << 6, 1u << 6);
	rk3588_hdptx_update_reg(sc, 0x064 * 4, 1u << 3,
	    cfg->sdm_num_sign << 3);
	rk3588_hdptx_write_reg(sc, 0x060 * 4, cfg->sdm_deno);
	rk3588_hdptx_write_reg(sc, 0x065 * 4, cfg->sdm_num);
	rk3588_hdptx_update_reg(sc, 0x069 * 4, 0x7, cfg->sdc_n);
	rk3588_hdptx_write_reg(sc, 0x06c * 4, cfg->sdc_num);
	rk3588_hdptx_write_reg(sc, 0x070 * 4, cfg->sdc_deno);
	rk3588_hdptx_update_reg(sc, 0x086 * 4, 0xf0, cfg->sdiv << 4);
	rk3588_hdptx_update_reg(sc, 0x086 * 4, 0x0e, 0x00);
	rk3588_hdptx_update_reg(sc, 0x086 * 4, 0x01, 0x01);
	bus_barrier(sc->mem, 0, rman_get_size(sc->mem),
	    BUS_SPACE_BARRIER_WRITE);
	expected[0] = 0;
	expected[1] = cfg->mdiv;
	expected[2] = cfg->mdiv;
	expected[3] = 0x11;
	expected[4] = cfg->sdiv << 4;
	expected[5] = 0x4f;
	expected[6] = cfg->sdm_deno;
	expected[7] = (0x0f & ~(1u << 3)) | (cfg->sdm_num_sign << 3);
	expected[8] = cfg->sdm_num;
	expected[9] = cfg->sdc_n;
	expected[10] = cfg->sdc_num;
	expected[11] = cfg->sdc_deno;
	expected[12] = (cfg->sdiv << 4) | 1;
	for (i = 0; i < nitems(rk3588_hdptx_cmn_probes); i++)
		rk3588_hdptx_verify_reg(sc, rk3588_hdptx_cmn_probes[i].reg,
		    expected[i], 0xff);
	if (sc->pll_verify_errors != 0) {
		error = EIO;
		goto out;
	}

	mask = RK3588_HDPTX_I_BIAS_EN | RK3588_HDPTX_I_BGR_EN;
	SYSCON_WRITE_4(sc->grf, RK3588_HDPTX_GRF_CON0,
	    (mask << 16) | mask);
	DELAY(10);
	error = hwreset_deassert(sc->resets[2]);
	if (error != 0)
		goto out;
	DELAY(10);
	mask = RK3588_HDPTX_I_PLL_EN;
	SYSCON_WRITE_4(sc->grf, RK3588_HDPTX_GRF_CON0,
	    (mask << 16) | mask);
	DELAY(10);
	error = hwreset_deassert(sc->resets[1]);
	if (error != 0)
		goto out;

	error = ETIMEDOUT;
	for (i = 0; i <= 20; i++) {
		status = SYSCON_READ_4(sc->grf, RK3588_HDPTX_GRF_STATUS);
		sc->pll_poll_count++;
		if ((status & RK3588_HDPTX_O_PHY_CLK_RDY) != 0) {
			error = 0;
			break;
		}
		if (i != 20)
			DELAY(20);
	}

out:
	sc->pll_grf_con0_final = SYSCON_READ_4(sc->grf,
	    RK3588_HDPTX_GRF_CON0);
	sc->pll_grf_status_after = SYSCON_READ_4(sc->grf,
	    RK3588_HDPTX_GRF_STATUS);
	sc->pll_ready = (sc->pll_grf_status_after &
	    RK3588_HDPTX_O_PHY_CLK_RDY) != 0;
	if (error == 0 && sc->pll_ready == 0)
		error = EIO;
	if (error == 0) {
		for (i = 0; i < nitems(rk3588_hdptx_cmn_probes); i++)
			rk3588_hdptx_verify_reg(sc,
			    rk3588_hdptx_cmn_probes[i].reg,
			    expected[i], 0xff);
		if (sc->pll_verify_errors != 0)
			error = EIO;
	}
	rk3588_hdptx_snapshot_cmn(sc, sc->pll_cmn_snapshot);
	reset_error = 0;
	for (i = 0; i < nitems(rk3588_hdptx_reset_names); i++) {
		reset_error = hwreset_is_asserted(sc->resets[i], &asserted);
		if (reset_error != 0)
			break;
		if (asserted)
			sc->pll_reset_asserted |= 1u << i;
	}
	if (error == 0 && reset_error != 0)
		error = reset_error;
	if (error == 0 && sc->pll_reset_asserted != (1u << 3))
		error = EIO;
	sc->pll_error = error;
}

int
rk3588_hdptx_configure_mode(int port, const struct videomode *mode)
{
	const struct rk3588_hdptx_ropll_config *ropll;
	struct rk3588_hdptx_phy_softc *sc;
	uint64_t rate;

	if (port < 0 || port >= (int)nitems(rk3588_hdptx_sc) || mode == NULL)
		return (EINVAL);
	sc = rk3588_hdptx_sc[port];
	if (sc == NULL)
		return (ENXIO);
	rate = (uint64_t)mode->dot_clock * 1000;
	ropll = rk3588_hdptx_find_ropll(rate);
	if (ropll == NULL)
		return (ERANGE);
	if (sc->lane_ready != 0 && sc->tmds_rate == rate)
		return (0);

	sc->tmds_rate = rate;
	sc->pll_error = 0;
	sc->lane_error = 0;
	rk3588_hdptx_program_tmds(sc, ropll);
	if (sc->pll_error != 0)
		return (sc->pll_error);
	rk3588_hdptx_program_tmds_lanes(sc);
	sc->pixel_clock_rate = sc->lane_error == 0 ? rate : 0;
	return (sc->lane_error);
}

int
rk3588_hdptx_disable(int port)
{
	struct rk3588_hdptx_phy_softc *sc;
	uint32_t mask;
	int error, lane;

	if (port < 0 || port >= (int)nitems(rk3588_hdptx_sc))
		return (EINVAL);
	sc = rk3588_hdptx_sc[port];
	if (sc == NULL)
		return (ENXIO);

	error = hwreset_assert(sc->resets[0]);
	if (error != 0)
		return (error);
	DELAY(20);
	error = hwreset_deassert(sc->resets[0]);
	if (error != 0)
		return (error);
	rk3588_hdptx_lane_write(sc, 0x300 * 4, 0x82);
	rk3588_hdptx_lane_write(sc, 0x10f * 4, 0xc1);
	rk3588_hdptx_lane_write(sc, 0x110 * 4, 0x01);
	for (lane = 3; lane <= 6; lane++)
		rk3588_hdptx_lane_write(sc, ((lane << 8) + 1) * 4, 0x80);

	error = hwreset_assert(sc->resets[3]);
	if (error == 0)
		error = hwreset_assert(sc->resets[1]);
	if (error == 0)
		error = hwreset_assert(sc->resets[2]);
	if (error != 0)
		return (error);
	mask = RK3588_HDPTX_I_PLL_EN | RK3588_HDPTX_I_BIAS_EN |
	    RK3588_HDPTX_I_BGR_EN;
	SYSCON_WRITE_4(sc->grf, RK3588_HDPTX_GRF_CON0, mask << 16);
	bus_barrier(sc->mem, 0, rman_get_size(sc->mem),
	    BUS_SPACE_BARRIER_READ | BUS_SPACE_BARRIER_WRITE);
	sc->pll_ready = 0;
	sc->lane_ready = 0;
	sc->pixel_clock_rate = 0;
	return (0);
}

static int
rk3588_hdptx_probe_resets(struct rk3588_hdptx_phy_softc *sc)
{
	bool asserted;
	int error, i;

	for (i = 0; i < nitems(rk3588_hdptx_reset_names); i++) {
		error = hwreset_get_by_ofw_name(sc->dev, sc->node,
		    __DECONST(char *, rk3588_hdptx_reset_names[i]),
		    &sc->resets[i]);
		if (error != 0) {
			device_printf(sc->dev, "Cannot get %s reset: %d\n",
			    rk3588_hdptx_reset_names[i], error);
			return (error);
		}
		sc->reset_get_mask |= 1u << i;
		error = hwreset_is_asserted(sc->resets[i], &asserted);
		if (error != 0) {
			device_printf(sc->dev, "Cannot read %s reset: %d\n",
			    rk3588_hdptx_reset_names[i], error);
			return (error);
		}
		if (asserted)
			sc->reset_before_asserted |= 1u << i;
	}

	for (i = 0; i < 3; i++) {
		error = hwreset_deassert(sc->resets[i]);
		if (error != 0) {
			device_printf(sc->dev, "Cannot deassert %s reset: %d\n",
			    rk3588_hdptx_reset_names[i], error);
			return (error);
		}
		error = hwreset_is_asserted(sc->resets[i], &asserted);
		if (error != 0 || asserted) {
			device_printf(sc->dev,
			    "%s reset did not read back deasserted: %d/%u\n",
			    rk3588_hdptx_reset_names[i], error, asserted);
			return (error != 0 ? error : EIO);
		}
		sc->reset_deassert_mask |= 1u << i;
	}

	for (i = 0; i < nitems(rk3588_hdptx_reset_names); i++) {
		error = hwreset_is_asserted(sc->resets[i], &asserted);
		if (error != 0) {
			device_printf(sc->dev,
			    "Cannot read final %s reset state: %d\n",
			    rk3588_hdptx_reset_names[i], error);
			return (error);
		}
		if (asserted)
			sc->reset_after_asserted |= 1u << i;
	}
	return (0);
}

static int
rk3588_hdptx_probe(device_t dev)
{

	if (!ofw_bus_status_okay(dev))
		return (ENXIO);
	if (!ofw_bus_is_compatible(dev, "rockchip,rk3588-hdptx-phy"))
		return (ENXIO);
	device_set_desc(dev, "Rockchip RK3588 Samsung HDMI HDPTX PHY");
	return (BUS_PROBE_DEFAULT);
}

static int
rk3588_hdptx_attach(device_t dev)
{
	const struct rk3588_hdptx_ropll_config *ropll;
	struct rk3588_hdptx_phy_softc *sc;
	struct phynode_init_def phy_init;
	struct videomode mode;
	rman_res_t base, size;
	int error, rid;

	sc = device_get_softc(dev);
	sc->dev = dev;
	sc->node = ofw_bus_get_node(dev);
	error = bus_get_resource(dev, SYS_RES_MEMORY, 0, &base, &size);
	if (error != 0)
		return (error);
	if (base == RK3588_HDPTX0_BASE)
		sc->port = 0;
	else if (base == RK3588_HDPTX1_BASE)
		sc->port = 1;
	else {
		device_printf(dev, "Unsupported register base %#jx\n",
		    (uintmax_t)base);
		return (ENXIO);
	}
	sc->base = base;
	if (sc->port == 0 && rk3588_hdmi_qp_get_mode(0, &mode) == 0) {
		sc->tmds_rate = (uint64_t)mode.dot_clock * 1000;
		ropll = rk3588_hdptx_find_ropll(sc->tmds_rate);
	} else {
		ropll = NULL;
	}

	error = syscon_get_by_ofw_property(dev, sc->node, "rockchip,grf",
	    &sc->grf);
	if (error != 0) {
		device_printf(dev, "Cannot get HDPTX GRF syscon: %d\n", error);
		return (error);
	}
	error = rk3588_hdptx_get_clocks(sc);
	if (error != 0)
		return (error);
	sc->grf_status_before = SYSCON_READ_4(sc->grf,
	    RK3588_HDPTX_GRF_STATUS);
	error = rk3588_hdptx_probe_resets(sc);
	if (error != 0)
		return (error);
	sc->grf_status_after = SYSCON_READ_4(sc->grf,
	    RK3588_HDPTX_GRF_STATUS);
	error = rk3588_hdptx_enable_clocks(sc);
	if (error != 0)
		return (error);
	rid = 0;
	sc->mem = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid, RF_ACTIVE);
	if (sc->mem == NULL) {
		device_printf(dev, "Cannot allocate HDPTX MMIO resource\n");
		rk3588_hdptx_disable_clocks(sc);
		return (ENXIO);
	}
	rk3588_hdptx_snapshot_cmn(sc, sc->cmn_snapshot);

	bzero(&phy_init, sizeof(phy_init));
	phy_init.id = PHY_NONE;
	phy_init.ofw_node = sc->node;
	sc->phynode = phynode_create(dev, &rk3588_hdptx_phynode_class,
	    &phy_init);
	if (sc->phynode == NULL || phynode_register(sc->phynode) == NULL) {
		device_printf(dev, "Cannot register HDPTX PHY provider\n");
		bus_release_resource(dev, SYS_RES_MEMORY, rid, sc->mem);
		sc->mem = NULL;
		rk3588_hdptx_disable_clocks(sc);
		return (ENXIO);
	}
	if (bootverbose)
		device_printf(dev,
	    "RK3588-DIAG V2230 port=%d base=%#jx clocks=%#x "
	    "rates=%ju/%ju resets=%#x before=%#x deassert=%#x after=%#x "
	    "grf=%#x->%#x analog-enable=unsupported\n",
	    sc->port, (uintmax_t)base, sc->clock_mask,
	    (uintmax_t)sc->clock_rate[0], (uintmax_t)sc->clock_rate[1],
	    sc->reset_get_mask, sc->reset_before_asserted,
	    sc->reset_deassert_mask, sc->reset_after_asserted,
	    sc->grf_status_before, sc->grf_status_after);
	if (bootverbose)
		device_printf(dev,
	    "RK3588-DIAG V2232 port=%d clocks-enabled=%#x mmio=%#jx "
	    "cmn=0008:%#x,0051:%#x,0055:%#x,0059:%#x,005a:%#x,"
	    "005e:%#x,0060:%#x,0064:%#x,0065:%#x,0069:%#x,006c:%#x,"
	    "0070:%#x,0086:%#x analog-write=none lane-reset=asserted\n",
	    sc->port, sc->clock_enable_mask, (uintmax_t)sc->base,
	    sc->cmn_snapshot[0], sc->cmn_snapshot[1], sc->cmn_snapshot[2],
	    sc->cmn_snapshot[3], sc->cmn_snapshot[4], sc->cmn_snapshot[5],
	    sc->cmn_snapshot[6], sc->cmn_snapshot[7], sc->cmn_snapshot[8],
	    sc->cmn_snapshot[9], sc->cmn_snapshot[10], sc->cmn_snapshot[11],
	    sc->cmn_snapshot[12]);
	if (sc->port == 0) {
		if (bootverbose)
			device_printf(dev,
		    "RK3588-DIAG V2272 port=0 begin rate=%ju supported=%u "
		    "refsel=clear-next common-reset=assert-next "
		    "lane-reset=asserted\n",
		    (uintmax_t)sc->tmds_rate, ropll != NULL);
		if (ropll != NULL)
			rk3588_hdptx_program_tmds(sc, ropll);
		else
			sc->pll_error = ERANGE;
		if (bootverbose || sc->pll_error != 0)
			device_printf(dev,
		    "RK3588-DIAG V2272 port=0 rate=%ju attempted=%u error=%d "
		    "writes/verifies/fatal/advisory=%u/%u/%u/%u "
		    "first=%#x:%#x/%#x reg0030=%#x/%#x "
		    "con0=%#x->%#x->%#x grf=%#x->%#x polls=%u ready=%u "
		    "resets=%#x "
		    "cmn=0008:%#x,0051:%#x,0055:%#x,0059:%#x,005a:%#x,"
		    "005e:%#x,0060:%#x,0064:%#x,0065:%#x,0069:%#x,"
		    "006c:%#x,0070:%#x,0086:%#x lane-reset=asserted\n",
		    (uintmax_t)sc->tmds_rate, sc->pll_attempted, sc->pll_error,
		    sc->pll_write_count, sc->pll_verify_count,
		    sc->pll_verify_errors, sc->pll_advisory_mismatches,
		    sc->pll_first_bad_reg,
		    sc->pll_first_bad_expected, sc->pll_first_bad_actual,
		    sc->pll_reg0030_expected, sc->pll_reg0030_actual,
		    sc->pll_grf_con0_before, sc->pll_grf_con0_after,
		    sc->pll_grf_con0_final,
		    sc->pll_grf_status_before, sc->pll_grf_status_after,
		    sc->pll_poll_count, sc->pll_ready, sc->pll_reset_asserted,
		    sc->pll_cmn_snapshot[0], sc->pll_cmn_snapshot[1],
		    sc->pll_cmn_snapshot[2], sc->pll_cmn_snapshot[3],
		    sc->pll_cmn_snapshot[4], sc->pll_cmn_snapshot[5],
		    sc->pll_cmn_snapshot[6], sc->pll_cmn_snapshot[7],
		    sc->pll_cmn_snapshot[8], sc->pll_cmn_snapshot[9],
		    sc->pll_cmn_snapshot[10], sc->pll_cmn_snapshot[11],
		    sc->pll_cmn_snapshot[12]);
		rk3588_hdptx_program_tmds_lanes(sc);
		if (bootverbose || sc->lane_error != 0)
			device_printf(dev,
		    "RK3588-DIAG V2272 port=0 rate=%ju attempted=%u error=%d "
		    "writes/verifies/fatal=%u/%u/%u first=%#x:%#x/%#x "
		    "grf=%#x->%#x polls=%u ready=%u con0=%#x reset=%u->%u "
		    "lntop=%#x/%#x tx=%#x skew=%#x drive=%#x/%#x "
		    "hdmi-qp=disabled vop2=disabled\n",
		    (uintmax_t)sc->tmds_rate, sc->lane_attempted, sc->lane_error,
		    sc->lane_write_count, sc->lane_verify_count,
		    sc->lane_verify_errors, sc->lane_first_bad_reg,
		    sc->lane_first_bad_expected, sc->lane_first_bad_actual,
		    sc->lane_grf_status_before, sc->lane_grf_status_after,
		    sc->lane_poll_count, sc->lane_ready, sc->lane_grf_con0_final,
		    sc->lane_reset_before_asserted,
		    sc->lane_reset_after_asserted, sc->lane_lntop_0_3,
		    sc->lane_lntop_4_7, sc->lane_tx_ctrl, sc->lane_skew,
		    sc->lane_drive_05, sc->lane_drive_06);
	} else {
		if (bootverbose)
			device_printf(dev,
		    "RK3588-DIAG V2236 port=1 skipped=non-target "
		    "analog-write=none lane-reset=asserted\n");
	}
	error = rk3588_hdptx_register_pixel_clock(sc);
	if (error != 0) {
		device_printf(dev, "Cannot register HDPTX pixel clock: %d\n",
		    error);
		return (error);
	}
	if (bootverbose)
		device_printf(dev,
	    "RK3588-DIAG V2237 port=%d provider=%u name=%s parent=%s "
	    "rate=%ju ready=%u\n",
	    sc->port, sc->pixel_clock_registered,
	    rk3588_hdptx_pixel_clock_names[sc->port], sc->pixel_clock_parent,
	    (uintmax_t)sc->pixel_clock_rate,
	    rk3588_hdptx_clk_ready(sc) ? 1 : 0);
	rk3588_hdptx_add_sysctls(sc);
	rk3588_hdptx_sc[sc->port] = sc;
	return (0);
}

static device_method_t rk3588_hdptx_methods[] = {
	DEVMETHOD(device_probe,		rk3588_hdptx_probe),
	DEVMETHOD(device_attach,	rk3588_hdptx_attach),

	DEVMETHOD_END
};

static driver_t rk3588_hdptx_driver = {
	"rk3588_hdptx_phy",
	rk3588_hdptx_methods,
	sizeof(struct rk3588_hdptx_phy_softc),
};

EARLY_DRIVER_MODULE(rk3588_hdptx_phy, simplebus, rk3588_hdptx_driver, 0, 0,
    BUS_PASS_DEFAULT);
MODULE_VERSION(rk3588_hdptx_phy, 1);
