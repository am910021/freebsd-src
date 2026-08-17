/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 */

#include "opt_platform.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/efi.h>
#include <sys/kernel.h>
#include <sys/physmem.h>

#include <vm/vm.h>

#include <machine/metadata.h>

#include <dev/fdt/fdt_common.h>
#include <dev/ofw/openfirm.h>

#include <arm64/arm64/machdep_soc.h>

#define	RK3588_DIAG_VERSION	"V366"

static struct mem_region rk3588_fdt_highmem_regions[FDT_MEM_REGIONS];
static int rk3588_fdt_highmem_regions_sz;

typedef void (*rk3588_efi_map_entry_cb)(struct efi_md *, void *argp);

static bool
rk3588_efi_map_entry_is_ram(struct efi_md *p)
{
	switch (p->md_type) {
	case EFI_MD_TYPE_RECLAIM:
	case EFI_MD_TYPE_RT_CODE:
	case EFI_MD_TYPE_RT_DATA:
	case EFI_MD_TYPE_CODE:
	case EFI_MD_TYPE_DATA:
	case EFI_MD_TYPE_BS_CODE:
	case EFI_MD_TYPE_BS_DATA:
	case EFI_MD_TYPE_FREE:
		return (true);
	default:
		return (false);
	}
}

static void
rk3588_foreach_efi_map_entry(struct efi_map_header *efihdr,
    rk3588_efi_map_entry_cb cb, void *argp)
{
	struct efi_md *map, *p;
	size_t efisz;
	int i, ndesc;

	efisz = (sizeof(struct efi_map_header) + 0xf) & ~0xf;
	map = (struct efi_md *)((uint8_t *)efihdr + efisz);

	if (efihdr->descriptor_size == 0)
		return;
	ndesc = efihdr->memory_size / efihdr->descriptor_size;

	for (i = 0, p = map; i < ndesc; i++,
	    p = efi_next_descriptor(p, efihdr->descriptor_size))
		cb(p, argp);
}

struct rk3588_efi_ram_info {
	uint64_t	max_end;
	uint64_t	total;
};

static void
rk3588_efi_ram_info_cb(struct efi_md *p, void *argp)
{
	struct rk3588_efi_ram_info *info;
	uint64_t end, size;

	if (!rk3588_efi_map_entry_is_ram(p))
		return;
	info = argp;
	size = p->md_pages * EFI_PAGE_SIZE;
	end = p->md_phys + size;
	if (end > info->max_end)
		info->max_end = end;
	info->total += size;
}

static bool
rk3588_root_has_compat(const char *needle)
{
	char compat[256], *end, *p;
	phandle_t root;
	int len;

	root = OF_peer(0);
	if (root == 0)
		return (false);
	len = OF_getprop(root, "compatible", compat, sizeof(compat) - 1);
	if (len <= 0)
		return (false);
	if (len >= (int)sizeof(compat))
		len = sizeof(compat) - 1;
	compat[len] = '\0';
	end = compat + len;
	for (p = compat; p < end; p += strlen(p) + 1) {
		if (*p == '\0')
			continue;
		if (strcmp(p, needle) == 0)
			return (true);
	}
	return (false);
}

static bool
rk3588_machdep_match(void)
{

	return (rk3588_root_has_compat("rockchip,rk3588"));
}

static bool
rk3588_machdep_memory_contains(vm_paddr_t pa)
{
	struct mem_region *mr;
	int i;

	for (i = 0; i < rk3588_fdt_highmem_regions_sz; i++) {
		mr = &rk3588_fdt_highmem_regions[i];
		if (pa >= mr->mr_start && pa < mr->mr_start + mr->mr_size)
			return (true);
	}
	return (false);
}

static void
rk3588_machdep_efi_map_fixup(struct efi_map_header *efihdr)
{
	struct rk3588_efi_ram_info info;
	struct mem_region mem_regions[FDT_MEM_REGIONS];
	uint64_t end, start;
	int enable, i, mem_regions_sz;

	enable = 1;
	TUNABLE_INT_FETCH("hw.rk3588.efi_fdt_highmem", &enable);
	if (enable == 0) {
		printf("RK3588-DIAG " RK3588_DIAG_VERSION
		    " EFI/FDT highmem supplement disabled by tunable\n");
		return;
	}

	memset(&info, 0, sizeof(info));
	rk3588_foreach_efi_map_entry(efihdr, rk3588_efi_ram_info_cb, &info);
	printf("RK3588-DIAG " RK3588_DIAG_VERSION
	    " EFI RAM total=%juMB max_end=%#jx\n",
	    (uintmax_t)(info.total / 1024 / 1024),
	    (uintmax_t)info.max_end);

	if (fdt_get_mem_regions(mem_regions, &mem_regions_sz, NULL) != 0) {
		printf("RK3588-DIAG " RK3588_DIAG_VERSION
		    " cannot read FDT memory banks\n");
		return;
	}

	for (i = 0; i < mem_regions_sz; i++) {
		start = mem_regions[i].mr_start;
		end = start + mem_regions[i].mr_size;
		printf("RK3588-DIAG " RK3588_DIAG_VERSION
		    " FDT RAM bank%d=%#jx-%#jx (%juMB)\n", i,
		    (uintmax_t)start, (uintmax_t)(end - 1),
		    (uintmax_t)(mem_regions[i].mr_size / 1024 / 1024));
	}

	if (info.max_end >= 0x100000000ULL) {
		printf("RK3588-DIAG " RK3588_DIAG_VERSION
		    " EFI already reports RAM above 4GB, no FDT supplement\n");
		return;
	}

	for (i = 0; i < mem_regions_sz; i++) {
		start = mem_regions[i].mr_start;
		end = start + mem_regions[i].mr_size;
		if (end <= 0x100000000ULL)
			continue;
		if (start < 0x100000000ULL)
			start = 0x100000000ULL;
		if (rk3588_fdt_highmem_regions_sz >= FDT_MEM_REGIONS)
			break;
		rk3588_fdt_highmem_regions[rk3588_fdt_highmem_regions_sz].
		    mr_start = start;
		rk3588_fdt_highmem_regions[rk3588_fdt_highmem_regions_sz].
		    mr_size = end - start;
		rk3588_fdt_highmem_regions_sz++;
		physmem_hardware_region(start, end - start);
		printf("RK3588-DIAG " RK3588_DIAG_VERSION
		    " add FDT highmem RAM %#jx-%#jx (%juMB)\n",
		    (uintmax_t)start, (uintmax_t)(end - 1),
		    (uintmax_t)((end - start) / 1024 / 1024));
	}
}

static const struct arm64_machdep_soc_ops rk3588_machdep_soc_ops = {
	.match = rk3588_machdep_match,
	.efi_map_fixup = rk3588_machdep_efi_map_fixup,
	.memory_contains = rk3588_machdep_memory_contains,
};

ARM64_MACHDEP_SOC_REGISTER(rk3588, rk3588_machdep_soc_ops);
