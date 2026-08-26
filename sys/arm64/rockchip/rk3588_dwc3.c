/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/condvar.h>

#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include <arm64/rockchip/rk3588_power.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usb_core.h>
#include <dev/usb/usb_busdma.h>
#include <dev/usb/usb_process.h>
#include <dev/usb/usb_controller.h>
#include <dev/usb/usb_bus.h>
#include <dev/usb/controller/dwc3.h>
#include <dev/usb/controller/dwc3_soc.h>

#define	DWC3_GUSB3PIPECTL0_UX_EXIT_PX		(1U << 27)
#define	DWC3_GUCTL_REFCLKPER_MASK		0xffc00000
#define	DWC3_GUCTL_REFCLKPER_SHIFT		22
#define	DWC3_GUCTL1_DEV_DECOUPLE_L1L2_EVT	(1U << 31)
#define	DWC3_GUCTL1_DEV_L1_EXIT_BY_HW		(1U << 24)
#define	DWC3_GUCTL1_PARKMODE_DISABLE_SS		(1U << 17)

#define	RK3588_DWC3_GFLADJ_REFCLK_FLADJ_MASK	0x003fff00
#define	RK3588_DWC3_GFLADJ_REFCLK_FLADJ_SHIFT	8
#define	RK3588_DWC3_GFLADJ_240MHZDECR_MASK	0x7f000000
#define	RK3588_DWC3_GFLADJ_240MHZDECR_SHIFT	24
#define	RK3588_DWC3_GFLADJ_240MHZDECR_PLS1	0x80000000

#define	RK3588_DWC3_READ(ctx, reg)					\
	bus_space_read_4((ctx)->bst, (ctx)->bsh, (reg))
#define	RK3588_DWC3_WRITE(ctx, reg, value)				\
	bus_space_write_4((ctx)->bst, (ctx)->bsh, (reg), (value))

static bool
rk3588_dwc3_match(device_t dev)
{

	return (ofw_bus_is_compatible(dev, "rockchip,rk3588-dwc3"));
}

static int
rk3588_dwc3_enable_power(device_t dev)
{
	int error;

	error = rk3588_power_domain_enable_by_node(dev, ofw_bus_get_node(dev));
	return (error == ENOENT ? 0 : error);
}

static void
rk3588_dwc3_pipe_setup(const struct dwc3_soc_context *ctx)
{
	uint32_t reg;

	reg = RK3588_DWC3_READ(ctx, DWC3_GUSB3PIPECTL0);
	reg &= ~DWC3_GUSB3PIPECTL0_UX_EXIT_PX;
	reg &= ~DWC3_GUSB3PIPECTL0_SUSPENDUSB3;
	if (device_has_property(ctx->dev,
	    "snps,dis-del-phy-power-chg-quirk"))
		reg &= ~DWC3_GUSB3PIPECTL0_DELAYP1TRANS;
	if (device_has_property(ctx->dev, "snps,dis_rxdet_inp3_quirk"))
		reg |= DWC3_GUSB3PIPECTL0_DISRXDETINP3;
	RK3588_DWC3_WRITE(ctx, DWC3_GUSB3PIPECTL0, reg);
}

static int
rk3588_dwc3_configure_core(const struct dwc3_soc_context *ctx)
{
	uint64_t rate;
	uint32_t decr, fladj, gfladj, guctl, guctl1, period;

	rate = ctx->ref_clk_rate;
	if (rate == 0)
		return (ENXIO);

	period = 1000000000ULL / rate;
	if (period == 0)
		return (ERANGE);

	guctl = RK3588_DWC3_READ(ctx, DWC3_GUCTL);
	guctl &= ~DWC3_GUCTL_REFCLKPER_MASK;
	guctl |= (period << DWC3_GUCTL_REFCLKPER_SHIFT) &
	    DWC3_GUCTL_REFCLKPER_MASK;
	RK3588_DWC3_WRITE(ctx, DWC3_GUCTL, guctl);

	fladj = (uint32_t)((125000ULL * 1000000000ULL) /
	    (rate * period));
	fladj -= 125000;
	decr = 480000000 / rate;
	gfladj = RK3588_DWC3_READ(ctx, DWC3_GFLADJ);
	gfladj &= ~RK3588_DWC3_GFLADJ_REFCLK_FLADJ_MASK;
	gfladj &= ~RK3588_DWC3_GFLADJ_240MHZDECR_MASK;
	gfladj &= ~RK3588_DWC3_GFLADJ_240MHZDECR_PLS1;
	gfladj |= (fladj << RK3588_DWC3_GFLADJ_REFCLK_FLADJ_SHIFT) &
	    RK3588_DWC3_GFLADJ_REFCLK_FLADJ_MASK;
	gfladj |= ((decr >> 1) << RK3588_DWC3_GFLADJ_240MHZDECR_SHIFT) &
	    RK3588_DWC3_GFLADJ_240MHZDECR_MASK;
	if ((decr & 1) != 0)
		gfladj |= RK3588_DWC3_GFLADJ_240MHZDECR_PLS1;
	RK3588_DWC3_WRITE(ctx, DWC3_GFLADJ, gfladj);

	guctl1 = RK3588_DWC3_READ(ctx, DWC3_GUCTL1);
	guctl1 |= DWC3_GUCTL1_DEV_L1_EXIT_BY_HW;
	guctl1 |= DWC3_GUCTL1_DEV_DECOUPLE_L1L2_EVT;
	guctl1 |= DWC3_GUCTL1_TX_IPGAP_LINECHECK_DIS;
	guctl1 |= DWC3_GUCTL1_PARKMODE_DISABLE_SS;
	RK3588_DWC3_WRITE(ctx, DWC3_GUCTL1, guctl1);
	return (0);
}

static const struct dwc3_soc_ops rk3588_dwc3_soc_ops = {
	.match = rk3588_dwc3_match,
	.enable_power = rk3588_dwc3_enable_power,
	.pipe_setup = rk3588_dwc3_pipe_setup,
	.configure_core = rk3588_dwc3_configure_core,
	.flags = DWC3_SOC_F_STRICT_RESOURCES |
	    DWC3_SOC_F_NO_HOST_AUTO_RETRY |
	    DWC3_SOC_F_KEEP_DUAL_ROLE_PHYS_ACTIVE,
	.usb_bus_quirks = USB_BUS_QUIRK_IGNORE_SS_CONNECT_CHANGE |
	    USB_BUS_QUIRK_DISABLE_SS_U1_TIMEOUT,
};

DWC3_SOC_REGISTER(rk3588, rk3588_dwc3_soc_ops);
