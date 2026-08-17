/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2013 The FreeBSD Foundation
 *
 * This software was developed by Konstantin Belousov <kib@FreeBSD.org>
 * under sponsorship from the FreeBSD Foundation.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/domainset.h>
#include <sys/malloc.h>
#include <sys/bus.h>
#include <sys/conf.h>
#include <sys/interrupt.h>
#include <sys/kernel.h>
#include <sys/ktr.h>
#include <sys/lock.h>
#include <sys/proc.h>
#include <sys/memdesc.h>
#include <sys/msan.h>
#include <sys/mutex.h>
#include <sys/sysctl.h>
#include <sys/rman.h>
#include <sys/taskqueue.h>
#include <sys/tree.h>
#include <sys/uio.h>
#include <sys/vmem.h>
#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>
#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_kern.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_map.h>
#include <dev/iommu/iommu.h>
#include <machine/atomic.h>
#include <machine/bus.h>
#if defined(__aarch64__)
#include <machine/cpufunc.h>
#endif
#include <machine/md_var.h>
#include <machine/iommu.h>
#include <dev/iommu/busdma_iommu.h>
#include <dev/iommu/busdma_iommu_soc.h>

/*
 * busdma_iommu.c, the implementation of the busdma(9) interface using
 * IOMMU units from Intel VT-d.
 */

static int
iommu_busdma_count_entries(struct iommu_map_entries_tailq *entries,
    struct iommu_map_entry **firstp)
{
	struct iommu_map_entry *entry;
	int count;

	count = 0;
	TAILQ_FOREACH(entry, entries, dmamap_link) {
		if (count == 0 && firstp != NULL)
			*firstp = entry;
		count++;
	}
	return (count);
}

static bool
iommu_bus_dma_is_dev_disabled(int domain, int bus, int slot, int func)
{
	char str[128], *env;
	int default_bounce;
	bool ret;
	static const char bounce_str[] = "bounce";
	static const char iommu_str[] = "iommu";
	static const char dmar_str[] = "dmar"; /* compatibility */

	default_bounce = 0;
	env = kern_getenv("hw.busdma.default");
	if (env != NULL) {
		if (strcmp(env, bounce_str) == 0)
			default_bounce = 1;
		else if (strcmp(env, iommu_str) == 0 ||
		    strcmp(env, dmar_str) == 0)
			default_bounce = 0;
		freeenv(env);
	}

	snprintf(str, sizeof(str), "hw.busdma.pci%d.%d.%d.%d",
	    domain, bus, slot, func);
	env = kern_getenv(str);
	if (env == NULL)
		return (default_bounce != 0);
	if (strcmp(env, bounce_str) == 0)
		ret = true;
	else if (strcmp(env, iommu_str) == 0 ||
	    strcmp(env, dmar_str) == 0)
		ret = false;
	else
		ret = default_bounce != 0;
	freeenv(env);
	return (ret);
}

/*
 * Given original device, find the requester ID that will be seen by
 * the IOMMU unit and used for page table lookup.  PCI bridges may take
 * ownership of transactions from downstream devices, so it may not be
 * the same as the BSF of the target device.  In those cases, all
 * devices downstream of the bridge must share a single mapping
 * domain, and must collectively be assigned to use either IOMMU or
 * bounce mapping.
 */
device_t
iommu_get_requester(device_t dev, uint16_t *rid)
{
	devclass_t pci_class;
	device_t l, pci, pcib, pcip, pcibp, requester;
	int cap_offset;
	uint16_t pcie_flags;
	bool bridge_is_pcie;

	pci_class = devclass_find("pci");
	l = requester = dev;

	pci = device_get_parent(dev);
	if (pci == NULL || device_get_devclass(pci) != pci_class) {
		*rid = 0;	/* XXXKIB: Could be ACPI HID */
		return (requester);
	}

	*rid = pci_get_rid(dev);

	/*
	 * Walk the bridge hierarchy from the target device to the
	 * host port to find the translating bridge nearest the IOMMU
	 * unit.
	 */
	for (;;) {
		pci = device_get_parent(l);
		KASSERT(pci != NULL, ("iommu_get_requester(%s): NULL parent "
		    "for %s", device_get_name(dev), device_get_name(l)));
		KASSERT(device_get_devclass(pci) == pci_class,
		    ("iommu_get_requester(%s): non-pci parent %s for %s",
		    device_get_name(dev), device_get_name(pci),
		    device_get_name(l)));

		pcib = device_get_parent(pci);
		KASSERT(pcib != NULL, ("iommu_get_requester(%s): NULL bridge "
		    "for %s", device_get_name(dev), device_get_name(pci)));

		/*
		 * The parent of our "bridge" isn't another PCI bus,
		 * so pcib isn't a PCI->PCI bridge but rather a host
		 * port, and the requester ID won't be translated
		 * further.
		 */
		pcip = device_get_parent(pcib);
		if (device_get_devclass(pcip) != pci_class)
			break;
		pcibp = device_get_parent(pcip);

		if (pci_find_cap(l, PCIY_EXPRESS, &cap_offset) == 0) {
			/*
			 * Do not stop the loop even if the target
			 * device is PCIe, because it is possible (but
			 * unlikely) to have a PCI->PCIe bridge
			 * somewhere in the hierarchy.
			 */
			l = pcib;
		} else {
			/*
			 * Device is not PCIe, it cannot be seen as a
			 * requester by IOMMU unit.  Check whether the
			 * bridge is PCIe.
			 */
			bridge_is_pcie = pci_find_cap(pcib, PCIY_EXPRESS,
			    &cap_offset) == 0;
			requester = pcib;

			/*
			 * Check for a buggy PCIe/PCI bridge that
			 * doesn't report the express capability.  If
			 * the bridge above it is express but isn't a
			 * PCI bridge, then we know pcib is actually a
			 * PCIe/PCI bridge.
			 */
			if (!bridge_is_pcie && pci_find_cap(pcibp,
			    PCIY_EXPRESS, &cap_offset) == 0) {
				pcie_flags = pci_read_config(pcibp,
				    cap_offset + PCIER_FLAGS, 2);
				if ((pcie_flags & PCIEM_FLAGS_TYPE) !=
				    PCIEM_TYPE_PCI_BRIDGE)
					bridge_is_pcie = true;
			}

			if (bridge_is_pcie) {
				/*
				 * The current device is not PCIe, but
				 * the bridge above it is.  This is a
				 * PCIe->PCI bridge.  Assume that the
				 * requester ID will be the secondary
				 * bus number with slot and function
				 * set to zero.
				 *
				 * XXX: Doesn't handle the case where
				 * the bridge is PCIe->PCI-X, and the
				 * bridge will only take ownership of
				 * requests in some cases.  We should
				 * provide context entries with the
				 * same page tables for taken and
				 * non-taken transactions.
				 */
				*rid = PCI_RID(pci_get_bus(l), 0, 0);
				l = pcibp;
			} else {
				/*
				 * Neither the device nor the bridge
				 * above it are PCIe.  This is a
				 * conventional PCI->PCI bridge, which
				 * will use the bridge's BSF as the
				 * requester ID.
				 */
				*rid = pci_get_rid(pcib);
				l = pcib;
			}
		}
	}
	return (requester);
}

struct iommu_ctx *
iommu_instantiate_ctx(struct iommu_unit *unit, device_t dev, bool rmrr)
{
	device_t requester;
	struct iommu_ctx *ctx;
	bool disabled;
	uint16_t rid;

	requester = iommu_get_requester(dev, &rid);

	/*
	 * If the user requested the IOMMU disabled for the device, we
	 * cannot disable the IOMMU unit, due to possibility of other
	 * devices on the same IOMMU unit still requiring translation.
	 * Instead provide the identity mapping for the device
	 * context.
	 */
	disabled = iommu_bus_dma_is_dev_disabled(pci_get_domain(requester),
	    pci_get_bus(requester), pci_get_slot(requester), 
	    pci_get_function(requester));
	ctx = iommu_get_ctx(unit, requester, rid, disabled, rmrr);
	if (ctx == NULL)
		return (NULL);
	if (disabled) {
		/*
		 * Keep the first reference on context, release the
		 * later refs.
		 */
		IOMMU_LOCK(unit);
		if ((ctx->flags & IOMMU_CTX_DISABLED) == 0) {
			ctx->flags |= IOMMU_CTX_DISABLED;
			IOMMU_UNLOCK(unit);
		} else {
			iommu_free_ctx_locked(unit, ctx);
		}
		ctx = NULL;
	}
	return (ctx);
}

struct iommu_ctx *
iommu_get_dev_ctx(device_t dev)
{
	struct iommu_unit *unit;

	unit = iommu_find(dev, bootverbose);
	/* Not in scope of any IOMMU ? */
	if (unit == NULL)
		return (NULL);
	if (!unit->dma_enabled)
		return (NULL);

	iommu_unit_pre_instantiate_ctx(unit);
	return (iommu_instantiate_ctx(unit, dev, false));
}

bus_dma_tag_t
iommu_get_dma_tag(device_t dev, device_t child)
{
	struct iommu_ctx *ctx;
	bus_dma_tag_t res;

	ctx = iommu_get_dev_ctx(child);
	if (ctx == NULL)
		return (NULL);

	res = (bus_dma_tag_t)ctx->tag;
	return (res);
}

bool
bus_dma_iommu_set_buswide(device_t dev)
{
	struct iommu_unit *unit;
	device_t parent;
	u_int busno, slot, func;

	parent = device_get_parent(dev);
	if (device_get_devclass(parent) != devclass_find("pci"))
		return (false);
	unit = iommu_find(dev, bootverbose);
	if (unit == NULL)
		return (false);
	busno = pci_get_bus(dev);
	slot = pci_get_slot(dev);
	func = pci_get_function(dev);
	if (slot != 0 || func != 0) {
		if (bootverbose) {
			device_printf(dev,
			    "iommu%d pci%d:%d:%d requested buswide busdma\n",
			    unit->unit, busno, slot, func);
		}
		return (false);
	}
	iommu_set_buswide_ctx(unit, busno);
	return (true);
}

void
iommu_set_buswide_ctx(struct iommu_unit *unit, u_int busno)
{

	MPASS(busno <= PCI_BUSMAX);
	IOMMU_LOCK(unit);
	unit->buswide_ctxs[busno / NBBY / sizeof(uint32_t)] |=
	    1 << (busno % (NBBY * sizeof(uint32_t)));
	IOMMU_UNLOCK(unit);
}

bool
iommu_is_buswide_ctx(struct iommu_unit *unit, u_int busno)
{

	MPASS(busno <= PCI_BUSMAX);
	return ((unit->buswide_ctxs[busno / NBBY / sizeof(uint32_t)] &
	    (1U << (busno % (NBBY * sizeof(uint32_t))))) != 0);
}

static MALLOC_DEFINE(M_IOMMU_DMAMAP, "iommu_dmamap", "IOMMU DMA Map");

static void iommu_bus_schedule_dmamap(struct iommu_unit *unit,
    struct bus_dmamap_iommu *map);

static int
iommu_bus_dma_tag_create(bus_dma_tag_t parent, bus_size_t alignment,
    bus_addr_t boundary, bus_addr_t lowaddr, bus_addr_t highaddr,
    bus_dma_filter_t *filter, void *filterarg, bus_size_t maxsize,
    int nsegments, bus_size_t maxsegsz, int flags, bus_dma_lock_t *lockfunc,
    void *lockfuncarg, bus_dma_tag_t *dmat)
{
	struct bus_dma_tag_iommu *newtag, *oldtag;
	int error;

	*dmat = NULL;
	error = common_bus_dma_tag_create(parent != NULL ?
	    &((struct bus_dma_tag_iommu *)parent)->common : NULL, alignment,
	    boundary, lowaddr, highaddr, filter, filterarg, maxsize,
	    nsegments, maxsegsz, flags, lockfunc, lockfuncarg,
	    sizeof(struct bus_dma_tag_iommu), (void **)&newtag);
	if (error != 0)
		goto out;

	oldtag = (struct bus_dma_tag_iommu *)parent;
	newtag->common.impl = &bus_dma_iommu_impl;
	newtag->ctx = oldtag->ctx;
	newtag->owner = oldtag->owner;
	newtag->dma_policy_owner =
	    iommu_busdma_soc_is_dma_policy_owner(newtag->owner);

	*dmat = (bus_dma_tag_t)newtag;
out:
	CTR4(KTR_BUSDMA, "%s returned tag %p tag flags 0x%x error %d",
	    __func__, newtag, (newtag != NULL ? newtag->common.flags : 0),
	    error);
	return (error);
}

static int
iommu_bus_dma_tag_set_domain(bus_dma_tag_t dmat)
{

	return (0);
}

static int
iommu_bus_dma_tag_destroy(bus_dma_tag_t dmat1)
{
	struct bus_dma_tag_iommu *dmat, *parent;
	struct bus_dma_tag_iommu *dmat_copy __unused;
	struct iommu_unit *iommu;
	struct iommu_ctx *ctx;
	int error;

	error = 0;
	dmat_copy = dmat = (struct bus_dma_tag_iommu *)dmat1;

	if (dmat != NULL) {
		if (dmat->map_count != 0) {
			error = EBUSY;
			goto out;
		}
		while (dmat != NULL) {
			parent = (struct bus_dma_tag_iommu *)dmat->common.parent;
			if (atomic_fetchadd_int(&dmat->common.ref_count, -1) ==
			    1) {
				ctx = dmat->ctx;
				if (dmat == ctx->tag) {
					iommu = ctx->domain->iommu;
					IOMMU_LOCK(iommu);
					iommu_free_ctx_locked(iommu, dmat->ctx);
				}
				free(dmat->segments, M_IOMMU_DMAMAP);
				free(dmat, M_DEVBUF);
				dmat = parent;
			} else
				dmat = NULL;
		}
	}
out:
	CTR3(KTR_BUSDMA, "%s tag %p error %d", __func__, dmat_copy, error);
	return (error);
}

static bool
iommu_bus_dma_id_mapped(bus_dma_tag_t dmat, vm_paddr_t buf, bus_size_t buflen)
{

	return (false);
}

static int
iommu_bus_dmamap_create(bus_dma_tag_t dmat, int flags, bus_dmamap_t *mapp)
{
	struct bus_dma_tag_iommu *tag;
	struct bus_dmamap_iommu *map;
	int sync_nsegs;

	tag = (struct bus_dma_tag_iommu *)dmat;
	map = malloc_domainset(sizeof(*map), M_IOMMU_DMAMAP,
	    DOMAINSET_PREF(tag->common.domain), M_NOWAIT | M_ZERO);
	if (map == NULL) {
		*mapp = NULL;
		return (ENOMEM);
	}
	if (tag->segments == NULL) {
		tag->segments = malloc_domainset(sizeof(bus_dma_segment_t) *
		    tag->common.nsegments, M_IOMMU_DMAMAP,
		    DOMAINSET_PREF(tag->common.domain), M_NOWAIT);
		if (tag->segments == NULL) {
			free(map, M_IOMMU_DMAMAP);
			*mapp = NULL;
			return (ENOMEM);
		}
	}
	sync_nsegs = howmany(tag->common.maxsize, PAGE_SIZE) + 1;
	if (sync_nsegs < tag->common.nsegments)
		sync_nsegs = tag->common.nsegments;
	if (sync_nsegs > 256)
		sync_nsegs = 256;
	map->sync_segs = malloc_domainset(sizeof(bus_dma_segment_t) *
	    sync_nsegs, M_IOMMU_DMAMAP,
	    DOMAINSET_PREF(tag->common.domain), M_NOWAIT | M_ZERO);
	if (map->sync_segs == NULL) {
		free(map, M_IOMMU_DMAMAP);
		*mapp = NULL;
		return (ENOMEM);
	}
	map->sync_maxsegs = sync_nsegs;
	IOMMU_DMAMAP_INIT(map);
	TAILQ_INIT(&map->map_entries);
	map->tag = tag;
	if ((tag->common.flags & BUS_DMA_COHERENT) != 0)
		map->flags |= BUS_DMAMAP_IOMMU_COHERENT;
	map->locked = true;
	map->cansleep = false;
	tag->map_count++;
	*mapp = (bus_dmamap_t)map;

	return (0);
}

static int
iommu_bus_dmamap_destroy(bus_dma_tag_t dmat, bus_dmamap_t map1)
{
	struct bus_dma_tag_iommu *tag;
	struct bus_dmamap_iommu *map;
	struct iommu_map_entries_tailq entries;

	tag = (struct bus_dma_tag_iommu *)dmat;
	map = (struct bus_dmamap_iommu *)map1;
	if (map != NULL) {
		TAILQ_INIT(&entries);
		IOMMU_DMAMAP_LOCK(map);
		if ((map->flags & BUS_DMAMAP_IOMMU_RETAINED) != 0) {
			TAILQ_CONCAT(&entries, &map->map_entries,
			    dmamap_link);
			map->flags &= ~BUS_DMAMAP_IOMMU_RETAINED;
		}
		if (!TAILQ_EMPTY(&map->map_entries)) {
			IOMMU_DMAMAP_UNLOCK(map);
			return (EBUSY);
		}
		IOMMU_DMAMAP_UNLOCK(map);
		if (!TAILQ_EMPTY(&entries)) {
			THREAD_NO_SLEEPING();
			iommu_domain_unload(tag->ctx->domain, &entries,
			    false);
			THREAD_SLEEPING_OK();
		}
		IOMMU_DMAMAP_DESTROY(map);
		free(map->sync_segs, M_IOMMU_DMAMAP);
		free(map, M_IOMMU_DMAMAP);
	}
	tag->map_count--;
	return (0);
}


static int
iommu_bus_dmamem_alloc(bus_dma_tag_t dmat, void** vaddr, int flags,
    bus_dmamap_t *mapp)
{
	struct bus_dma_tag_iommu *tag;
	struct bus_dmamap_iommu *map;
	int error, mflags;
	vm_memattr_t attr;

	tag = (struct bus_dma_tag_iommu *)dmat;
	error = iommu_bus_dmamap_create(dmat, flags, mapp);
	if (error != 0)
		return (error);

	mflags = (flags & BUS_DMA_NOWAIT) != 0 ? M_NOWAIT : M_WAITOK;
	mflags |= (flags & BUS_DMA_ZERO) != 0 ? M_ZERO : 0;
	attr = (flags & (BUS_DMA_NOCACHE | BUS_DMA_COHERENT)) != 0 ?
	    VM_MEMATTR_UNCACHEABLE :
	    VM_MEMATTR_DEFAULT;

	map = (struct bus_dmamap_iommu *)*mapp;
	if (attr == VM_MEMATTR_UNCACHEABLE)
		map->flags |= BUS_DMAMAP_IOMMU_COHERENT;

	if (tag->common.maxsize < PAGE_SIZE &&
	    tag->common.alignment <= tag->common.maxsize &&
	    attr == VM_MEMATTR_DEFAULT) {
		*vaddr = malloc_domainset(tag->common.maxsize, M_DEVBUF,
		    DOMAINSET_PREF(tag->common.domain), mflags);
		map->flags |= BUS_DMAMAP_IOMMU_MALLOC;
	} else {
		*vaddr = kmem_alloc_attr_domainset(
		    DOMAINSET_PREF(tag->common.domain), tag->common.maxsize,
		    mflags, 0ul, BUS_SPACE_MAXADDR, attr);
		map->flags |= BUS_DMAMAP_IOMMU_KMEM_ALLOC;
	}
	if (*vaddr == NULL) {
		iommu_bus_dmamap_destroy(dmat, *mapp);
		*mapp = NULL;
		return (ENOMEM);
	}
	return (0);
}

static void
iommu_bus_dmamem_free(bus_dma_tag_t dmat, void *vaddr, bus_dmamap_t map1)
{
	struct bus_dma_tag_iommu *tag;
	struct bus_dmamap_iommu *map;

	tag = (struct bus_dma_tag_iommu *)dmat;
	map = (struct bus_dmamap_iommu *)map1;

	if ((map->flags & BUS_DMAMAP_IOMMU_MALLOC) != 0) {
		free(vaddr, M_DEVBUF);
		map->flags &= ~BUS_DMAMAP_IOMMU_MALLOC;
	} else {
		KASSERT((map->flags & BUS_DMAMAP_IOMMU_KMEM_ALLOC) != 0,
		    ("iommu_bus_dmamem_free for non alloced map %p", map));
		kmem_free(vaddr, tag->common.maxsize);
		map->flags &= ~BUS_DMAMAP_IOMMU_KMEM_ALLOC;
	}

	iommu_bus_dmamap_destroy(dmat, map1);
}

static int
iommu_busdma_iova_cache_bucket(bus_size_t mapsize)
{
	u_long pages;

	pages = atop(mapsize);
	if (pages == 0)
		return (-1);
	if (pages >= IOMMU_IOVA_CACHE_BUCKETS)
		return (IOMMU_IOVA_CACHE_BUCKETS - 1);
	return ((int)pages);
}

static bool
iommu_busdma_iova_entry_ok(struct bus_dma_tag_iommu *tag,
    struct iommu_map_entry *entry, int offset, bus_size_t buflen)
{

	return (entry->end <= tag->common.lowaddr ||
	    entry->start >= tag->common.highaddr) &&
	    vm_addr_align_ok(entry->start + offset, tag->common.alignment) &&
	    vm_addr_bound_ok(entry->start + offset, buflen,
	    tag->common.boundary);
}

static void
iommu_busdma_iova_cache_flush_pending(struct iommu_domain *domain)
{
	struct iommu_map_entries_tailq pending;
	struct iommu_map_entry *entry, *entry1;
	int bucket, i;

	if (domain->ops->sync == NULL)
		return;

	TAILQ_INIT(&pending);
	IOMMU_DOMAIN_LOCK(domain);
	if (domain->iova_pending_count == 0) {
		IOMMU_DOMAIN_UNLOCK(domain);
		return;
	}
	for (i = 0; i < IOMMU_IOVA_CACHE_BUCKETS; i++) {
		TAILQ_CONCAT(&pending,
		    &domain->iova_pending[i], dmamap_link);
	}
	domain->iova_pending_count = 0;
	IOMMU_DOMAIN_UNLOCK(domain);

	domain->ops->sync(domain);

	IOMMU_DOMAIN_LOCK(domain);
	TAILQ_FOREACH_SAFE(entry, &pending, dmamap_link, entry1) {
		TAILQ_REMOVE(&pending, entry, dmamap_link);
		bucket = iommu_busdma_iova_cache_bucket(entry->end -
		    entry->start);
		KASSERT(bucket >= 0,
		    ("invalid IOVA cache bucket for %p", entry));
		TAILQ_INSERT_TAIL(&domain->iova_cache[bucket],
		    entry, dmamap_link);
			domain->iova_cache_count++;
	}
	IOMMU_DOMAIN_UNLOCK(domain);
}

static int
iommu_busdma_iova_cache_pop(struct bus_dma_tag_iommu *tag,
    struct iommu_domain *domain, vm_page_t *ma, int offset, bus_size_t buflen,
    u_int eflags, u_int gas_flags, struct iommu_map_entry **res)
{
	struct iommu_map_entry *entry;
	bus_size_t mapsize;
	int bucket, error __diagused;
	bool flushed;

	*res = NULL;
	if (!iommu_busdma_soc_iova_cache_enabled(tag->owner))
		return (ENOENT);

	mapsize = roundup2(buflen + offset, IOMMU_PAGE_SIZE);
	bucket = iommu_busdma_iova_cache_bucket(mapsize);
	if (bucket < 0)
		return (ENOENT);

	flushed = false;
retry:
	IOMMU_DOMAIN_LOCK(domain);
	TAILQ_FOREACH(entry, &domain->iova_cache[bucket],
	    dmamap_link) {
		if (entry->end - entry->start == mapsize &&
		    iommu_busdma_iova_entry_ok(tag, entry, offset, buflen)) {
			TAILQ_REMOVE(&domain->iova_cache[bucket],
			    entry, dmamap_link);
			domain->iova_cache_count--;
			IOMMU_DOMAIN_UNLOCK(domain);
			entry->flags = IOMMU_MAP_ENTRY_MAP | eflags;
			error = domain->ops->map(domain, entry, ma, eflags,
			    ((gas_flags & IOMMU_MF_CANWAIT) != 0 ?
			    IOMMU_PGF_WAITOK : 0));
			if (error != 0) {
				iommu_domain_unload_entry(entry, true,
				    (gas_flags & IOMMU_MF_CANWAIT) != 0);
				return (error);
			}
			*res = entry;
			return (0);
		}
	}
	if (!flushed && domain->iova_pending_count != 0) {
		IOMMU_DOMAIN_UNLOCK(domain);
		flushed = true;
		iommu_busdma_iova_cache_flush_pending(domain);
		goto retry;
	}
	IOMMU_DOMAIN_UNLOCK(domain);
	return (ENOENT);
}

static bool
iommu_busdma_iova_cache_put(struct bus_dma_tag_iommu *tag,
    struct iommu_domain *domain, struct iommu_map_entry *entry)
{
	bus_size_t mapsize;
	int bucket, error __diagused;
	bool do_flush;

	if (!iommu_busdma_soc_iova_cache_enabled(tag->owner))
		return (false);

	mapsize = entry->end - entry->start;
	bucket = iommu_busdma_iova_cache_bucket(mapsize);
	if (bucket < 0)
		return (false);

	IOMMU_DOMAIN_LOCK(domain);
	if (domain->iova_cache_count +
	    domain->iova_pending_count >=
	    iommu_busdma_soc_iova_cache_max(tag->owner)) {
		IOMMU_DOMAIN_UNLOCK(domain);
		return (false);
	}
	IOMMU_DOMAIN_UNLOCK(domain);

	if (iommu_busdma_soc_iova_fq_enabled(tag->owner) &&
	    domain->ops->unmap_nosync != NULL && domain->ops->sync != NULL) {
		error = domain->ops->unmap_nosync(domain, entry, 0);
		KASSERT(error == 0, ("unmap_nosync %p error %d", domain,
		    error));
		IOMMU_DOMAIN_LOCK(domain);
			TAILQ_INSERT_TAIL(&domain->iova_pending[bucket],
			    entry, dmamap_link);
			domain->iova_pending_count++;
			do_flush = iommu_busdma_soc_iova_fq_batch(
			    tag->owner) > 0 &&
			    domain->iova_pending_count >=
			    iommu_busdma_soc_iova_fq_batch(tag->owner);
		IOMMU_DOMAIN_UNLOCK(domain);
		if (do_flush)
			iommu_busdma_iova_cache_flush_pending(domain);
		return (true);
	}

	error = domain->ops->unmap(domain, entry, 0);
	KASSERT(error == 0, ("unmap %p error %d", domain, error));

	IOMMU_DOMAIN_LOCK(domain);
	TAILQ_INSERT_TAIL(&domain->iova_cache[bucket], entry,
	    dmamap_link);
	domain->iova_cache_count++;
	IOMMU_DOMAIN_UNLOCK(domain);
	return (true);
}

static int
iommu_bus_dmamap_load_something1(struct bus_dma_tag_iommu *tag,
    struct bus_dmamap_iommu *map, vm_page_t *ma, int offset, bus_size_t buflen,
    int flags, bus_dma_segment_t *segs, int *segp,
    struct iommu_map_entries_tailq *entries)
{
	struct iommu_ctx *ctx;
	struct iommu_domain *domain;
	struct iommu_map_entry *entry;
	bus_size_t buflen1;
	int error, e_flags, idx, gas_flags, seg;

	KASSERT(offset < IOMMU_PAGE_SIZE, ("offset %d", offset));
	if (segs == NULL)
		segs = tag->segments;
	ctx = tag->ctx;
	domain = ctx->domain;
	e_flags = IOMMU_MAP_ENTRY_READ |
	    ((flags & BUS_DMA_NOWRITE) == 0 ? IOMMU_MAP_ENTRY_WRITE : 0);
	seg = *segp;
	error = 0;
	idx = 0;
	while (buflen > 0) {
		seg++;
		if (seg >= tag->common.nsegments) {
			error = EFBIG;
			break;
		}
		buflen1 = buflen > tag->common.maxsegsz ?
		    tag->common.maxsegsz : buflen;

		/*
		 * (Too) optimistically allow split if there are more
		 * then one segments left.
		 */
		gas_flags = map->cansleep ? IOMMU_MF_CANWAIT : 0;
		if (seg + 1 < tag->common.nsegments)
			gas_flags |= IOMMU_MF_CANSPLIT;

		error = iommu_busdma_iova_cache_pop(tag, domain, ma + idx,
		    offset, buflen1, e_flags, gas_flags, &entry);
		if (error == ENOENT)
			error = iommu_gas_map(domain, &tag->common, buflen1,
			    offset, e_flags, gas_flags, ma + idx, &entry);
		if (error != 0)
			break;
		/* Update buflen1 in case buffer split. */
		if (buflen1 > entry->end - entry->start - offset)
			buflen1 = entry->end - entry->start - offset;

		KASSERT(vm_addr_align_ok(entry->start + offset,
		    tag->common.alignment),
		    ("alignment failed: ctx %p start 0x%jx offset %x "
		    "align 0x%jx", ctx, (uintmax_t)entry->start, offset,
		    (uintmax_t)tag->common.alignment));
		KASSERT(entry->end <= tag->common.lowaddr ||
		    entry->start >= tag->common.highaddr,
		    ("entry placement failed: ctx %p start 0x%jx end 0x%jx "
		    "lowaddr 0x%jx highaddr 0x%jx", ctx,
		    (uintmax_t)entry->start, (uintmax_t)entry->end,
		    (uintmax_t)tag->common.lowaddr,
		    (uintmax_t)tag->common.highaddr));
		KASSERT(vm_addr_bound_ok(entry->start + offset, buflen1,
		    tag->common.boundary),
		    ("boundary failed: ctx %p start 0x%jx end 0x%jx "
		    "boundary 0x%jx", ctx, (uintmax_t)entry->start,
		    (uintmax_t)entry->end, (uintmax_t)tag->common.boundary));
		KASSERT(buflen1 <= tag->common.maxsegsz,
		    ("segment too large: ctx %p start 0x%jx end 0x%jx "
		    "buflen1 0x%jx maxsegsz 0x%jx", ctx,
		    (uintmax_t)entry->start, (uintmax_t)entry->end,
		    (uintmax_t)buflen1, (uintmax_t)tag->common.maxsegsz));

		KASSERT((entry->flags & IOMMU_MAP_ENTRY_MAP) != 0,
		    ("entry %p missing IOMMU_MAP_ENTRY_MAP", entry));
		TAILQ_INSERT_TAIL(entries, entry, dmamap_link);

		segs[seg].ds_addr = entry->start + offset;
		segs[seg].ds_len = buflen1;

		idx += OFF_TO_IDX(offset + buflen1);
		offset += buflen1;
		offset &= IOMMU_PAGE_MASK;
		buflen -= buflen1;
	}
	if (error == 0)
		*segp = seg;
	return (error);
}

static int
iommu_bus_dmamap_reuse_retained(struct bus_dma_tag_iommu *tag,
    struct bus_dmamap_iommu *map, vm_page_t *ma, int offset, bus_size_t buflen,
    int flags, bus_dma_segment_t *segs, int *segp)
{
	struct iommu_ctx *ctx;
	struct iommu_domain *domain;
	struct iommu_map_entry *entry;
	bus_size_t buflen1, mapsize, rem;
	int e_flags, error, idx, nentries, offset1, seg, seg0;

	if (!iommu_busdma_soc_retain_map_enabled(tag->owner))
		return (EJUSTRETURN);
	if (segs == NULL)
		segs = tag->segments;

	entry = NULL;
	IOMMU_DMAMAP_LOCK(map);
	if ((map->flags & BUS_DMAMAP_IOMMU_RETAINED) == 0) {
		IOMMU_DMAMAP_UNLOCK(map);
		return (EJUSTRETURN);
	}
	nentries = iommu_busdma_count_entries(&map->map_entries, &entry);
	IOMMU_DMAMAP_UNLOCK(map);

	if (nentries != 1 || entry == NULL) {
		if (nentries <= 1)
			return (EJUSTRETURN);
	}

	seg0 = *segp;
	seg = seg0;
	rem = buflen;
	offset1 = offset;
	TAILQ_FOREACH(entry, &map->map_entries, dmamap_link) {
		seg++;
		if (seg >= tag->common.nsegments || rem == 0)
			return (EJUSTRETURN);
		mapsize = entry->end - entry->start;
		if ((bus_size_t)offset1 >= mapsize)
			return (EJUSTRETURN);
		buflen1 = MIN(rem, mapsize - offset1);
		if (buflen1 > tag->common.maxsegsz ||
		    roundup2(offset1 + buflen1, IOMMU_PAGE_SIZE) != mapsize ||
		    !vm_addr_align_ok(entry->start + offset1,
		    tag->common.alignment) ||
		    !vm_addr_bound_ok(entry->start + offset1, buflen1,
		    tag->common.boundary))
			return (EJUSTRETURN);
		rem -= buflen1;
		offset1 = 0;
	}
	if (rem != 0)
		return (EJUSTRETURN);

	ctx = tag->ctx;
	domain = ctx->domain;
	e_flags = IOMMU_MAP_ENTRY_READ |
	    ((flags & BUS_DMA_NOWRITE) == 0 ? IOMMU_MAP_ENTRY_WRITE : 0);

	seg = seg0;
	idx = 0;
	rem = buflen;
	offset1 = offset;
	TAILQ_FOREACH(entry, &map->map_entries, dmamap_link) {
		mapsize = entry->end - entry->start;
		buflen1 = MIN(rem, mapsize - offset1);
		error = domain->ops->unmap(domain, entry, 0);
		if (error == 0)
			error = domain->ops->map(domain, entry, ma + idx,
			    e_flags, 0);
		if (error != 0)
			return (error);
		seg++;
		segs[seg].ds_addr = entry->start + offset1;
		segs[seg].ds_len = buflen1;
		idx += OFF_TO_IDX(offset1 + buflen1);
		rem -= buflen1;
		offset1 = 0;
	}

	IOMMU_DMAMAP_LOCK(map);
	map->flags &= ~BUS_DMAMAP_IOMMU_RETAINED;
	IOMMU_DMAMAP_UNLOCK(map);
	*segp = seg;
	return (0);
}

static void
iommu_bus_dmamap_drop_retained(struct bus_dma_tag_iommu *tag,
    struct bus_dmamap_iommu *map)
{
	struct iommu_map_entries_tailq entries;

	TAILQ_INIT(&entries);
	IOMMU_DMAMAP_LOCK(map);
	if ((map->flags & BUS_DMAMAP_IOMMU_RETAINED) != 0) {
		TAILQ_CONCAT(&entries, &map->map_entries, dmamap_link);
		map->flags &= ~BUS_DMAMAP_IOMMU_RETAINED;
	}
	IOMMU_DMAMAP_UNLOCK(map);
	if (!TAILQ_EMPTY(&entries)) {
		THREAD_NO_SLEEPING();
		iommu_domain_unload(tag->ctx->domain, &entries, false);
		THREAD_SLEEPING_OK();
	}
}

static int
iommu_bus_dmamap_load_something(struct bus_dma_tag_iommu *tag,
    struct bus_dmamap_iommu *map, vm_page_t *ma, int offset, bus_size_t buflen,
    int flags, bus_dma_segment_t *segs, int *segp)
{
	struct iommu_ctx *ctx;
	struct iommu_domain *domain;
	struct iommu_map_entries_tailq entries;
	int error;

	ctx = tag->ctx;
	domain = ctx->domain;
	atomic_add_long(&ctx->loads, 1);

	TAILQ_INIT(&entries);
	error = iommu_bus_dmamap_reuse_retained(tag, map, ma, offset, buflen,
	    flags, segs, segp);
	if (error == EJUSTRETURN &&
	    iommu_busdma_soc_retain_map_enabled(tag->owner))
		iommu_bus_dmamap_drop_retained(tag, map);
	if (error != EJUSTRETURN)
		return (error);
	error = iommu_bus_dmamap_load_something1(tag, map, ma, offset,
	    buflen, flags, segs, segp, &entries);
	if (error == 0) {
		IOMMU_DMAMAP_LOCK(map);
		TAILQ_CONCAT(&map->map_entries, &entries, dmamap_link);
		IOMMU_DMAMAP_UNLOCK(map);
	} else if (!TAILQ_EMPTY(&entries)) {
		/*
		 * The busdma interface does not allow us to report
		 * partial buffer load, so unfortunately we have to
		 * revert all work done.
		 */
		IOMMU_DOMAIN_LOCK(domain);
		TAILQ_CONCAT(&domain->unload_entries, &entries, dmamap_link);
		IOMMU_DOMAIN_UNLOCK(domain);
		taskqueue_enqueue(domain->iommu->delayed_taskqueue,
		    &domain->unload_task);
	}

	if (error == ENOMEM && (flags & BUS_DMA_NOWAIT) == 0 &&
	    !map->cansleep)
		error = EINPROGRESS;
	if (error == EINPROGRESS)
		iommu_bus_schedule_dmamap(domain->iommu, map);
	return (error);
}

static void iommu_bus_dmamap_reset_sync(struct bus_dmamap_iommu *map);
static void iommu_bus_dmamap_save_sync_ma(struct bus_dmamap_iommu *map,
    vm_page_t *ma, int offset, bus_size_t buflen, int error);

static int
iommu_bus_dmamap_load_ma(bus_dma_tag_t dmat, bus_dmamap_t map1,
    struct vm_page **ma, bus_size_t tlen, int ma_offs, int flags,
    bus_dma_segment_t *segs, int *segp)
{
	struct bus_dma_tag_iommu *tag;
	struct bus_dmamap_iommu *map;
	int error;

	tag = (struct bus_dma_tag_iommu *)dmat;
	map = (struct bus_dmamap_iommu *)map1;
	if (*segp == -1)
		iommu_bus_dmamap_reset_sync(map);
	error = iommu_bus_dmamap_load_something(tag, map, ma, ma_offs, tlen,
	    flags, segs, segp);
	iommu_bus_dmamap_save_sync_ma(map, ma, ma_offs, tlen, error);
	return (error);
}

static void
iommu_bus_dmamap_reset_sync(struct bus_dmamap_iommu *map)
{

	map->mem.md_type = 0;
	map->sync_count = 0;
	map->sync_overflow = false;
}

static void
iommu_bus_dmamap_save_sync_seg(struct bus_dmamap_iommu *map, vm_paddr_t paddr,
    bus_size_t len)
{
	bus_dma_segment_t *seg;

	if (len == 0)
		return;
	if (!PHYS_IN_DMAP(paddr)) {
		map->sync_overflow = true;
		return;
	}
	if (map->sync_count > 0) {
		seg = &map->sync_segs[map->sync_count - 1];
		if (seg->ds_addr + seg->ds_len == paddr) {
			seg->ds_len += len;
			return;
		}
	}
	if (map->sync_count >= map->sync_maxsegs) {
		map->sync_overflow = true;
		return;
	}
	seg = &map->sync_segs[map->sync_count++];
	seg->ds_addr = paddr;
	seg->ds_len = len;
}

static void
iommu_bus_dmamap_save_sync_ma(struct bus_dmamap_iommu *map, vm_page_t *ma,
    int offset, bus_size_t buflen, int error)
{
	vm_paddr_t paddr;
	bus_size_t seglen;
	int i;

	if (error != 0) {
		iommu_bus_dmamap_reset_sync(map);
		return;
	}
	for (i = 0; buflen != 0; i++, offset = 0) {
		seglen = MIN(PAGE_SIZE - offset, buflen);
		paddr = VM_PAGE_TO_PHYS(ma[i]) + offset;
		iommu_bus_dmamap_save_sync_seg(map, paddr, seglen);
		buflen -= seglen;
	}
}

static void
iommu_bus_dmamap_save_vaddr(struct bus_dmamap_iommu *map, void *buf,
    bus_size_t buflen, pmap_t pmap, int error)
{

	if (error != 0) {
		map->mem.md_type = 0;
		return;
	}
	if (pmap != kernel_pmap) {
		map->mem.md_type = 0;
		return;
	}
	map->mem.u.md_vaddr = buf;
	map->mem.md_len = buflen;
	map->mem.md_offset = 0;
	map->mem.md_type = MEMDESC_VADDR;
}

static void
iommu_bus_dmamap_save_paddr(struct bus_dmamap_iommu *map, vm_paddr_t paddr,
    bus_size_t buflen, int error)
{

	if (error != 0) {
		map->mem.md_type = 0;
		return;
	}
	if (!PHYS_IN_DMAP(paddr)) {
		map->mem.md_type = 0;
		return;
	}
	map->mem.u.md_paddr = paddr;
	map->mem.md_len = buflen;
	map->mem.md_offset = 0;
	map->mem.md_type = MEMDESC_PADDR;
}

static int
iommu_bus_dmamap_load_phys(bus_dma_tag_t dmat, bus_dmamap_t map1,
    vm_paddr_t buf, bus_size_t buflen, int flags, bus_dma_segment_t *segs,
    int *segp)
{
	struct bus_dma_tag_iommu *tag;
	struct bus_dmamap_iommu *map;
	vm_page_t *ma, fma;
	vm_page_t ma_local[4];
	vm_paddr_t pstart, pend, paddr;
	int error, i, ma_cnt, mflags, offset;

	tag = (struct bus_dma_tag_iommu *)dmat;
	map = (struct bus_dmamap_iommu *)map1;
	if (*segp == -1)
		iommu_bus_dmamap_reset_sync(map);
	pstart = trunc_page(buf);
	pend = round_page(buf + buflen);
	offset = buf & PAGE_MASK;
	ma_cnt = OFF_TO_IDX(pend - pstart);
	mflags = map->cansleep ? M_WAITOK : M_NOWAIT;
	if (ma_cnt <= nitems(ma_local)) {
		ma = ma_local;
	} else {
		ma = malloc(sizeof(vm_page_t) * ma_cnt, M_DEVBUF, mflags);
		if (ma == NULL)
			return (ENOMEM);
	}
	fma = NULL;
	for (i = 0; i < ma_cnt; i++) {
		paddr = pstart + ptoa(i);
		ma[i] = PHYS_TO_VM_PAGE(paddr);
		if (ma[i] == NULL || VM_PAGE_TO_PHYS(ma[i]) != paddr) {
			/*
			 * If PHYS_TO_VM_PAGE() returned NULL or the
			 * vm_page was not initialized we'll use a
			 * fake page.
			 */
			if (fma == NULL) {
				fma = malloc(sizeof(struct vm_page) * ma_cnt,
				    M_DEVBUF, M_ZERO | mflags);
				if (fma == NULL) {
					if (ma != ma_local)
						free(ma, M_DEVBUF);
					return (ENOMEM);
				}
			}
			vm_page_initfake(&fma[i], pstart + ptoa(i),
			    VM_MEMATTR_DEFAULT);
			ma[i] = &fma[i];
		}
	}
	error = iommu_bus_dmamap_load_something(tag, map, ma, offset, buflen,
	    flags, segs, segp);
	iommu_bus_dmamap_save_sync_ma(map, ma, offset, buflen, error);
	iommu_bus_dmamap_save_paddr(map, buf, buflen, error);
	free(fma, M_DEVBUF);
	if (ma != ma_local)
		free(ma, M_DEVBUF);
	return (error);
}

static int
iommu_bus_dmamap_load_buffer(bus_dma_tag_t dmat, bus_dmamap_t map1, void *buf,
    bus_size_t buflen, pmap_t pmap, int flags, bus_dma_segment_t *segs,
    int *segp)
{
	struct bus_dma_tag_iommu *tag;
	struct bus_dmamap_iommu *map;
	vm_page_t *ma, fma;
	vm_page_t ma_local[4];
	vm_paddr_t pstart, pend, paddr;
	int error, i, ma_cnt, mflags, offset;

	tag = (struct bus_dma_tag_iommu *)dmat;
	map = (struct bus_dmamap_iommu *)map1;
	if (*segp == -1)
		iommu_bus_dmamap_reset_sync(map);
	pstart = trunc_page((vm_offset_t)buf);
	pend = round_page((vm_offset_t)buf + buflen);
	offset = (vm_offset_t)buf & PAGE_MASK;
	ma_cnt = OFF_TO_IDX(pend - pstart);
	mflags = map->cansleep ? M_WAITOK : M_NOWAIT;
	if (ma_cnt <= nitems(ma_local)) {
		ma = ma_local;
	} else {
		ma = malloc(sizeof(vm_page_t) * ma_cnt, M_DEVBUF, mflags);
		if (ma == NULL)
			return (ENOMEM);
	}
	fma = NULL;
	for (i = 0; i < ma_cnt; i++, pstart += PAGE_SIZE) {
		if (pmap == kernel_pmap)
			paddr = pmap_kextract(pstart);
		else
			paddr = pmap_extract(pmap, pstart);
		ma[i] = PHYS_TO_VM_PAGE(paddr);
		if (ma[i] == NULL || VM_PAGE_TO_PHYS(ma[i]) != paddr) {
			/*
			 * If PHYS_TO_VM_PAGE() returned NULL or the
			 * vm_page was not initialized we'll use a
			 * fake page.
			 */
			if (fma == NULL) {
				fma = malloc(sizeof(struct vm_page) * ma_cnt,
				    M_DEVBUF, M_ZERO | mflags);
				if (fma == NULL) {
					if (ma != ma_local)
						free(ma, M_DEVBUF);
					return (ENOMEM);
				}
			}
			vm_page_initfake(&fma[i], paddr, VM_MEMATTR_DEFAULT);
			ma[i] = &fma[i];
		}
	}
	error = iommu_bus_dmamap_load_something(tag, map, ma, offset, buflen,
	    flags, segs, segp);
	iommu_bus_dmamap_save_sync_ma(map, ma, offset, buflen, error);
	iommu_bus_dmamap_save_vaddr(map, buf, buflen, pmap, error);
	if (ma != ma_local)
		free(ma, M_DEVBUF);
	free(fma, M_DEVBUF);
	return (error);
}

static void
iommu_bus_dmamap_waitok(bus_dma_tag_t dmat, bus_dmamap_t map1,
    struct memdesc *mem, bus_dmamap_callback_t *callback, void *callback_arg)
{
	struct bus_dmamap_iommu *map;

	if (map1 == NULL)
		return;
	map = (struct bus_dmamap_iommu *)map1;
	map->mem = *mem;
	map->tag = (struct bus_dma_tag_iommu *)dmat;
	map->callback = callback;
	map->callback_arg = callback_arg;
}

static bus_dma_segment_t *
iommu_bus_dmamap_complete(bus_dma_tag_t dmat, bus_dmamap_t map1,
    bus_dma_segment_t *segs, int nsegs, int error)
{
	struct bus_dma_tag_iommu *tag;
	struct bus_dmamap_iommu *map;

	tag = (struct bus_dma_tag_iommu *)dmat;
	map = (struct bus_dmamap_iommu *)map1;

	if (!map->locked) {
		KASSERT(map->cansleep,
		    ("map not locked and not sleepable context %p", map));

		/*
		 * We are called from the delayed context.  Relock the
		 * driver.
		 */
		(tag->common.lockfunc)(tag->common.lockfuncarg, BUS_DMA_LOCK);
		map->locked = true;
	}

	if (segs == NULL)
		segs = tag->segments;
	return (segs);
}

/*
 * The limitations of busdma KPI forces the iommu to perform the actual
 * unload, consisting of the unmapping of the map entries page tables,
 * from the delayed context on i386, since page table page mapping
 * might require a sleep to be successfull.  The unfortunate
 * consequence is that the DMA requests can be served some time after
 * the bus_dmamap_unload() call returned.
 *
 * On amd64, we assume that sf allocation cannot fail.
 */
static void
iommu_bus_dmamap_unload(bus_dma_tag_t dmat, bus_dmamap_t map1)
{
	struct bus_dma_tag_iommu *tag;
	struct bus_dmamap_iommu *map;
	struct iommu_ctx *ctx;
	struct iommu_domain *domain;
	struct iommu_map_entries_tailq entries;
	int nentries;

	tag = (struct bus_dma_tag_iommu *)dmat;
	map = (struct bus_dmamap_iommu *)map1;
	ctx = tag->ctx;
	domain = ctx->domain;
	atomic_add_long(&ctx->unloads, 1);

	TAILQ_INIT(&entries);
	IOMMU_DMAMAP_LOCK(map);
	nentries = iommu_busdma_count_entries(&map->map_entries, NULL);
	if (iommu_busdma_soc_retain_map_enabled(tag->owner) && nentries >= 1) {
		map->flags |= BUS_DMAMAP_IOMMU_RETAINED;
		iommu_bus_dmamap_reset_sync(map);
		IOMMU_DMAMAP_UNLOCK(map);
		return;
	}
	TAILQ_CONCAT(&entries, &map->map_entries, dmamap_link);
	map->flags &= ~BUS_DMAMAP_IOMMU_RETAINED;
	iommu_bus_dmamap_reset_sync(map);
	IOMMU_DMAMAP_UNLOCK(map);
	if (iommu_busdma_soc_lazy_unload_enabled(tag->owner)) {
		IOMMU_DOMAIN_LOCK(domain);
		TAILQ_CONCAT(&domain->unload_entries, &entries, dmamap_link);
		IOMMU_DOMAIN_UNLOCK(domain);
		taskqueue_enqueue(domain->iommu->delayed_taskqueue,
		    &domain->unload_task);
		return;
	}
	if (iommu_busdma_soc_iova_cache_enabled(tag->owner)) {
		struct iommu_map_entries_tailq drop_entries;
		struct iommu_map_entry *entry, *entry1;

		TAILQ_INIT(&drop_entries);
		TAILQ_FOREACH_SAFE(entry, &entries, dmamap_link, entry1) {
			TAILQ_REMOVE(&entries, entry, dmamap_link);
			if (!iommu_busdma_iova_cache_put(tag, domain, entry))
				TAILQ_INSERT_TAIL(&drop_entries, entry,
				    dmamap_link);
		}
		TAILQ_CONCAT(&entries, &drop_entries, dmamap_link);
		if (TAILQ_EMPTY(&entries))
			return;
	}
#if defined(IOMMU_DOMAIN_UNLOAD_SLEEP)
	IOMMU_DOMAIN_LOCK(domain);
	TAILQ_CONCAT(&domain->unload_entries, &entries, dmamap_link);
	IOMMU_DOMAIN_UNLOCK(domain);
	taskqueue_enqueue(domain->iommu->delayed_taskqueue,
	    &domain->unload_task);
#else
	THREAD_NO_SLEEPING();
	iommu_domain_unload(domain, &entries, false);
	THREAD_SLEEPING_OK();
	KASSERT(TAILQ_EMPTY(&entries), ("lazy iommu_ctx_unload %p", ctx));
#endif
}

#if defined(__aarch64__)
static void
iommu_bus_dmamap_sync_vaddr(vm_offset_t va, vm_size_t size,
    bus_dmasync_op_t op)
{

	if (size == 0)
		return;

	switch (op) {
	case BUS_DMASYNC_PREWRITE:
	case BUS_DMASYNC_PREWRITE | BUS_DMASYNC_PREREAD:
		cpu_dcache_wb_range(va, size);
		break;
	case BUS_DMASYNC_PREREAD:
		if ((va & (dcache_line_size - 1)) != 0)
			cpu_dcache_wb_range(va, 1);
		if (((va + size) & (dcache_line_size - 1)) != 0)
			cpu_dcache_wb_range(va + size, 1);
		cpu_dcache_inv_range(va, size);
		break;
	case BUS_DMASYNC_POSTREAD:
	case BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE:
		cpu_dcache_inv_range(va, size);
		break;
	case BUS_DMASYNC_POSTWRITE:
		break;
	default:
		break;
	}
}

static void
iommu_bus_dmamap_sync_segments(struct bus_dmamap_iommu *map,
    bus_dmasync_op_t op)
{
	bus_dma_segment_t *seg;
	int i;

	if ((op & BUS_DMASYNC_POSTREAD) != 0)
		dsb(sy);
	for (i = 0; i < map->sync_count; i++) {
		seg = &map->sync_segs[i];
		iommu_bus_dmamap_sync_vaddr(PHYS_TO_DMAP(seg->ds_addr),
		    seg->ds_len, op);
	}
	if ((op & (BUS_DMASYNC_PREREAD | BUS_DMASYNC_PREWRITE)) != 0)
		dsb(sy);
}

static void
iommu_bus_dmamap_sync_memdesc(struct memdesc *mem, bus_dmasync_op_t op)
{
	struct bus_dma_segment *seg;
	struct iovec *iov;
	struct uio *uio;
	vm_offset_t va;
	vm_size_t len;
	u_int offset;
	int i;

	if ((op & BUS_DMASYNC_POSTREAD) != 0)
		dsb(sy);

	switch (mem->md_type) {
	case MEMDESC_VADDR:
		iommu_bus_dmamap_sync_vaddr((vm_offset_t)mem->u.md_vaddr,
		    mem->md_len, op);
		break;
	case MEMDESC_PADDR:
		iommu_bus_dmamap_sync_vaddr(PHYS_TO_DMAP(mem->u.md_paddr),
		    mem->md_len, op);
		break;
	case MEMDESC_VLIST:
		for (i = 0, seg = mem->u.md_list; i < mem->md_nseg;
		    i++, seg++) {
			iommu_bus_dmamap_sync_vaddr((vm_offset_t)seg->ds_addr,
			    seg->ds_len, op);
		}
		break;
	case MEMDESC_UIO:
		uio = mem->u.md_uio;
		for (i = 0, iov = uio->uio_iov; i < uio->uio_iovcnt;
		    i++, iov++) {
			iommu_bus_dmamap_sync_vaddr((vm_offset_t)iov->iov_base,
			    iov->iov_len, op);
		}
		break;
	case MEMDESC_VMPAGES:
		offset = mem->md_offset;
		len = mem->md_len;
		for (i = 0; len != 0; i++, offset = 0) {
			vm_size_t seglen;

			seglen = MIN(PAGE_SIZE - offset, len);
			va = PHYS_TO_DMAP(VM_PAGE_TO_PHYS(mem->u.md_ma[i])) +
			    offset;
			iommu_bus_dmamap_sync_vaddr(va, seglen, op);
			len -= seglen;
		}
		break;
	default:
		break;
	}

	if ((op & (BUS_DMASYNC_PREREAD | BUS_DMASYNC_PREWRITE)) != 0)
		dsb(sy);
}
#endif

static void
iommu_bus_dmamap_sync(bus_dma_tag_t dmat, bus_dmamap_t map1,
    bus_dmasync_op_t op)
{
	struct bus_dmamap_iommu *map;

	map = (struct bus_dmamap_iommu *)map1;
#if defined(__aarch64__)
	if ((map->flags & BUS_DMAMAP_IOMMU_COHERENT) != 0) {
		if ((op & BUS_DMASYNC_POSTREAD) != 0)
			dsb(sy);
		if ((op & (BUS_DMASYNC_PREREAD | BUS_DMASYNC_PREWRITE)) != 0)
			dsb(sy);
	} else if (map->sync_overflow && map->mem.md_type != 0) {
		iommu_bus_dmamap_sync_memdesc(&map->mem, op);
	} else if (map->sync_count > 0) {
		iommu_bus_dmamap_sync_segments(map, op);
	} else if (map->mem.md_type != 0) {
		iommu_bus_dmamap_sync_memdesc(&map->mem, op);
	}
#endif
	kmsan_bus_dmamap_sync(&map->kmsan_mem, op);
}

#ifdef KMSAN
static void
iommu_bus_dmamap_load_kmsan(bus_dmamap_t map1, struct memdesc *mem)
{
	struct bus_dmamap_iommu *map;

	map = (struct bus_dmamap_iommu *)map1;
	if (map == NULL)
		return;
	memcpy(&map->kmsan_mem, mem, sizeof(struct memdesc));
}
#endif

struct bus_dma_impl bus_dma_iommu_impl = {
	.tag_create = iommu_bus_dma_tag_create,
	.tag_destroy = iommu_bus_dma_tag_destroy,
	.tag_set_domain = iommu_bus_dma_tag_set_domain,
	.id_mapped = iommu_bus_dma_id_mapped,
	.map_create = iommu_bus_dmamap_create,
	.map_destroy = iommu_bus_dmamap_destroy,
	.mem_alloc = iommu_bus_dmamem_alloc,
	.mem_free = iommu_bus_dmamem_free,
	.load_phys = iommu_bus_dmamap_load_phys,
	.load_buffer = iommu_bus_dmamap_load_buffer,
	.load_ma = iommu_bus_dmamap_load_ma,
	.map_waitok = iommu_bus_dmamap_waitok,
	.map_complete = iommu_bus_dmamap_complete,
	.map_unload = iommu_bus_dmamap_unload,
	.map_sync = iommu_bus_dmamap_sync,
#ifdef KMSAN
	.load_kmsan = iommu_bus_dmamap_load_kmsan,
#endif
};

static void
iommu_bus_task_dmamap(void *arg, int pending)
{
	struct bus_dma_tag_iommu *tag;
	struct bus_dmamap_iommu *map;
	struct iommu_unit *unit;

	unit = arg;
	IOMMU_LOCK(unit);
	while ((map = TAILQ_FIRST(&unit->delayed_maps)) != NULL) {
		TAILQ_REMOVE(&unit->delayed_maps, map, delay_link);
		IOMMU_UNLOCK(unit);
		tag = map->tag;
		map->cansleep = true;
		map->locked = false;
		bus_dmamap_load_mem((bus_dma_tag_t)tag, (bus_dmamap_t)map,
		    &map->mem, map->callback, map->callback_arg,
		    BUS_DMA_WAITOK);
		map->cansleep = false;
		if (map->locked) {
			(tag->common.lockfunc)(tag->common.lockfuncarg,
			    BUS_DMA_UNLOCK);
		} else
			map->locked = true;
		map->cansleep = false;
		IOMMU_LOCK(unit);
	}
	IOMMU_UNLOCK(unit);
}

static void
iommu_bus_schedule_dmamap(struct iommu_unit *unit, struct bus_dmamap_iommu *map)
{

	map->locked = false;
	IOMMU_LOCK(unit);
	TAILQ_INSERT_TAIL(&unit->delayed_maps, map, delay_link);
	IOMMU_UNLOCK(unit);
	taskqueue_enqueue(unit->delayed_taskqueue, &unit->dmamap_load_task);
}

int
iommu_init_busdma(struct iommu_unit *unit)
{
	int error;

	unit->dma_enabled = 1;
	error = TUNABLE_INT_FETCH("hw.iommu.dma", &unit->dma_enabled);
	if (error == 0) /* compatibility */
		TUNABLE_INT_FETCH("hw.dmar.dma", &unit->dma_enabled);
	SYSCTL_ADD_INT(&unit->sysctl_ctx,
	    SYSCTL_CHILDREN(device_get_sysctl_tree(unit->dev)),
	    OID_AUTO, "dma", CTLFLAG_RD, &unit->dma_enabled, 0,
	    "DMA ops enabled");
	TAILQ_INIT(&unit->delayed_maps);
	TASK_INIT(&unit->dmamap_load_task, 0, iommu_bus_task_dmamap, unit);
	unit->delayed_taskqueue = taskqueue_create("iommu", M_WAITOK,
	    taskqueue_thread_enqueue, &unit->delayed_taskqueue);
	taskqueue_start_threads(&unit->delayed_taskqueue, 1, PI_DISK,
	    "iommu%d busdma taskq", unit->unit);
	return (0);
}

void
iommu_fini_busdma(struct iommu_unit *unit)
{

	if (unit->delayed_taskqueue == NULL)
		return;

	taskqueue_drain(unit->delayed_taskqueue, &unit->dmamap_load_task);
	taskqueue_free(unit->delayed_taskqueue);
	unit->delayed_taskqueue = NULL;
}

int
bus_dma_iommu_load_ident(bus_dma_tag_t dmat, bus_dmamap_t map1,
    vm_paddr_t start, vm_size_t length, int flags)
{
	struct bus_dma_tag_common *tc;
	struct bus_dma_tag_iommu *tag;
	struct bus_dmamap_iommu *map;
	struct iommu_ctx *ctx;
	struct iommu_domain *domain;
	struct iommu_map_entry *entry;
	vm_page_t *ma;
	vm_size_t i;
	int error;
	bool waitok;

	MPASS((start & PAGE_MASK) == 0);
	MPASS((length & PAGE_MASK) == 0);
	MPASS(length > 0);
	MPASS(start + length >= start);
	MPASS((flags & ~(BUS_DMA_NOWAIT | BUS_DMA_NOWRITE)) == 0);

	tc = (struct bus_dma_tag_common *)dmat;
	if (tc->impl != &bus_dma_iommu_impl)
		return (0);

	tag = (struct bus_dma_tag_iommu *)dmat;
	ctx = tag->ctx;
	domain = ctx->domain;
	map = (struct bus_dmamap_iommu *)map1;
	waitok = (flags & BUS_DMA_NOWAIT) != 0;

	entry = iommu_gas_alloc_entry(domain, waitok ? 0 : IOMMU_PGF_WAITOK);
	if (entry == NULL)
		return (ENOMEM);
	entry->start = start;
	entry->end = start + length;
	ma = malloc(sizeof(vm_page_t) * atop(length), M_TEMP, waitok ?
	    M_WAITOK : M_NOWAIT);
	if (ma == NULL) {
		iommu_gas_free_entry(entry);
		return (ENOMEM);
	}
	for (i = 0; i < atop(length); i++) {
		ma[i] = vm_page_getfake(entry->start + PAGE_SIZE * i,
		    VM_MEMATTR_DEFAULT);
	}
	error = iommu_gas_map_region(domain, entry, IOMMU_MAP_ENTRY_READ |
	    ((flags & BUS_DMA_NOWRITE) ? 0 : IOMMU_MAP_ENTRY_WRITE) |
	    IOMMU_MAP_ENTRY_MAP, waitok ? IOMMU_MF_CANWAIT : 0, ma);
	if (error == 0) {
		IOMMU_DMAMAP_LOCK(map);
		TAILQ_INSERT_TAIL(&map->map_entries, entry, dmamap_link);
		IOMMU_DMAMAP_UNLOCK(map);
	} else {
		iommu_gas_free_entry(entry);
	}
	for (i = 0; i < atop(length); i++)
		vm_page_putfake(ma[i]);
	free(ma, M_TEMP);
	return (error);
}

static void
iommu_domain_unload_task(void *arg, int pending)
{
	struct iommu_domain *domain;
	struct iommu_map_entries_tailq entries;

	domain = arg;
	TAILQ_INIT(&entries);

	for (;;) {
		IOMMU_DOMAIN_LOCK(domain);
		TAILQ_SWAP(&domain->unload_entries, &entries,
		    iommu_map_entry, dmamap_link);
		IOMMU_DOMAIN_UNLOCK(domain);
		if (TAILQ_EMPTY(&entries))
			break;
		iommu_domain_unload(domain, &entries, true);
	}
}

void
iommu_domain_init(struct iommu_unit *unit, struct iommu_domain *domain,
    const struct iommu_domain_map_ops *ops)
{
	int i;

	domain->ops = ops;
	domain->iommu = unit;

	TASK_INIT(&domain->unload_task, 0, iommu_domain_unload_task, domain);
	RB_INIT(&domain->rb_root);
	TAILQ_INIT(&domain->unload_entries);
	for (i = 0; i < IOMMU_IOVA_CACHE_BUCKETS; i++) {
		TAILQ_INIT(&domain->iova_cache[i]);
		TAILQ_INIT(&domain->iova_pending[i]);
	}
	domain->iova_cache_count = 0;
	domain->iova_pending_count = 0;
	mtx_init(&domain->lock, "iodom", NULL, MTX_DEF);
}

void
iommu_domain_fini(struct iommu_domain *domain)
{
	struct iommu_map_entry *entry;
	int i;

	iommu_busdma_iova_cache_flush_pending(domain);

	for (i = 0; i < IOMMU_IOVA_CACHE_BUCKETS; i++) {
		while ((entry = TAILQ_FIRST(
		    &domain->iova_cache[i])) != NULL) {
			TAILQ_REMOVE(&domain->iova_cache[i],
			    entry, dmamap_link);
			iommu_gas_free_space(entry);
			iommu_gas_free_entry(entry);
		}
	}

	mtx_destroy(&domain->lock);
}
