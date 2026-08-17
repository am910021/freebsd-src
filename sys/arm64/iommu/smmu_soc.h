/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 */

#ifndef	_ARM64_IOMMU_SMMU_SOC_H_
#define	_ARM64_IOMMU_SMMU_SOC_H_

#include <sys/bus.h>
#include <sys/types.h>

struct smmu_soc_ops {
	int	(*dma_memattr_policy)(device_t, int);
	void	(*sync_timeout)(device_t, uint32_t, uint32_t, uint32_t,
		    uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
		    uint32_t, uint32_t);
	bool	(*event_record)(device_t, uint8_t, u_int, uintptr_t);
	bool	(*range_tlbi_enabled)(device_t, uint32_t);
	bool	(*unmap_asid_tlbi)(device_t, uintmax_t, uint32_t);
	bool	(*suppress_map_tlbi)(device_t);
};

void	smmu_soc_register(const struct smmu_soc_ops *);
int	smmu_soc_dma_memattr_policy(device_t, int);
void	smmu_soc_sync_timeout(device_t, uint32_t, uint32_t, uint32_t,
	    uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
	    uint32_t);
bool	smmu_soc_event_record(device_t, uint8_t, u_int, uintptr_t);
bool	smmu_soc_range_tlbi_enabled(device_t, uint32_t);
bool	smmu_soc_unmap_asid_tlbi(device_t, uintmax_t, uint32_t);
bool	smmu_soc_suppress_map_tlbi(device_t);

#endif /* _ARM64_IOMMU_SMMU_SOC_H_ */
