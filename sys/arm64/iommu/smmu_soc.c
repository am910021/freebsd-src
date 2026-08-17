/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 */

#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/bus.h>
#include <sys/systm.h>

#include <arm64/iommu/smmu_soc.h>

static const struct smmu_soc_ops *smmu_soc_ops;

void
smmu_soc_register(const struct smmu_soc_ops *ops)
{

	smmu_soc_ops = ops;
}

int
smmu_soc_dma_memattr_policy(device_t dev, int device_policy)
{

	if (smmu_soc_ops == NULL || smmu_soc_ops->dma_memattr_policy == NULL)
		return (device_policy);
	return (smmu_soc_ops->dma_memattr_policy(dev, device_policy));
}

void
smmu_soc_sync_timeout(device_t dev, uint32_t target, uint32_t prod,
    uint32_t cons, uint32_t hwprod, uint32_t hwcons, uint32_t cerr,
    uint32_t cr0, uint32_t cr0ack, uint32_t gerror, uint32_t gerrorn)
{

	if (smmu_soc_ops == NULL || smmu_soc_ops->sync_timeout == NULL)
		return;
	smmu_soc_ops->sync_timeout(dev, target, prod, cons, hwprod, hwcons,
	    cerr, cr0, cr0ack, gerror, gerrorn);
}

bool
smmu_soc_event_record(device_t dev, uint8_t event_id, u_int sid,
    uintptr_t input_addr)
{

	if (smmu_soc_ops == NULL || smmu_soc_ops->event_record == NULL)
		return (true);
	return (smmu_soc_ops->event_record(dev, event_id, sid, input_addr));
}

bool
smmu_soc_range_tlbi_enabled(device_t dev, uint32_t features)
{

	if (smmu_soc_ops == NULL || smmu_soc_ops->range_tlbi_enabled == NULL)
		return (true);
	return (smmu_soc_ops->range_tlbi_enabled(dev, features));
}

bool
smmu_soc_unmap_asid_tlbi(device_t dev, uintmax_t pages, uint32_t features)
{

	if (smmu_soc_ops == NULL || smmu_soc_ops->unmap_asid_tlbi == NULL)
		return (false);
	return (smmu_soc_ops->unmap_asid_tlbi(dev, pages, features));
}

bool
smmu_soc_suppress_map_tlbi(device_t dev)
{

	if (smmu_soc_ops == NULL || smmu_soc_ops->suppress_map_tlbi == NULL)
		return (false);
	return (smmu_soc_ops->suppress_map_tlbi(dev));
}
