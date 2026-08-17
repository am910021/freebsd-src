/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 *
 * ARM SMMU platform policy interface.
 */

#ifndef	_ARM64_IOMMU_SMMU_PLATFORM_H_
#define	_ARM64_IOMMU_SMMU_PLATFORM_H_

#define	SMMU_DMA_MEMATTR_AUTO		-2
#define	SMMU_DMA_MEMATTR_GENERIC		-1
#define	SMMU_DMA_MEMATTR_NORMAL_NC	0
#define	SMMU_DMA_MEMATTR_PRESERVE	1
#define	SMMU_DMA_MEMATTR_DEVICE		2

int smmu_fdt_platform_dma_memattr_policy(device_t dev);

#endif /* _ARM64_IOMMU_SMMU_PLATFORM_H_ */
