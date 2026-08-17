/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/cpuset.h>
#include <sys/kernel.h>

#include <arm64/arm64/gicv3_its_soc.h>

static const struct gicv3_its_soc_ops *gicv3_its_soc_ops;

static bool
gicv3_its_soc_matches(device_t dev)
{

	if (gicv3_its_soc_ops == NULL || gicv3_its_soc_ops->match == NULL)
		return (false);

	return (gicv3_its_soc_ops->match(dev));
}

void
gicv3_its_soc_register(const struct gicv3_its_soc_ops *ops)
{

	gicv3_its_soc_ops = ops;
}

void
gicv3_its_soc_apply_quirks(device_t dev, u_int *flags)
{

	if (gicv3_its_soc_matches(dev) &&
	    gicv3_its_soc_ops->apply_quirks != NULL)
		gicv3_its_soc_ops->apply_quirks(dev, flags);
}

void
gicv3_its_soc_select_cpus(device_t dev, const cpuset_t *available,
    cpuset_t *selected)
{

	CPU_COPY(available, selected);
	if (gicv3_its_soc_matches(dev) &&
	    gicv3_its_soc_ops->select_cpus != NULL &&
	    gicv3_its_soc_ops->select_cpus(dev, available, selected))
		return;
}
