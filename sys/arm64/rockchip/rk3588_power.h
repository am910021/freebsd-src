/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 *
 * Minimal RK3588 power-domain helpers.
 */

#ifndef _ARM64_ROCKCHIP_RK3588_POWER_H_
#define	_ARM64_ROCKCHIP_RK3588_POWER_H_

#include <sys/bus.h>
#include <sys/types.h>

#include <dev/ofw/openfirm.h>

/*
 * Keep the Linux DT binding numbers for local consumers.  FreeBSD does not
 * yet have a generic OF power-domain framework on this path.
 */
#define	RK3588_PD_CPU_0		0
#define	RK3588_PD_CPU_1		1
#define	RK3588_PD_CPU_2		2
#define	RK3588_PD_CPU_3		3
#define	RK3588_PD_CPU_4		4
#define	RK3588_PD_CPU_5		5
#define	RK3588_PD_CPU_6		6
#define	RK3588_PD_CPU_7		7
#define	RK3588_PD_NPU		8
#define	RK3588_PD_NPUTOP	9
#define	RK3588_PD_NPU1		10
#define	RK3588_PD_NPU2		11
#define	RK3588_PD_GPU		12
#define	RK3588_PD_VCODEC	13
#define	RK3588_PD_RKVDEC0	14
#define	RK3588_PD_RKVDEC1	15
#define	RK3588_PD_VENC0		16
#define	RK3588_PD_VENC1		17
#define	RK3588_PD_DDR01		18
#define	RK3588_PD_DDR23		19
#define	RK3588_PD_CENTER	20
#define	RK3588_PD_VDPU		21
#define	RK3588_PD_RGA30		22
#define	RK3588_PD_AV1		23
#define	RK3588_PD_VOP		24
#define	RK3588_PD_VO0		25
#define	RK3588_PD_VO1		26
#define	RK3588_PD_VI		27
#define	RK3588_PD_ISP1		28
#define	RK3588_PD_FEC		29
#define	RK3588_PD_RGA31		30
#define	RK3588_PD_USB		31
#define	RK3588_PD_PHP		32
#define	RK3588_PD_GMAC		33
#define	RK3588_PD_PCIE		34
#define	RK3588_PD_NVM		35
#define	RK3588_PD_NVM0		36
#define	RK3588_PD_SDIO		37
#define	RK3588_PD_AUDIO		38
#define	RK3588_PD_SECURE	39
#define	RK3588_PD_SDMMC		40
#define	RK3588_PD_CRYPTO	41
#define	RK3588_PD_BUS		42
#define	RK3588_PD_PMU1		43

int	rk3588_power_domain_enable(device_t consumer, uint32_t domain_id);
int	rk3588_power_domain_enable_by_node(device_t consumer, phandle_t node);

#endif /* _ARM64_ROCKCHIP_RK3588_POWER_H_ */
