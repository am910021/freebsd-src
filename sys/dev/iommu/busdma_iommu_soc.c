/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 */

#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/bus.h>
#include <sys/systm.h>

#include <dev/iommu/busdma_iommu_soc.h>

static const struct iommu_busdma_soc_ops *iommu_busdma_soc_ops;

void
iommu_busdma_soc_register(const struct iommu_busdma_soc_ops *ops)
{

	iommu_busdma_soc_ops = ops;
}

bool
iommu_busdma_soc_is_dma_policy_owner(device_t owner)
{

	return (iommu_busdma_soc_ops != NULL &&
	    iommu_busdma_soc_ops->is_dma_policy_owner != NULL &&
	    iommu_busdma_soc_ops->is_dma_policy_owner(owner));
}

bool
iommu_busdma_soc_retain_map_enabled(device_t owner)
{

	return (iommu_busdma_soc_ops != NULL &&
	    iommu_busdma_soc_ops->retain_map_enabled != NULL &&
	    iommu_busdma_soc_ops->retain_map_enabled(owner));
}

bool
iommu_busdma_soc_lazy_unload_enabled(device_t owner)
{

	return (iommu_busdma_soc_ops != NULL &&
	    iommu_busdma_soc_ops->lazy_unload_enabled != NULL &&
	    iommu_busdma_soc_ops->lazy_unload_enabled(owner));
}

bool
iommu_busdma_soc_iova_cache_enabled(device_t owner)
{

	return (iommu_busdma_soc_ops != NULL &&
	    iommu_busdma_soc_ops->iova_cache_enabled != NULL &&
	    iommu_busdma_soc_ops->iova_cache_enabled(owner));
}

u_int
iommu_busdma_soc_iova_cache_max(device_t owner)
{

	if (iommu_busdma_soc_ops == NULL ||
	    iommu_busdma_soc_ops->iova_cache_max == NULL)
		return (0);
	return (iommu_busdma_soc_ops->iova_cache_max(owner));
}

bool
iommu_busdma_soc_iova_fq_enabled(device_t owner)
{

	return (iommu_busdma_soc_ops != NULL &&
	    iommu_busdma_soc_ops->iova_fq_enabled != NULL &&
	    iommu_busdma_soc_ops->iova_fq_enabled(owner));
}

u_int
iommu_busdma_soc_iova_fq_batch(device_t owner)
{

	if (iommu_busdma_soc_ops == NULL ||
	    iommu_busdma_soc_ops->iova_fq_batch == NULL)
		return (0);
	return (iommu_busdma_soc_ops->iova_fq_batch(owner));
}
