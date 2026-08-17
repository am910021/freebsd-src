/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 */

#ifndef _ARM64_ROCKCHIP_RK3588_PCI_DW_H_
#define	_ARM64_ROCKCHIP_RK3588_PCI_DW_H_

int	pci_dw_rk3588_pre_link_setup(device_t);
int	pci_dw_rk3588_post_link_setup(device_t);
int	pci_dw_rk3588_setup_inbound(device_t);

#endif /* _ARM64_ROCKCHIP_RK3588_PCI_DW_H_ */
