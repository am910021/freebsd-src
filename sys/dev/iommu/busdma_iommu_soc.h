/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 */

#ifndef _DEV_IOMMU_BUSDMA_IOMMU_SOC_H_
#define	_DEV_IOMMU_BUSDMA_IOMMU_SOC_H_

struct iommu_busdma_soc_ops {
	bool	(*is_dma_policy_owner)(device_t owner);
	bool	(*retain_map_enabled)(device_t owner);
	bool	(*lazy_unload_enabled)(device_t owner);
	bool	(*iova_cache_enabled)(device_t owner);
	u_int	(*iova_cache_max)(device_t owner);
	bool	(*iova_fq_enabled)(device_t owner);
	u_int	(*iova_fq_batch)(device_t owner);
};

void	iommu_busdma_soc_register(const struct iommu_busdma_soc_ops *ops);
bool	iommu_busdma_soc_is_dma_policy_owner(device_t owner);
bool	iommu_busdma_soc_retain_map_enabled(device_t owner);
bool	iommu_busdma_soc_lazy_unload_enabled(device_t owner);
bool	iommu_busdma_soc_iova_cache_enabled(device_t owner);
u_int	iommu_busdma_soc_iova_cache_max(device_t owner);
bool	iommu_busdma_soc_iova_fq_enabled(device_t owner);
u_int	iommu_busdma_soc_iova_fq_batch(device_t owner);

#endif /* _DEV_IOMMU_BUSDMA_IOMMU_SOC_H_ */
