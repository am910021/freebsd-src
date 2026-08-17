/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <sys/taskqueue.h>

#include <dev/extres/clk/clk.h>
#include <dev/mmc/bridge.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#include <dev/sdhci/sdhci.h>
#include <dev/sdhci/sdhci_fdt_soc.h>

#define	RK3588_DWCMSHC_EMMC_HOST_CTRL			0x0508
#define	RK3588_DWCMSHC_EMMC_DLL_CTRL			0x0800
#define	RK3588_DWCMSHC_EMMC_DLL_RXCLK			0x0804
#define	RK3588_DWCMSHC_EMMC_DLL_TXCLK			0x0808
#define	RK3588_DWCMSHC_EMMC_DLL_STRBIN			0x080c

#define	DWCMSHC_P_VENDOR_AREA1			0x00e8
#define	 DWCMSHC_AREA1_MASK			0x00000fff
#define	DWCMSHC_HOST_CTRL3			0x0008
#define	DWCMSHC_EMMC_CONTROL			0x002c
#define	 DWCMSHC_CARD_IS_EMMC			0x00000001
#define	 DWCMSHC_ENHANCED_STROBE		0x00000100
#define	DWCMSHC_EMMC_ATCTRL			0x0040
#define	DWCMSHC_EMMC_DLL_CMDOUT			0x0810
#define	 DWCMSHC_CTRL_HS400			0x00000007
#define	RK3588_DWCMSHC_EMMC_DLL_STATUS0		0x0840
#define	RK3588_DWCMSHC_HS400_CLOCK		200000000

#define	RK3588_DLL_CTRL_START			0x00000001
#define	RK3588_DLL_CTRL_RESET			0x00000002
#define	RK3588_DLL_CTRL_BYPASS			0x01000000
#define	RK3588_DLL_LOCKED			0x00000100
#define	RK3588_DLL_TIMEOUT			0x00000200
#define	RK3588_DLL_RXCLK_ORI_GATE		0x80000000
#define	RK3588_DLL_RXCLK_NO_INVERTER		0x20000000
#define	RK3588_DLL_DLYENA			0x08000000
#define	RK3588_DLL_TXCLK_TAPNUM_DEFAULT		0x00000010
#define	RK3588_DLL_TXCLK_TAPNUM_90_DEGREES	0x0000000a
#define	RK3588_DLL_TXCLK_TAPNUM_FROM_SW		0x01000000
#define	RK3588_DLL_STRBIN_TAPNUM_DEFAULT		0x00000004
#define	RK3588_DLL_STRBIN_TAPNUM_FROM_SW		0x01000000
#define	RK3588_DLL_STRBIN_DELAY_NUM_SEL		0x04000000
#define	RK3588_DLL_STRBIN_DELAY_NUM_DEFAULT	0x00160000
#define	RK3588_DLL_CMDOUT_TAPNUM_90_DEGREES	0x00000008
#define	RK3588_DLL_CMDOUT_TAPNUM_FROM_SW		0x01000000
#define	RK3588_DLL_CMDOUT_SRC_CLK_NEG		0x10000000
#define	RK3588_DLL_CMDOUT_EN_SRC_CLK_NEG		0x20000000
#define	RK3588_DLL_START_POINT_DEFAULT		0x00050000
#define	RK3588_DLL_INC_DEFAULT			0x00000200

static int rk3588_sdhci_allow_hs400 = 1;

SYSCTL_NODE(_hw, OID_AUTO, rk3588, CTLFLAG_RD | CTLFLAG_MPSAFE, 0,
    "Rockchip RK3588 controls");
SYSCTL_INT(_hw_rk3588, OID_AUTO, sdhci_allow_hs400, CTLFLAG_RWTUN,
    &rk3588_sdhci_allow_hs400, 0,
    "Allow RK3588 SDHCI to advertise eMMC HS400/HS400ES support");

static bool
rk3588_sdhci_match(device_t dev)
{

	return (ofw_bus_is_compatible(dev, "rockchip,rk3588-dwcmshc"));
}

static bool
rk3588_sdhci_probe(device_t dev)
{

	if (!rk3588_sdhci_match(dev))
		return (false);

	device_set_desc(dev, "Rockchip RK3588 DWCMSHC SDHCI controller");
	return (true);
}

static void
rk3588_sdhci_probe_setup(device_t dev, u_int *quirks)
{

	*quirks = SDHCI_QUIRK_ALL_SLOTS_NON_REMOVABLE |
	    SDHCI_QUIRK_PRESET_VALUE_BROKEN |
	    SDHCI_QUIRK_WAIT_WHILE_BUSY;
	device_set_desc(dev, "Rockchip RK3588 DWCMSHC SDHCI controller");
}

static void
rk3588_sdhci_post_fdt_parse(device_t dev, u_int *quirks __unused,
    bool *wp_disabled)
{
	phandle_t node;

	node = ofw_bus_get_node(dev);
	if (OF_hasprop(node, "non-removable") && OF_hasprop(node, "no-sd") &&
	    OF_hasprop(node, "no-sdio"))
		*wp_disabled = true;
}

static bool
rk3588_sdhci_uses_core_clock(device_t dev __unused)
{

	return (true);
}

static bool
rk3588_sdhci_uses_controller_resets(device_t dev __unused)
{

	return (true);
}

static void
rk3588_sdhci_enable_optional_clock(device_t dev, const char *name)
{
	clk_t clk;
	int error;

	error = clk_get_by_ofw_name(dev, 0, name, &clk);
	if (error != 0)
		return;
	error = clk_enable(clk);
	if (error != 0)
		device_printf(dev,
		    "cannot enable optional %s clock: %d\n", name, error);
}

static void
rk3588_sdhci_clock_bringup(device_t dev)
{

	rk3588_sdhci_enable_optional_clock(dev, "bus");
	rk3588_sdhci_enable_optional_clock(dev, "axi");
	rk3588_sdhci_enable_optional_clock(dev, "block");
	rk3588_sdhci_enable_optional_clock(dev, "timer");
}

static uint32_t
rk3588_sdhci_vendor_area1(device_t dev, struct sdhci_slot *slot)
{
	uint32_t vendor;

	vendor = sdhci_fdt_slot_read_4(dev, slot, DWCMSHC_P_VENDOR_AREA1) &
	    DWCMSHC_AREA1_MASK;
	if (vendor == 0)
		vendor = RK3588_DWCMSHC_EMMC_HOST_CTRL - DWCMSHC_HOST_CTRL3;

	return (vendor);
}

static void
rk3588_sdhci_init_slot(device_t dev, struct sdhci_slot *slot)
{
	uint32_t emmc, vendor;
	phandle_t node;

	vendor = rk3588_sdhci_vendor_area1(dev, slot);
	sdhci_fdt_slot_write_4(dev, slot, vendor + DWCMSHC_HOST_CTRL3, 0);
	emmc = sdhci_fdt_slot_read_4(dev, slot,
	    vendor + DWCMSHC_EMMC_CONTROL);
	sdhci_fdt_slot_write_4(dev, slot, vendor + DWCMSHC_EMMC_CONTROL,
	    emmc & ~DWCMSHC_ENHANCED_STROBE);
	sdhci_fdt_slot_write_4(dev, slot, RK3588_DWCMSHC_EMMC_DLL_TXCLK, 0);
	sdhci_fdt_slot_write_4(dev, slot, RK3588_DWCMSHC_EMMC_DLL_STRBIN, 0);
	sdhci_fdt_slot_write_4(dev, slot, DWCMSHC_EMMC_DLL_CMDOUT, 0);

	node = ofw_bus_get_node(dev);
	if (OF_hasprop(node, "mmc-hs400-1_8v")) {
		slot->host.caps |= MMC_CAP_MMC_DDR52_180 |
		    MMC_CAP_MMC_HS200_180 | MMC_CAP_SIGNALING_180;
		if (rk3588_sdhci_allow_hs400)
			slot->host.caps |= MMC_CAP_MMC_HS400_180;
	}
	if (rk3588_sdhci_allow_hs400 &&
	    OF_hasprop(node, "mmc-hs400-enhanced-strobe"))
		slot->host.caps |= MMC_CAP_MMC_ENH_STROBE;
}

static bool
rk3588_sdhci_enable_high_speed_caps(device_t dev __unused)
{

	return (true);
}

static uint32_t
rk3588_sdhci_filter_caps2(device_t dev __unused, uint32_t caps2)
{

	if (!rk3588_sdhci_allow_hs400)
		caps2 &= ~SDHCI_CAN_MMC_HS400;

	return (caps2);
}

static void
rk3588_sdhci_low_speed_dll(device_t dev, struct sdhci_slot *slot,
    uint32_t clock __unused)
{
	uint32_t strbin;

	sdhci_fdt_slot_write_4(dev, slot, RK3588_DWCMSHC_EMMC_DLL_CTRL,
	    RK3588_DLL_CTRL_BYPASS | RK3588_DLL_CTRL_START);
	sdhci_fdt_slot_write_4(dev, slot, RK3588_DWCMSHC_EMMC_DLL_RXCLK,
	    RK3588_DLL_RXCLK_ORI_GATE);
	sdhci_fdt_slot_write_4(dev, slot, RK3588_DWCMSHC_EMMC_DLL_TXCLK, 0);
	sdhci_fdt_slot_write_4(dev, slot, DWCMSHC_EMMC_DLL_CMDOUT, 0);
	strbin = RK3588_DLL_DLYENA | RK3588_DLL_STRBIN_DELAY_NUM_SEL |
	    RK3588_DLL_STRBIN_DELAY_NUM_DEFAULT;
	sdhci_fdt_slot_write_4(dev, slot, RK3588_DWCMSHC_EMMC_DLL_STRBIN, strbin);
}

static void
rk3588_sdhci_high_speed_dll(device_t dev, struct sdhci_slot *slot,
    uint32_t clock)
{
	uint32_t vendor, status, txclk_tapnum, extra;
	enum mmc_bus_timing timing;
	int i;

	vendor = rk3588_sdhci_vendor_area1(dev, slot);
	timing = slot->host.ios.timing;

	sdhci_fdt_slot_write_4(dev, slot, RK3588_DWCMSHC_EMMC_DLL_CTRL,
	    RK3588_DLL_CTRL_RESET);
	DELAY(1);
	sdhci_fdt_slot_write_4(dev, slot, RK3588_DWCMSHC_EMMC_DLL_CTRL, 0);
	sdhci_fdt_slot_write_4(dev, slot, RK3588_DWCMSHC_EMMC_DLL_RXCLK,
	    RK3588_DLL_DLYENA);

	extra = RK3588_DLL_START_POINT_DEFAULT | RK3588_DLL_INC_DEFAULT |
	    RK3588_DLL_CTRL_START;
	sdhci_fdt_slot_write_4(dev, slot, RK3588_DWCMSHC_EMMC_DLL_CTRL,
	    extra);
	for (i = 0; i < 500; i++) {
		status = sdhci_fdt_slot_read_4(dev, slot,
		    RK3588_DWCMSHC_EMMC_DLL_STATUS0);
		if ((status & RK3588_DLL_LOCKED) != 0 &&
		    (status & RK3588_DLL_TIMEOUT) == 0)
			break;
		DELAY(1000);
	}
	if (i == 500) {
		device_printf(dev,
		    "high-speed DLL lock timeout clock=%u status=0x%x timing=%d\n",
		    clock, status, timing);
		return;
	}

	extra = (1U << 16) | (3U << 17) | (3U << 19);
	sdhci_fdt_slot_write_4(dev, slot, vendor + DWCMSHC_EMMC_ATCTRL,
	    extra);

	txclk_tapnum = RK3588_DLL_TXCLK_TAPNUM_DEFAULT;
	if (timing == bus_timing_mmc_hs400 ||
	    timing == bus_timing_mmc_hs400es) {
		txclk_tapnum = RK3588_DLL_TXCLK_TAPNUM_90_DEGREES;
		extra = RK3588_DLL_CMDOUT_SRC_CLK_NEG |
		    RK3588_DLL_CMDOUT_EN_SRC_CLK_NEG |
		    RK3588_DLL_DLYENA |
		    RK3588_DLL_CMDOUT_TAPNUM_90_DEGREES |
		    RK3588_DLL_CMDOUT_TAPNUM_FROM_SW;
		sdhci_fdt_slot_write_4(dev, slot, DWCMSHC_EMMC_DLL_CMDOUT,
		    extra);
		extra = sdhci_fdt_slot_read_4(dev, slot,
		    vendor + DWCMSHC_EMMC_CONTROL);
		if (timing == bus_timing_mmc_hs400es)
			extra |= DWCMSHC_ENHANCED_STROBE;
		else
			extra &= ~DWCMSHC_ENHANCED_STROBE;
		sdhci_fdt_slot_write_4(dev, slot, vendor + DWCMSHC_EMMC_CONTROL,
		    extra | DWCMSHC_CARD_IS_EMMC);
		device_printf(dev,
		    "HS400%s clock=%u dll=0x%x cmdout=0x%x emmc=0x%x\n",
		    timing == bus_timing_mmc_hs400es ? "ES" : "", clock,
		    status, sdhci_fdt_slot_read_4(dev, slot,
		    DWCMSHC_EMMC_DLL_CMDOUT),
		    sdhci_fdt_slot_read_4(dev, slot,
		    vendor + DWCMSHC_EMMC_CONTROL));
	}

	extra = RK3588_DLL_DLYENA | RK3588_DLL_TXCLK_TAPNUM_FROM_SW |
	    RK3588_DLL_RXCLK_NO_INVERTER | txclk_tapnum;
	sdhci_fdt_slot_write_4(dev, slot, RK3588_DWCMSHC_EMMC_DLL_TXCLK,
	    extra);
	extra = RK3588_DLL_DLYENA | RK3588_DLL_STRBIN_TAPNUM_DEFAULT |
	    RK3588_DLL_STRBIN_TAPNUM_FROM_SW;
	sdhci_fdt_slot_write_4(dev, slot, RK3588_DWCMSHC_EMMC_DLL_STRBIN,
	    extra);
}

static bool
rk3588_sdhci_set_clock(device_t dev, struct sdhci_slot *slot, int *clock)
{
	uint64_t freq;
	uint16_t hostctrl2;
	int error;

	if (*clock == 400000)
		*clock = 375000;
	/* The MMC core polls status before its later HS400 clock update. */
	if (*clock != 0 && *clock <= 52000000 &&
	    (slot->host.ios.timing == bus_timing_mmc_hs400 ||
	    slot->host.ios.timing == bus_timing_mmc_hs400es)) {
		device_printf(dev, "stage HS400%s clock before status\n",
		    slot->host.ios.timing == bus_timing_mmc_hs400es ?
		    "ES" : "");
		*clock = RK3588_DWCMSHC_HS400_CLOCK;
	}

	if (*clock != 0) {
		error = sdhci_fdt_set_core_clock_flags(dev, *clock,
		    *clock > 52000000 ? CLK_SET_ROUND_DOWN : 0, &freq);
		if (error != 0)
			device_printf(dev,
			    "cannot set core clock to %d: %d\n", *clock,
			    error);

		sdhci_fdt_slot_write_2(dev, slot, SDHCI_CLOCK_CONTROL, 0);

		if (*clock <= 52000000)
			rk3588_sdhci_low_speed_dll(dev, slot, *clock);
		else
			rk3588_sdhci_high_speed_dll(dev, slot, *clock);
	}
	if (*clock != 0 &&
	    (slot->host.ios.timing == bus_timing_mmc_hs400 ||
	    slot->host.ios.timing == bus_timing_mmc_hs400es)) {
		hostctrl2 = sdhci_fdt_slot_read_2(dev, slot,
		    SDHCI_HOST_CONTROL2);
		hostctrl2 &= ~SDHCI_CTRL2_UHS_MASK;
		hostctrl2 |= DWCMSHC_CTRL_HS400;
		sdhci_fdt_slot_write_2(dev, slot, SDHCI_HOST_CONTROL2,
		    hostctrl2);
		device_printf(dev, "HS400 Host Control2=0x%x\n", hostctrl2);
	}

	return (true);
}

static void
rk3588_sdhci_post_reset(device_t dev, struct sdhci_slot *slot, uint8_t mask)
{
	if ((mask & SDHCI_RESET_ALL) != 0)
		rk3588_sdhci_init_slot(dev, slot);
	if ((mask & SDHCI_RESET_CMD) == 0)
		return;

	sdhci_fdt_slot_write_4(dev, slot, SDHCI_INT_STATUS,
	    SDHCI_INT_RESPONSE);
}

static const struct sdhci_fdt_soc_ops rk3588_sdhci_ops = {
	.match = rk3588_sdhci_match,
	.probe = rk3588_sdhci_probe,
	.probe_setup = rk3588_sdhci_probe_setup,
	.post_fdt_parse = rk3588_sdhci_post_fdt_parse,
	.uses_core_clock = rk3588_sdhci_uses_core_clock,
	.uses_controller_resets = rk3588_sdhci_uses_controller_resets,
	.clock_bringup = rk3588_sdhci_clock_bringup,
	.init_slot = rk3588_sdhci_init_slot,
	.enable_high_speed_caps = rk3588_sdhci_enable_high_speed_caps,
	.filter_caps2 = rk3588_sdhci_filter_caps2,
	.set_clock = rk3588_sdhci_set_clock,
	.post_reset = rk3588_sdhci_post_reset,
};

static void
rk3588_sdhci_register(void *arg __unused)
{

	sdhci_fdt_soc_register(&rk3588_sdhci_ops);
}

SYSINIT(rk3588_sdhci_register, SI_SUB_DRIVERS, SI_ORDER_FIRST,
    rk3588_sdhci_register, NULL);
