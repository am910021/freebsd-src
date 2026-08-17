/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 */

#ifndef _ARM64_ARM64_GICV3_ITS_SOC_H_
#define	_ARM64_ARM64_GICV3_ITS_SOC_H_

#include <sys/types.h>
#include <sys/bus.h>
#include <sys/cpuset.h>

#define	GICV3_ITS_SOC_FLAG_CMDQ_FLUSH		0x00000001
#define	GICV3_ITS_SOC_FLAG_LPI_CONF_FLUSH	0x00000002
#define	GICV3_ITS_SOC_FLAG_ERRATA_CAVIUM_22375	0x00000004
#define	GICV3_ITS_SOC_FLAG_LPI_PREALLOC		0x00000008
#define	GICV3_ITS_SOC_FLAG_FORCE_CACHE_FLUSH	0x00000010
#define	GICV3_ITS_SOC_FLAG_FORCE_NOSHAREABLE	0x00000020

struct gicv3_its_soc_ops {
	bool	(*match)(device_t dev);
	void	(*apply_quirks)(device_t dev, u_int *flags);
	bool	(*select_cpus)(device_t dev, const cpuset_t *available,
	    cpuset_t *selected);
};

void	gicv3_its_soc_register(const struct gicv3_its_soc_ops *ops);
void	gicv3_its_soc_apply_quirks(device_t dev, u_int *flags);
void	gicv3_its_soc_select_cpus(device_t dev, const cpuset_t *available,
	    cpuset_t *selected);

#endif /* _ARM64_ARM64_GICV3_ITS_SOC_H_ */
