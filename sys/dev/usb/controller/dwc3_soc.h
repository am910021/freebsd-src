/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 */

#ifndef _DEV_USB_CONTROLLER_DWC3_SOC_H_
#define	_DEV_USB_CONTROLLER_DWC3_SOC_H_

#include <sys/bus.h>
#include <sys/linker_set.h>

#include <machine/bus.h>

#define	DWC3_SOC_F_STRICT_RESOURCES		0x00000001
#define	DWC3_SOC_F_NO_HOST_AUTO_RETRY		0x00000002
#define	DWC3_SOC_F_KEEP_DUAL_ROLE_PHYS_ACTIVE	0x00000004

struct dwc3_soc_context {
	device_t		dev;
	bus_space_tag_t		bst;
	bus_space_handle_t	bsh;
	uint64_t		ref_clk_rate;
};

struct dwc3_soc_ops {
	bool		(*match)(device_t);
	int		(*enable_power)(device_t);
	void		(*pipe_setup)(const struct dwc3_soc_context *);
	int		(*configure_core)(const struct dwc3_soc_context *);
	uint32_t	flags;
	uint8_t		usb_bus_quirks;
};

#define	DWC3_SOC_REGISTER(name, ops)	DATA_SET(dwc3_soc_set, ops)

const struct dwc3_soc_ops *dwc3_soc_find(device_t);

#endif /* _DEV_USB_CONTROLLER_DWC3_SOC_H_ */
