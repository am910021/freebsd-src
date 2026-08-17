/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2019 Emmanuel Vadot <manu@FreeBSD.Org>
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 */

#ifndef _ARM64_ROCKCHIP_RK_USB2PHY_H_
#define	_ARM64_ROCKCHIP_RK_USB2PHY_H_

enum rk_usbphy {
	RK_USBPHY_HOST = 0,
	RK_USBPHY_OTG,
};

struct rk_usb2phy_reg {
	uint32_t	offset;
	uint32_t	enable_mask;
	uint32_t	disable_mask;
};

struct rk_usb2phy_softc;

struct rk_usb2phy_regs {
	struct rk_usb2phy_reg	clk_ctl;
	struct rk_usb2phy_reg	host_ctl;
	bool			has_host_ctl;
	bool			has_otg;
	bool			relative_offsets;
	bool			resets_required;
	u_int			clock_enable_delay;
	int			(*enable)(struct rk_usb2phy_softc *,
				    intptr_t, bool);
	void			(*port_init)(struct rk_usb2phy_softc *,
				    intptr_t);
};

struct rk_usb2phy_softc {
	device_t		dev;
	struct syscon		*grf;
	regulator_t		phy_supply;
	clk_t			clk;
	hwreset_array_t		resets;
	struct rk_usb2phy_regs	regs;
	uint32_t		reg_base;
	phandle_t		host_node;
	int			mode;
	bool			running;
	bool			host_init_done;
	bool			tuned;
};

extern const struct rk_usb2phy_regs rk3588_usb2phy_regs;

#endif
