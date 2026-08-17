/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/fbio.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/memdesc.h>
#include <sys/module.h>
#include <sys/rman.h>
#include <sys/rwlock.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

#include <machine/bus.h>

#include <vm/vm.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_pager.h>

#include <dev/drm2/drmP.h>
#include <dev/drm2/drm_crtc_helper.h>
#include <dev/drm2/drm_edid.h>
#include <dev/drm2/drm_fourcc.h>
#include <dev/extres/clk/clk.h>
#include <dev/extres/syscon/syscon.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#include <dev/videomode/videomode.h>

#include "syscon_if.h"

#include "rk3588_power.h"
#include "rk3588_hdmi_mode.h"

#define RK3588_VOP2_VERSION		0x40176786u

#define VOP2_REG_CFG_DONE		0x000
#define  VOP2_GLB_CFG_DONE_EN		(1u << 15)
#define  VOP2_VP0_CFG_DONE		((1u << 16) | 1u)
#define VOP2_VERSION_INFO		0x004
#define VOP2_AUTO_GATING_CTRL		0x008
#define  VOP2_AUTO_GATING_EN		(1u << 31)
#define VOP2_SYS_WIN_REG_CFG_DONE	0x00c
#define VOP2_DSP_IF_EN			0x028
#define  VOP2_HDMI0_EN			(1u << 3)
#define  VOP2_HDMI0_MUX_MASK		(3u << 16)
#define VOP2_DSP_IF_CTRL			0x02c
#define  VOP2_HDMI0_DCLK_DIV_MASK	(3u << 16)
#define  VOP2_HDMI0_DCLK_DIV4		(2u << 16)
#define  VOP2_HDMI0_PCLK_DIV2		(1u << 18)
#define VOP2_DSP_IF_POL			0x030
#define  VOP2_HDMI_PIN_POL_MASK		(7u << 4)
#define  VOP2_HDMI_DCLK_POL		(1u << 7)
#define  VOP2_IF_CFG_DONE_IMMEDIATE	(1u << 28)
#define VOP2_SYS_PD_CTRL			0x034
#define  VOP2_PD_ENABLE_MASK		0x8fu
#define VOP2_SYS_STATUS0			0x060
#define  VOP2_PD_STATUS_MASK		0x8f00u
#define VOP2_SYS_STATUS1			0x064
#define VOP2_SYS_STATUS2			0x068
#define VOP2_SYS_STATUS3			0x06c
#define VOP2_SYS0_INT_STATUS		0x088
#define VOP2_SYS1_INT_STATUS		0x098
#define VOP2_VP0_INT_EN			0x0a0
#define VOP2_VP0_INT_CLR			0x0a4
#define VOP2_VP0_INT_STATUS		0x0a8
#define VOP2_VP0_INT_RAW_STATUS		0x0ac
#define  VOP2_VP0_FS			1u
#define  VOP2_VP0_FS_FIELD		(1u << 5)
#define  VOP2_VP0_INT_MASK		0xffffu
#define  VOP2_VP0_FS_CLEAR		((1u << 16) | VOP2_VP0_FS)
#define  VOP2_VP0_DSP_HOLD_VALID	(1u << 6)
#define  VOP2_VP0_DSP_HOLD_CLEAR	\
	    ((VOP2_VP0_DSP_HOLD_VALID << 16) | VOP2_VP0_DSP_HOLD_VALID)
#define VOP2_OVL_CTRL			0x600
#define  VOP2_OVL_VP0_YUV		1u
#define  VOP2_OVL_LAYERSEL_REGDONE_IMD	(1u << 28)
#define  VOP2_OVL_LAYERSEL_REGDONE_MASK	(3u << 30)
#define VOP2_OVL_LAYER_SEL		0x604
#define  VOP2_OVL_LAYER_SEL_LINUX	0x76543210u
#define VOP2_OVL_PORT_SEL		0x608
#define  VOP2_OVL_PORT_SEL_LINUX	0xa0507873u
#define VOP2_CLUSTER0_MIX_SRC_COLOR	0x610
#define  VOP2_CLUSTER0_MIX_SRC_COLOR_LINUX 0x000001a1u
#define VOP2_CLUSTER0_MIX_DST_COLOR	0x614
#define  VOP2_CLUSTER0_MIX_DST_COLOR_LINUX 0x00ff0061u
#define VOP2_CLUSTER0_MIX_SRC_ALPHA	0x618
#define  VOP2_CLUSTER0_MIX_SRC_ALPHA_LINUX 0x00000020u
#define VOP2_CLUSTER0_MIX_DST_ALPHA	0x61c
#define  VOP2_CLUSTER0_MIX_DST_ALPHA_LINUX 0x00000074u
#define VOP2_VP0_BG_MIX_CTRL		0x6e0
#define  VOP2_VP0_BG_DELAY_PIXELS	54u
#define  VOP2_VP0_BG_DELAY		(VOP2_VP0_BG_DELAY_PIXELS << 24)
#define VOP2_CLUSTER_DLY_NUM		0x6f0
#define  VOP2_CLUSTER_DLY_NUM_LINUX	0x00000404u
#define VOP2_SMART_DLY_NUM		0x6f8
#define  VOP2_SMART_DLY_NUM_LINUX	0x17170000u

#define VOP2_VP0_DSP_CTRL		0xc00
#define  VOP2_VP0_OUT_MODE_MASK		0xfu
#define  VOP2_VP0_OUT_MODE_RGB888	0u
#define  VOP2_VP0_OUT_MODE_AAAA		0xfu
#define  VOP2_VP0_STANDBY		(1u << 31)
#define VOP2_VP0_COLOR_BAR_CTRL		0xc08
#define  VOP2_VP0_COLOR_BAR_HORIZONTAL	1u
#define VOP2_VP0_CLK_CTRL		0xc0c
#define  VOP2_VP0_CLK_DIV_MASK		0xfu
#define  VOP2_VP0_DCLK_CORE_DIV4	2u
#define VOP2_VP0_DSP_BG			0xc2c
#define  VOP2_VP0_BG_LINUX		0u
#define VOP2_VP0_PRE_SCAN_HTIMING	0xc30
#define VOP2_VP0_POST_HACT		0xc34
#define VOP2_VP0_POST_VACT		0xc38
#define VOP2_VP0_POST_SCALE		0xc3c
#define VOP2_VP0_POST_SCALE_CTRL	0xc40
#define VOP2_VP0_HTOTAL_HSYNC		0xc48
#define VOP2_VP0_HACT			0xc4c
#define VOP2_VP0_VTOTAL_VSYNC		0xc50
#define VOP2_VP0_VACT			0xc54

#define VOP2_POST_SCALE_1TO1		0x10001000u

#define VOP2_CLUSTER0_WIN0_CTRL0		0x1000
#define  VOP2_CLUSTER0_WIN0_ENABLE	1u
#define VOP2_CLUSTER0_WIN0_CTRL1		0x1004
#define VOP2_CLUSTER0_WIN0_CTRL2		0x1008
#define  VOP2_CLUSTER0_WIN0_AXI_IDS	0x62u
#define VOP2_CLUSTER0_WIN0_YRGB_MST	0x1010
#define VOP2_CLUSTER0_WIN0_CBR_MST	0x1014
#define VOP2_CLUSTER0_WIN0_VIR		0x1018
#define VOP2_CLUSTER0_WIN0_ACT_INFO	0x1020
#define VOP2_CLUSTER0_WIN0_DSP_INFO	0x1024
#define VOP2_CLUSTER0_WIN0_DSP_ST	0x1028
#define VOP2_CLUSTER0_WIN0_SCL_FACTOR	0x1030
#define VOP2_CLUSTER0_WIN0_AFBCD_OUTPUT	0x1050
#define  VOP2_CLUSTER0_AFBCD_AUTOGATE	0x10u
#define VOP2_CLUSTER0_WIN0_AFBCD_CTRL	0x106c
#define  VOP2_CLUSTER0_AFBCD_HALF_BLOCK	0x80u
#define VOP2_CLUSTER0_WIN1_CTRL0		0x1080
#define VOP2_CLUSTER0_CTRL		0x1100
#define  VOP2_CLUSTER0_ENABLE		1u

#define VOP2_ESMART0_REGION0_CTRL	0x1810
#define VOP2_ESMART0_REGION1_CTRL	0x1840
#define VOP2_ESMART0_REGION2_CTRL	0x1870
#define VOP2_ESMART0_REGION3_CTRL	0x18a0

#define RK3588_VOP2_FB_BPP		4u
#define RK3588_VOP2_FB_MAX_WIDTH	3840u
#define RK3588_VOP2_FB_MAX_HEIGHT	2160u

#define RK3588_VOP_GRF_CON2		0x008
#define  RK3588_HDMITX0_ENABLE		(1u << 1)
#define RK3588_VO1_GRF_CON0		0x000
#define  RK3588_HDMI0_POL_MASK		(3u << 5)
#define RK3588_GRF_SOC_STATUS1		0x384
#define  RK3588_HDMI0_HPD		(1u << 16)

#define RK_MMU_DTE_ADDR			0x00
#define RK_MMU_STATUS			0x04
#define RK_MMU_PAGE_FAULT_ADDR		0x0c
#define RK_MMU_INT_RAWSTAT		0x14
#define RK_MMU_INT_MASK			0x1c
#define RK_MMU_INT_STATUS		0x20
#define RK3588_VOP2_MMU_BANKS		2

enum rk3588_vop2_clock {
	VOP2_CLK_ACLK,
	VOP2_CLK_HCLK,
	VOP2_CLK_DCLK,
	VOP2_CLK_PCLK,
	VOP2_CLK_PHY0,
	VOP2_CLK_COUNT
};

static const char * const rk3588_vop2_clock_names[VOP2_CLK_COUNT] = {
	"aclk", "hclk", "dclk_vp0", "pclk_vop", "pll_hdmiphy0"
};

static const uint64_t rk3588_vop2_clock_rates[VOP2_CLK_COUNT] = {
	500000000, 198000000, 0, 100000000, 0
};

static const uint32_t rk3588_vop2_window_enable_regs[] = {
	0x1000, 0x1200, 0x1400, 0x1600,
	0x1810, 0x1a10, 0x1c10, 0x1e10
};

struct rk3588_vop2_mmu_snapshot {
	uint32_t	dte;
	uint32_t	status;
	uint32_t	fault;
	uint32_t	raw;
	uint32_t	mask;
	uint32_t	irq;
};

struct rk3588_vop2_softc;

struct rk3588_vop2_bo {
	struct drm_gem_object gem_obj;
	struct rk3588_vop2_softc *sc;
	bus_dma_tag_t	tag;
	bus_dmamap_t	map;
	vm_page_t	*pages;
	vm_object_t	pager;
	bus_addr_t	dma_addr;
	size_t		npages;
	int		dma_error;
	bool		map_loaded;
};

struct rk3588_vop2_drm_fb {
	struct drm_framebuffer base;
	struct rk3588_vop2_bo *bo;
};

struct rk3588_vop2_softc {
	device_t	dev;
	struct resource	*mem;
	struct resource	*irq;
	void		*irq_cookie;
	struct syscon	*grf;
	struct syscon	*vop_grf;
	struct syscon	*vo1_grf;
	clk_t		clocks[VOP2_CLK_COUNT];
	struct intr_config_hook config_hook;
	bus_dma_tag_t	fb_tag;
	bus_dmamap_t	fb_map;
	void		*fb_vaddr;
	uint64_t	fb_addr;
	struct fb_info	fb_info;
	struct drm_device drm_dev;
	struct drm_crtc drm_crtc;
	struct drm_encoder drm_encoder;
	struct drm_connector drm_connector;
	struct drm_framebuffer drm_fb;
	struct videomode mode;
	uint64_t	clock_rate[VOP2_CLK_COUNT];
	uint64_t	tmds_rate;
	uint32_t	fb_width;
	uint32_t	fb_height;
	uint32_t	fb_stride;
	uint32_t	fb_size;
	uint32_t	fb_window_info;
	uint32_t	version;
	uint32_t	pd_before;
	uint32_t	pd_after;
	uint32_t	pd_status;
	uint32_t	cfg_before;
	uint32_t	cfg_after;
	uint32_t	if_en_before;
	uint32_t	if_en_after;
	uint32_t	if_ctrl_before;
	uint32_t	if_ctrl_after;
	uint32_t	if_pol_before;
	uint32_t	if_pol_after;
	uint32_t	dsp_ctrl_before;
	uint32_t	dsp_ctrl_after;
	uint32_t	bg_after;
	uint32_t	color_bar_after;
	uint32_t	ovl_ctrl_before;
	uint32_t	ovl_ctrl_after;
	uint32_t	layer_sel_before;
	uint32_t	layer_sel_after;
	uint32_t	port_sel_before;
	uint32_t	port_sel_after;
	uint32_t	cluster_dly_after;
	uint32_t	smart_dly_after;
	uint32_t	cluster_ctrl_after;
	uint32_t	cluster_ctrl2_after;
	uint32_t	cluster_mix_after[4];
	uint32_t	cluster_sub_ctrl_after;
	uint32_t	cluster_afbc_output_after;
	uint32_t	cluster_afbc_ctrl_after;
	uint32_t	window_ctrl_after;
	uint32_t	window_addr_after;
	uint32_t	window_vir_after;
	uint32_t	window_act_after;
	uint32_t	window_dsp_after;
	uint32_t	window_st_after;
	uint32_t	fs_status;
	uint32_t	win_cfg_done_after;
	uint32_t	sys_status[4];
	uint32_t	sys_int_status[2];
	uint32_t	vp_int_status;
	uint32_t	vop_grf_after;
	uint32_t	vo1_grf_after;
	struct rk3588_vop2_mmu_snapshot mmu_before[RK3588_VOP2_MMU_BANKS];
	struct rk3588_vop2_mmu_snapshot mmu_after[RK3588_VOP2_MMU_BANKS];
	int		iommu_before_error;
	int		iommu_after_error;
	int		fs_count;
	int		fs_polls;
	int		fb_nsegs;
	int		attempted;
	int		configured;
	int		clocks_ready;
	int		error;
	int		fb_error;
	int		drm_error;
	int		drm_inited;
	int		drm_modeset_count;
	int		drm_modeset_error;
	int		drm_dpms_count;
	int		drm_dpms_error;
	int		drm_crtc_dpms_active;
	int		drm_crtc_dclk_enabled;
	int		drm_core_clocks_enabled;
	int		drm_crtc_hold_polls;
	uint32_t	drm_crtc_hold_status;
	int		vblank_irq_count;
	uint32_t	vblank_irq_status;
	struct drm_pending_vblank_event *flip_event;
	int		page_flip_count;
	int		page_flip_cancel_count;
	int		page_flip_error;
	const char	*program_stage;
};

static struct rk3588_vop2_softc *rk3588_vop2_sc;

static const struct drm_framebuffer_funcs rk3588_vop2_drm_gem_fb_funcs;
static int rk3588_vop2_drm_set_scanout(struct rk3588_vop2_softc *,
    bus_addr_t, uint32_t, uint32_t, uint32_t, bus_dma_tag_t, bus_dmamap_t,
    bool);
static int rk3588_vop2_crtc_set_output(struct rk3588_vop2_softc *, bool);

static void
rk3588_vop2_vblank_enable(struct rk3588_vop2_softc *sc)
{

	bus_write_4(sc->mem, VOP2_VP0_INT_CLR,
	    (VOP2_VP0_FS_FIELD << 16) | VOP2_VP0_FS_FIELD);
	bus_write_4(sc->mem, VOP2_VP0_INT_EN,
	    (VOP2_VP0_FS_FIELD << 16) | VOP2_VP0_FS_FIELD);
	bus_barrier(sc->mem, VOP2_VP0_INT_EN, 16,
	    BUS_SPACE_BARRIER_READ | BUS_SPACE_BARRIER_WRITE);
}

static void
rk3588_vop2_vblank_disable(struct rk3588_vop2_softc *sc)
{

	bus_write_4(sc->mem, VOP2_VP0_INT_EN,
	    VOP2_VP0_FS_FIELD << 16);
	bus_barrier(sc->mem, VOP2_VP0_INT_EN, sizeof(uint32_t),
	    BUS_SPACE_BARRIER_READ | BUS_SPACE_BARRIER_WRITE);
}

static int
rk3588_vop2_drm_enable_vblank(struct drm_device *drm, int pipe)
{
	struct rk3588_vop2_softc *sc;

	if (pipe != 0)
		return (-EINVAL);
	sc = device_get_softc(drm->dev);
	rk3588_vop2_vblank_enable(sc);
	return (0);
}

static void
rk3588_vop2_drm_disable_vblank(struct drm_device *drm, int pipe)
{
	struct rk3588_vop2_softc *sc;

	if (pipe != 0)
		return;
	sc = device_get_softc(drm->dev);
	rk3588_vop2_vblank_disable(sc);
}

static void
rk3588_vop2_intr(void *arg)
{
	struct rk3588_vop2_softc *sc;
	struct drm_pending_vblank_event *event;
	uint32_t status;

	sc = arg;
	status = bus_read_4(sc->mem, VOP2_VP0_INT_STATUS) &
	    VOP2_VP0_INT_MASK;
	if (status == 0)
		return;
	bus_write_4(sc->mem, VOP2_VP0_INT_CLR, (status << 16) | status);
	bus_barrier(sc->mem, VOP2_VP0_INT_CLR, sizeof(uint32_t),
	    BUS_SPACE_BARRIER_READ | BUS_SPACE_BARRIER_WRITE);
	sc->vblank_irq_status = status;
	if ((status & VOP2_VP0_FS_FIELD) != 0) {
		sc->vblank_irq_count++;
		if (sc->drm_inited) {
			drm_handle_vblank(&sc->drm_dev, 0);
			mtx_lock(&sc->drm_dev.event_lock);
			event = sc->flip_event;
			if (event != NULL) {
				sc->flip_event = NULL;
				drm_send_vblank_event(&sc->drm_dev, 0, event);
				drm_vblank_put(&sc->drm_dev, 0);
				sc->page_flip_count++;
			}
			mtx_unlock(&sc->drm_dev.event_lock);
		}
	}
}

static void
rk3588_vop2_bo_load(void *arg, bus_dma_segment_t *segs, int nsegs, int error)
{
	struct rk3588_vop2_bo *bo;

	bo = arg;
	if (error != 0) {
		bo->dma_error = error;
		return;
	}
	if (nsegs != 1 || segs[0].ds_len < bo->gem_obj.size ||
	    segs[0].ds_addr > BUS_SPACE_MAXADDR_32BIT) {
		bo->dma_error = EFBIG;
		return;
	}
	bo->dma_addr = segs[0].ds_addr;
}

static void
rk3588_vop2_bo_free_pages(struct rk3588_vop2_bo *bo)
{
	size_t i;

	if (bo->pages == NULL)
		return;
	for (i = 0; i < bo->npages; i++) {
		KASSERT(bo->pages[i]->object == NULL,
		    ("RK3588 GEM page still belongs to a pager"));
		vm_page_unwire_noq(bo->pages[i]);
		vm_page_free(bo->pages[i]);
	}
	free(bo->pages, DRM_MEM_DRIVER);
	bo->pages = NULL;
}

static void
rk3588_vop2_bo_free_object(struct drm_gem_object *gem_obj)
{
	struct rk3588_vop2_bo *bo;

	bo = container_of(gem_obj, struct rk3588_vop2_bo, gem_obj);
	KASSERT(bo->pager == NULL, ("RK3588 GEM pager still active"));
	drm_gem_free_mmap_offset(gem_obj);
	drm_gem_object_release(gem_obj);
	if (bo->map_loaded)
		bus_dmamap_unload(bo->tag, bo->map);
	if (bo->map != NULL)
		bus_dmamap_destroy(bo->tag, bo->map);
	if (bo->tag != NULL)
		bus_dma_tag_destroy(bo->tag);
	rk3588_vop2_bo_free_pages(bo);
	free(bo, DRM_MEM_DRIVER);
}

static int
rk3588_vop2_bo_create(struct drm_device *drm, size_t size,
    struct rk3588_vop2_bo **result)
{
	struct rk3588_vop2_bo *bo;
	struct memdesc mem;
	vm_page_t first;
	size_t i;
	int error;

	size = round_page(size);
	if (size == 0)
		return (-EINVAL);
	bo = malloc(sizeof(*bo), DRM_MEM_DRIVER, M_WAITOK | M_ZERO);
	bo->sc = device_get_softc(drm->dev);
	error = drm_gem_object_init(drm, &bo->gem_obj, size);
	if (error != 0)
		goto fail_bo;

	bo->npages = atop(size);
	bo->pages = mallocarray(bo->npages, sizeof(*bo->pages),
	    DRM_MEM_DRIVER, M_WAITOK | M_ZERO);
	first = vm_page_alloc_noobj_contig(VM_ALLOC_WAITOK | VM_ALLOC_WIRED |
	    VM_ALLOC_ZERO, bo->npages, 0, 1ULL << 32, PAGE_SIZE, 0,
	    VM_MEMATTR_DEFAULT);
	if (first == NULL) {
		free(bo->pages, DRM_MEM_DRIVER);
		bo->pages = NULL;
		error = -ENOMEM;
		goto fail_gem;
	}
	for (i = 0; i < bo->npages; i++) {
		bo->pages[i] = first + i;
		bo->pages[i]->valid = VM_PAGE_BITS_ALL;
	}

	error = bus_dma_tag_create(bus_get_dma_tag(bo->sc->dev), PAGE_SIZE, 0,
	    BUS_SPACE_MAXADDR_32BIT, BUS_SPACE_MAXADDR, NULL, NULL, size, 1,
	    size, 0, NULL, NULL, &bo->tag);
	if (error != 0) {
		error = -error;
		goto fail_pages;
	}
	error = bus_dmamap_create(bo->tag, 0, &bo->map);
	if (error != 0) {
		error = -error;
		goto fail_tag;
	}
	mem = memdesc_vmpages(bo->pages, size, 0);
	error = bus_dmamap_load_mem(bo->tag, bo->map, &mem,
	    rk3588_vop2_bo_load, bo, BUS_DMA_NOWAIT);
	if (error == 0)
		error = bo->dma_error;
	if (error != 0) {
		error = -error;
		goto fail_map;
	}
	bo->map_loaded = true;
	*result = bo;
	return (0);

fail_map:
	bus_dmamap_destroy(bo->tag, bo->map);
	bo->map = NULL;
fail_tag:
	bus_dma_tag_destroy(bo->tag);
	bo->tag = NULL;
fail_pages:
	rk3588_vop2_bo_free_pages(bo);
fail_gem:
	drm_gem_object_release(&bo->gem_obj);
fail_bo:
	free(bo, DRM_MEM_DRIVER);
	return (error);
}

static int
rk3588_vop2_gem_pager_fault(vm_object_t vm_obj, vm_ooffset_t offset,
    int prot, vm_page_t *mres)
{
	struct rk3588_vop2_bo *bo;
	vm_page_t page;
	vm_pindex_t pindex;

	(void)prot;
	bo = container_of(vm_obj->handle, struct rk3588_vop2_bo, gem_obj);
	pindex = OFF_TO_IDX(offset);
	if (pindex >= bo->npages)
		return (VM_PAGER_FAIL);
	page = bo->pages[pindex];
	if (page->object != NULL)
		return (VM_PAGER_FAIL);
	KASSERT(vm_page_wired(page), ("RK3588 GEM page is not wired"));
	KASSERT(!vm_page_busied(page), ("RK3588 GEM page is busy"));
	vm_page_busy_acquire(page, 0);
	page->oflags &= ~VPO_UNMANAGED;
	vm_page_replace(page, vm_obj, (*mres)->pindex, *mres);
	*mres = page;
	bo->pager = vm_obj;
	page->valid = VM_PAGE_BITS_ALL;
	return (VM_PAGER_OK);
}

static int
rk3588_vop2_gem_pager_ctor(void *handle, vm_ooffset_t size, vm_prot_t prot,
    vm_ooffset_t foff, struct ucred *cred, u_short *color)
{

	(void)handle;
	(void)size;
	(void)prot;
	(void)foff;
	(void)cred;
	if (color != NULL)
		*color = 0;
	return (0);
}

static void
rk3588_vop2_gem_pager_dtor(void *handle)
{
	struct drm_gem_object *gem_obj;
	struct rk3588_vop2_bo *bo;
	vm_object_t pager;
	vm_page_t page;
	size_t i;

	gem_obj = handle;
	bo = container_of(gem_obj, struct rk3588_vop2_bo, gem_obj);
	pager = bo->pager;
	if (pager != NULL) {
		VM_OBJECT_WLOCK(pager);
retry:
		for (i = 0; i < bo->npages; i++) {
			page = bo->pages[i];
			if (page->object != pager)
				continue;
			if (vm_page_busy_acquire(page, VM_ALLOC_WAITFAIL) == 0)
				goto retry;
			cdev_pager_free_page(pager, page);
		}
		VM_OBJECT_WUNLOCK(pager);
		bo->pager = NULL;
	}
	drm_gem_pager_dtr(gem_obj);
}

static struct cdev_pager_ops rk3588_vop2_gem_pager_ops = {
	.cdev_pg_fault = rk3588_vop2_gem_pager_fault,
	.cdev_pg_ctor = rk3588_vop2_gem_pager_ctor,
	.cdev_pg_dtor = rk3588_vop2_gem_pager_dtor,
};

static int
rk3588_vop2_dumb_create(struct drm_file *file, struct drm_device *drm,
    struct drm_mode_create_dumb *args)
{
	struct rk3588_vop2_bo *bo;
	uint64_t pitch, size;
	int error;

	if (args->width == 0 || args->height == 0 || args->bpp != 32 ||
	    args->width > RK3588_VOP2_FB_MAX_WIDTH ||
	    args->height > RK3588_VOP2_FB_MAX_HEIGHT)
		return (-EINVAL);
	pitch = roundup2((uint64_t)args->width * RK3588_VOP2_FB_BPP, 64);
	size = pitch * args->height;
	if (pitch > UINT_MAX || size > SIZE_MAX)
		return (-EOVERFLOW);

	error = rk3588_vop2_bo_create(drm, size, &bo);
	if (error != 0)
		return (error);
	error = drm_gem_handle_create(file, &bo->gem_obj, &args->handle);
	if (error != 0) {
		drm_gem_object_unreference_unlocked(&bo->gem_obj);
		return (error);
	}
	args->pitch = pitch;
	args->size = round_page(size);
	drm_gem_object_unreference_unlocked(&bo->gem_obj);
	return (0);
}

static int
rk3588_vop2_dumb_map_offset(struct drm_file *file,
    struct drm_device *drm, uint32_t handle, uint64_t *offset)
{
	struct drm_gem_object *gem_obj;
	int error;

	DRM_LOCK(drm);
	gem_obj = drm_gem_object_lookup(drm, file, handle);
	if (gem_obj == NULL) {
		DRM_UNLOCK(drm);
		return (-EINVAL);
	}
	error = drm_gem_create_mmap_offset(gem_obj);
	if (error == 0)
		*offset = DRM_GEM_MAPPING_OFF(gem_obj->map_list.key) |
		    DRM_GEM_MAPPING_KEY;
	drm_gem_object_unreference(gem_obj);
	DRM_UNLOCK(drm);
	return (error);
}

static int
rk3588_vop2_dumb_destroy(struct drm_file *file, struct drm_device *drm,
    unsigned int handle)
{

	(void)drm;
	return (drm_gem_handle_delete(file, handle));
}

static void
rk3588_vop2_drm_mode(const struct videomode *vm,
    struct drm_display_mode *mode)
{

	memset(mode, 0, sizeof(*mode));
	mode->clock = vm->dot_clock;
	mode->hdisplay = vm->hdisplay;
	mode->hsync_start = vm->hsync_start;
	mode->hsync_end = vm->hsync_end;
	mode->htotal = vm->htotal;
	mode->vdisplay = vm->vdisplay;
	mode->vsync_start = vm->vsync_start;
	mode->vsync_end = vm->vsync_end;
	mode->vtotal = vm->vtotal;
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	if ((vm->flags & VID_PHSYNC) != 0)
		mode->flags |= DRM_MODE_FLAG_PHSYNC;
	if ((vm->flags & VID_NHSYNC) != 0)
		mode->flags |= DRM_MODE_FLAG_NHSYNC;
	if ((vm->flags & VID_PVSYNC) != 0)
		mode->flags |= DRM_MODE_FLAG_PVSYNC;
	if ((vm->flags & VID_NVSYNC) != 0)
		mode->flags |= DRM_MODE_FLAG_NVSYNC;
	if ((vm->flags & VID_INTERLACE) != 0)
		mode->flags |= DRM_MODE_FLAG_INTERLACE;
	drm_mode_set_name(mode);
}

static void
rk3588_vop2_videomode(const struct drm_display_mode *mode,
    struct videomode *vm)
{

	memset(vm, 0, sizeof(*vm));
	vm->dot_clock = mode->clock;
	vm->hdisplay = mode->hdisplay;
	vm->hsync_start = mode->hsync_start;
	vm->hsync_end = mode->hsync_end;
	vm->htotal = mode->htotal;
	vm->vdisplay = mode->vdisplay;
	vm->vsync_start = mode->vsync_start;
	vm->vsync_end = mode->vsync_end;
	vm->vtotal = mode->vtotal;
	if ((mode->flags & DRM_MODE_FLAG_PHSYNC) != 0)
		vm->flags |= VID_PHSYNC;
	if ((mode->flags & DRM_MODE_FLAG_NHSYNC) != 0)
		vm->flags |= VID_NHSYNC;
	if ((mode->flags & DRM_MODE_FLAG_PVSYNC) != 0)
		vm->flags |= VID_PVSYNC;
	if ((mode->flags & DRM_MODE_FLAG_NVSYNC) != 0)
		vm->flags |= VID_NVSYNC;
	if ((mode->flags & DRM_MODE_FLAG_INTERLACE) != 0)
		vm->flags |= VID_INTERLACE;
}

static enum drm_connector_status
rk3588_vop2_drm_detect(struct drm_connector *connector, bool force)
{
	int connected;

	(void)connector;
	(void)force;
	if (rk3588_hdmi_qp_get_hpd(0, &connected) != 0)
		return (connector_status_unknown);
	return (connected ? connector_status_connected :
	    connector_status_disconnected);
}

static int
rk3588_vop2_drm_get_modes(struct drm_connector *connector)
{
	union {
		struct edid edid;
		uint8_t bytes[256];
	} raw;
	size_t length;
	int error;

	memset(&raw, 0, sizeof(raw));
	error = rk3588_hdmi_qp_get_edid(0, raw.bytes, sizeof(raw.bytes),
	    &length);
	if (error != 0) {
		drm_mode_connector_update_edid_property(connector, NULL);
		return (0);
	}
	drm_mode_connector_update_edid_property(connector, &raw.edid);
	return (drm_add_edid_modes(connector, &raw.edid));
}

static int
rk3588_vop2_drm_mode_valid(struct drm_connector *connector,
    struct drm_display_mode *mode)
{

	(void)connector;
	if ((mode->flags & DRM_MODE_FLAG_INTERLACE) != 0)
		return (MODE_NO_INTERLACE);
	if ((mode->hdisplay == 1920 && mode->vdisplay == 1080 &&
	    mode->clock == 148500) ||
	    (mode->hdisplay == 1920 && mode->vdisplay == 1200 &&
	    mode->clock == 154000) ||
	    (mode->hdisplay == 3840 && mode->vdisplay == 2160 &&
	    mode->clock == 297000))
		return (MODE_OK);
	return (MODE_BAD);
}

static struct drm_encoder *
rk3588_vop2_drm_best_encoder(struct drm_connector *connector)
{
	struct rk3588_vop2_softc *sc;

	sc = container_of(connector, struct rk3588_vop2_softc, drm_connector);
	return (&sc->drm_encoder);
}

static void
rk3588_vop2_drm_connector_dpms(struct drm_connector *connector, int mode)
{
	struct rk3588_vop2_softc *sc;
	int error;

	if (mode == connector->dpms)
		return;
	sc = container_of(connector, struct rk3588_vop2_softc,
	    drm_connector);
	if (mode == DRM_MODE_DPMS_ON) {
		error = rk3588_hdmi_qp_prepare_output(0);
		if (error == 0)
			error = rk3588_vop2_crtc_set_output(sc, true);
		if (error == 0)
			error = rk3588_hdmi_qp_set_output(0, true);
		if (error != 0) {
			(void)rk3588_vop2_crtc_set_output(sc, false);
			(void)rk3588_hdmi_qp_set_output(0, false);
		}
	} else {
		error = rk3588_vop2_crtc_set_output(sc, false);
		if (error == 0) {
			error = rk3588_hdmi_qp_set_output(0, false);
			if (error != 0)
				(void)rk3588_vop2_crtc_set_output(sc, true);
		}
	}
	sc->drm_dpms_error = error;
	if (error == 0) {
		connector->dpms = mode;
		sc->drm_dpms_count++;
	} else {
		device_printf(sc->dev, "DRM DPMS mode %d failed: %d\n",
		    mode, error);
	}
}

static void
rk3588_vop2_drm_connector_destroy(struct drm_connector *connector)
{

	drm_connector_cleanup(connector);
}

static void
rk3588_vop2_drm_encoder_destroy(struct drm_encoder *encoder)
{

	drm_encoder_cleanup(encoder);
}

static int
rk3588_vop2_drm_set_config(struct drm_mode_set *set)
{
	struct rk3588_vop2_drm_fb *rkfb;
	struct rk3588_vop2_softc *sc;
	struct videomode mode;
	int error;

	if (set == NULL || set->crtc == NULL)
		return (-EINVAL);
	sc = container_of(set->crtc, struct rk3588_vop2_softc, drm_crtc);
	if (set->crtc != &sc->drm_crtc)
		return (-EINVAL);
	if (set->fb == NULL) {
		error = rk3588_vop2_drm_set_scanout(sc, sc->fb_addr,
		    sc->fb_stride, sc->fb_width, sc->fb_height, sc->fb_tag,
		    sc->fb_map, true);
		if (error != 0)
			return (-error);
		sc->drm_crtc.fb = &sc->drm_fb;
		sc->drm_crtc.enabled = true;
		sc->drm_modeset_count++;
		return (0);
	}
	if ((set->fb != &sc->drm_fb &&
	    set->fb->funcs != &rk3588_vop2_drm_gem_fb_funcs) ||
	    set->mode == NULL || set->x != 0 || set->y != 0 ||
	    set->num_connectors != 1 || set->connectors == NULL ||
	    set->connectors[0] != &sc->drm_connector ||
	    rk3588_vop2_drm_mode_valid(&sc->drm_connector,
	    set->mode) != MODE_OK)
		return (-EINVAL);

	rk3588_vop2_videomode(set->mode, &mode);
	error = rk3588_hdmi_qp_set_mode(0, &mode);
	sc->drm_modeset_error = error;
	if (error != 0)
		return (-error);
	if (set->fb != &sc->drm_fb) {
		rkfb = container_of(set->fb, struct rk3588_vop2_drm_fb, base);
		error = rk3588_vop2_drm_set_scanout(sc, rkfb->bo->dma_addr,
		    set->fb->pitches[0], set->mode->hdisplay,
		    set->mode->vdisplay, rkfb->bo->tag, rkfb->bo->map, true);
		sc->drm_modeset_error = error;
		if (error != 0)
			return (-error);
	}

	sc->drm_crtc.fb = set->fb;
	sc->drm_crtc.enabled = true;
	sc->drm_crtc.mode = *set->mode;
	sc->drm_crtc.hwmode = *set->mode;
	sc->drm_crtc.x = 0;
	sc->drm_crtc.y = 0;
	sc->drm_encoder.crtc = &sc->drm_crtc;
	sc->drm_connector.encoder = &sc->drm_encoder;
	sc->drm_connector.dpms = DRM_MODE_DPMS_ON;
	sc->drm_modeset_count++;
	return (0);
}

static void
rk3588_vop2_drm_crtc_destroy(struct drm_crtc *crtc)
{

	drm_crtc_cleanup(crtc);
}

static int
rk3588_vop2_drm_page_flip(struct drm_crtc *crtc,
    struct drm_framebuffer *fb, struct drm_pending_vblank_event *event)
{
	struct rk3588_vop2_drm_fb *rkfb;
	struct rk3588_vop2_softc *sc;
	int error;

	sc = container_of(crtc, struct rk3588_vop2_softc, drm_crtc);
	if (crtc != &sc->drm_crtc ||
	    fb->funcs != &rk3588_vop2_drm_gem_fb_funcs)
		return (-EINVAL);
	mtx_lock(&sc->drm_dev.event_lock);
	if (sc->flip_event != NULL) {
		mtx_unlock(&sc->drm_dev.event_lock);
		return (-EBUSY);
	}
	mtx_unlock(&sc->drm_dev.event_lock);

	rkfb = container_of(fb, struct rk3588_vop2_drm_fb, base);
	if (event == NULL) {
		error = rk3588_vop2_drm_set_scanout(sc, rkfb->bo->dma_addr,
		    fb->pitches[0], crtc->mode.hdisplay, crtc->mode.vdisplay,
		    rkfb->bo->tag, rkfb->bo->map, true);
		sc->page_flip_error = error;
		if (error != 0)
			return (-error);
		crtc->fb = fb;
		sc->page_flip_count++;
		return (0);
	}

	error = drm_vblank_get(&sc->drm_dev, 0);
	if (error != 0) {
		sc->page_flip_error = -error;
		return (error);
	}
	rk3588_vop2_vblank_disable(sc);
	error = rk3588_vop2_drm_set_scanout(sc, rkfb->bo->dma_addr,
	    fb->pitches[0], crtc->mode.hdisplay, crtc->mode.vdisplay,
	    rkfb->bo->tag, rkfb->bo->map, false);
	sc->page_flip_error = error;
	if (error != 0) {
		rk3588_vop2_vblank_enable(sc);
		drm_vblank_put(&sc->drm_dev, 0);
		return (-error);
	}

	event->pipe = 0;
	mtx_lock(&sc->drm_dev.event_lock);
	sc->flip_event = event;
	crtc->fb = fb;
	mtx_unlock(&sc->drm_dev.event_lock);
	rk3588_vop2_vblank_enable(sc);
	return (0);
}

static const struct drm_connector_helper_funcs
rk3588_vop2_drm_connector_helper_funcs = {
	.get_modes = rk3588_vop2_drm_get_modes,
	.mode_valid = rk3588_vop2_drm_mode_valid,
	.best_encoder = rk3588_vop2_drm_best_encoder,
};

static const struct drm_connector_funcs rk3588_vop2_drm_connector_funcs = {
	.dpms = rk3588_vop2_drm_connector_dpms,
	.detect = rk3588_vop2_drm_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = rk3588_vop2_drm_connector_destroy,
};

static const struct drm_encoder_funcs rk3588_vop2_drm_encoder_funcs = {
	.destroy = rk3588_vop2_drm_encoder_destroy,
};

static const struct drm_crtc_funcs rk3588_vop2_drm_crtc_funcs = {
	.destroy = rk3588_vop2_drm_crtc_destroy,
	.page_flip = rk3588_vop2_drm_page_flip,
	.set_config = rk3588_vop2_drm_set_config,
};

static void
rk3588_vop2_drm_fb_destroy(struct drm_framebuffer *fb)
{

	drm_framebuffer_cleanup(fb);
}

static int
rk3588_vop2_drm_fb_create_handle(struct drm_framebuffer *fb,
    struct drm_file *file, unsigned int *handle)
{

	(void)fb;
	(void)file;
	*handle = 0;
	return (-EOPNOTSUPP);
}

static const struct drm_framebuffer_funcs rk3588_vop2_drm_fb_funcs = {
	.destroy = rk3588_vop2_drm_fb_destroy,
	.create_handle = rk3588_vop2_drm_fb_create_handle,
};

static void
rk3588_vop2_drm_gem_fb_destroy(struct drm_framebuffer *fb)
{
	struct rk3588_vop2_drm_fb *rkfb;

	rkfb = container_of(fb, struct rk3588_vop2_drm_fb, base);
	drm_gem_object_unreference_unlocked(&rkfb->bo->gem_obj);
	drm_framebuffer_cleanup(fb);
	free(rkfb, DRM_MEM_DRIVER);
}

static int
rk3588_vop2_drm_gem_fb_create_handle(struct drm_framebuffer *fb,
    struct drm_file *file, unsigned int *handle)
{
	struct rk3588_vop2_drm_fb *rkfb;

	rkfb = container_of(fb, struct rk3588_vop2_drm_fb, base);
	return (drm_gem_handle_create(file, &rkfb->bo->gem_obj, handle));
}

static const struct drm_framebuffer_funcs rk3588_vop2_drm_gem_fb_funcs = {
	.destroy = rk3588_vop2_drm_gem_fb_destroy,
	.create_handle = rk3588_vop2_drm_gem_fb_create_handle,
};

static int
rk3588_vop2_drm_fb_create(struct drm_device *drm, struct drm_file *file,
    struct drm_mode_fb_cmd2 *cmd, struct drm_framebuffer **fb)
{
	struct drm_gem_object *gem_obj;
	struct rk3588_vop2_drm_fb *rkfb;
	uint64_t required;
	int error, i;

	*fb = NULL;
	if (cmd->pixel_format != DRM_FORMAT_XRGB8888 ||
	    cmd->width == 0 || cmd->height == 0 ||
	    cmd->width > RK3588_VOP2_FB_MAX_WIDTH ||
	    cmd->height > RK3588_VOP2_FB_MAX_HEIGHT ||
	    cmd->handles[0] == 0 || cmd->offsets[0] != 0 ||
	    cmd->pitches[0] < cmd->width * RK3588_VOP2_FB_BPP ||
	    (cmd->pitches[0] & 63) != 0)
		return (-EINVAL);
	for (i = 1; i < 4; i++)
		if (cmd->handles[i] != 0 || cmd->pitches[i] != 0 ||
		    cmd->offsets[i] != 0)
			return (-EINVAL);

	required = (uint64_t)(cmd->height - 1) * cmd->pitches[0] +
	    (uint64_t)cmd->width * RK3588_VOP2_FB_BPP;
	gem_obj = drm_gem_object_lookup(drm, file, cmd->handles[0]);
	if (gem_obj == NULL)
		return (-ENOENT);
	if (required > gem_obj->size) {
		drm_gem_object_unreference_unlocked(gem_obj);
		return (-EINVAL);
	}

	rkfb = malloc(sizeof(*rkfb), DRM_MEM_DRIVER, M_WAITOK | M_ZERO);
	rkfb->bo = container_of(gem_obj, struct rk3588_vop2_bo, gem_obj);
	drm_helper_mode_fill_fb_struct(&rkfb->base, cmd);
	error = drm_framebuffer_init(drm, &rkfb->base,
	    &rk3588_vop2_drm_gem_fb_funcs);
	if (error != 0) {
		drm_gem_object_unreference_unlocked(gem_obj);
		free(rkfb, DRM_MEM_DRIVER);
		return (error);
	}
	*fb = &rkfb->base;
	return (0);
}

static const struct drm_mode_config_funcs rk3588_vop2_drm_config_funcs = {
	.fb_create = rk3588_vop2_drm_fb_create,
};

static int
rk3588_vop2_drm_load(struct drm_device *drm, unsigned long flags)
{
	struct rk3588_vop2_softc *sc;
	int error;

	(void)flags;
	sc = device_get_softc(drm->dev);
	drm_mode_config_init(drm);
	drm->mode_config.min_width = 32;
	drm->mode_config.min_height = 32;
	drm->mode_config.max_width = RK3588_VOP2_FB_MAX_WIDTH;
	drm->mode_config.max_height = RK3588_VOP2_FB_MAX_HEIGHT;
	drm->mode_config.funcs = &rk3588_vop2_drm_config_funcs;

	error = drm_crtc_init(drm, &sc->drm_crtc,
	    &rk3588_vop2_drm_crtc_funcs);
	if (error != 0)
		goto fail;
	error = drm_encoder_init(drm, &sc->drm_encoder,
	    &rk3588_vop2_drm_encoder_funcs, DRM_MODE_ENCODER_TMDS);
	if (error != 0)
		goto fail;
	error = drm_connector_init(drm, &sc->drm_connector,
	    &rk3588_vop2_drm_connector_funcs, DRM_MODE_CONNECTOR_HDMIA);
	if (error != 0)
		goto fail;
	drm_connector_helper_add(&sc->drm_connector,
	    &rk3588_vop2_drm_connector_helper_funcs);
	error = drm_mode_connector_attach_encoder(&sc->drm_connector,
	    &sc->drm_encoder);
	if (error != 0)
		goto fail;
	sc->drm_connector.polled = DRM_CONNECTOR_POLL_HPD;
	sc->drm_fb.width = RK3588_VOP2_FB_MAX_WIDTH;
	sc->drm_fb.height = RK3588_VOP2_FB_MAX_HEIGHT;
	sc->drm_fb.depth = 24;
	sc->drm_fb.bits_per_pixel = 32;
	sc->drm_fb.pitches[0] = sc->fb_stride;
	sc->drm_fb.pixel_format = DRM_FORMAT_XRGB8888;
	error = drm_framebuffer_init(drm, &sc->drm_fb,
	    &rk3588_vop2_drm_fb_funcs);
	if (error != 0)
		goto fail;

	sc->drm_encoder.possible_crtcs = 1;
	sc->drm_encoder.crtc = &sc->drm_crtc;
	sc->drm_connector.encoder = &sc->drm_encoder;
	sc->drm_connector.dpms = DRM_MODE_DPMS_ON;
	sc->drm_connector.status =
	    rk3588_vop2_drm_detect(&sc->drm_connector, false);
	sc->drm_crtc.enabled = true;
	sc->drm_crtc.fb = &sc->drm_fb;
	rk3588_vop2_drm_mode(&sc->mode, &sc->drm_crtc.mode);
	sc->drm_crtc.hwmode = sc->drm_crtc.mode;
	drm->irq_enabled = true;
	drm->max_vblank_count = UINT_MAX;
	error = drm_vblank_init(drm, drm->mode_config.num_crtc);
	if (error != 0)
		goto fail;
	drm->vblank_disable_allowed = true;
	drm_kms_helper_poll_init(drm);
	return (0);

fail:
	drm_mode_config_cleanup(drm);
	return (error);
}

static int
rk3588_vop2_drm_unload(struct drm_device *drm)
{

	drm_kms_helper_poll_fini(drm);
	drm_vblank_cleanup(drm);
	drm_mode_config_cleanup(drm);
	return (0);
}

static void
rk3588_vop2_drm_preclose(struct drm_device *drm, struct drm_file *file)
{
	struct rk3588_vop2_softc *sc;

	sc = device_get_softc(drm->dev);
	mtx_lock(&drm->event_lock);
	if (sc->flip_event != NULL &&
	    sc->flip_event->base.file_priv == file) {
		sc->flip_event->base.destroy(&sc->flip_event->base);
		sc->flip_event = NULL;
		drm_vblank_put(drm, 0);
		sc->page_flip_cancel_count++;
	}
	mtx_unlock(&drm->event_lock);
}

static struct drm_driver rk3588_vop2_drm_driver = {
	.driver_features = DRIVER_MODESET | DRIVER_GEM,
	.load = rk3588_vop2_drm_load,
	.unload = rk3588_vop2_drm_unload,
	.preclose = rk3588_vop2_drm_preclose,
	.gem_free_object = rk3588_vop2_bo_free_object,
	.gem_pager_ops = &rk3588_vop2_gem_pager_ops,
	.dumb_create = rk3588_vop2_dumb_create,
	.dumb_map_offset = rk3588_vop2_dumb_map_offset,
	.dumb_destroy = rk3588_vop2_dumb_destroy,
	.get_vblank_counter = drm_vblank_count,
	.enable_vblank = rk3588_vop2_drm_enable_vblank,
	.disable_vblank = rk3588_vop2_drm_disable_vblank,
	.name = "rk3588-vop2",
	.desc = "Rockchip RK3588 VOP2 KMS",
	.date = "20260724",
	.major = 0,
	.minor = 1,
	.patchlevel = 0,
};

static int
rk3588_vop2_drm_init(struct rk3588_vop2_softc *sc)
{

	sc->drm_error = drm_get_platform_dev(sc->dev, &sc->drm_dev,
	    &rk3588_vop2_drm_driver);
	sc->drm_inited = sc->drm_error == 0;
	return (sc->drm_error);
}

void
rk3588_vop2_hpd_changed(void)
{
	struct rk3588_vop2_softc *sc;

	sc = rk3588_vop2_sc;
	if (sc != NULL && sc->drm_inited)
		drm_helper_hpd_irq_event(&sc->drm_dev);
}

static int
rk3588_vop2_iommu_snapshot(struct rk3588_vop2_softc *sc,
    struct rk3588_vop2_mmu_snapshot snapshot[RK3588_VOP2_MMU_BANKS])
{
	struct resource *res;
	device_t iommu;
	pcell_t xref;
	phandle_t node;
	int i, rid;

	node = ofw_bus_get_node(sc->dev);
	if (OF_getencprop(node, "iommus", &xref, sizeof(xref)) != sizeof(xref))
		return (ENXIO);
	iommu = ofw_bus_find_child_device_by_phandle(device_get_parent(sc->dev),
	    OF_node_from_xref(xref));
	if (iommu == NULL ||
	    !ofw_bus_is_compatible(iommu, "rockchip,rk3588-iommu"))
		return (ENXIO);

	for (i = 0; i < RK3588_VOP2_MMU_BANKS; i++) {
		rid = i;
		res = bus_alloc_resource_any(iommu, SYS_RES_MEMORY, &rid,
		    RF_ACTIVE);
		if (res == NULL)
			return (ENXIO);
		snapshot[i].dte = bus_read_4(res, RK_MMU_DTE_ADDR);
		snapshot[i].status = bus_read_4(res, RK_MMU_STATUS);
		snapshot[i].fault = bus_read_4(res, RK_MMU_PAGE_FAULT_ADDR);
		snapshot[i].raw = bus_read_4(res, RK_MMU_INT_RAWSTAT);
		snapshot[i].mask = bus_read_4(res, RK_MMU_INT_MASK);
		snapshot[i].irq = bus_read_4(res, RK_MMU_INT_STATUS);
		bus_release_resource(iommu, SYS_RES_MEMORY, rid, res);
	}
	return (0);
}

static void
rk3588_vop2_fb_load(void *arg, bus_dma_segment_t *segs, int nsegs, int error)
{
	struct rk3588_vop2_softc *sc;

	sc = arg;
	if (error != 0) {
		sc->error = error;
		return;
	}
	sc->fb_nsegs = nsegs;
	if (nsegs != 1 || segs[0].ds_addr > BUS_SPACE_MAXADDR_32BIT ||
	    segs[0].ds_len < sc->fb_size) {
		sc->error = EFBIG;
		return;
	}
	sc->fb_addr = segs[0].ds_addr;
}

static int
rk3588_vop2_get_mode(struct rk3588_vop2_softc *sc)
{
	int error;

	error = rk3588_hdmi_qp_get_mode(0, &sc->mode);
	if (error != 0)
		return (error);
	if ((sc->mode.flags & VID_INTERLACE) != 0 ||
	    !((sc->mode.hdisplay == 1920 && sc->mode.vdisplay == 1080 &&
	    sc->mode.dot_clock == 148500) ||
	    (sc->mode.hdisplay == 1920 && sc->mode.vdisplay == 1200 &&
	    sc->mode.dot_clock == 154000) ||
	    (sc->mode.hdisplay == 3840 && sc->mode.vdisplay == 2160 &&
	    sc->mode.dot_clock == 297000)))
		return (ERANGE);

	sc->tmds_rate = (uint64_t)sc->mode.dot_clock * 1000;
	sc->fb_width = sc->mode.hdisplay;
	sc->fb_height = sc->mode.vdisplay;
	sc->fb_stride = RK3588_VOP2_FB_MAX_WIDTH * RK3588_VOP2_FB_BPP;
	sc->fb_size = sc->fb_stride * RK3588_VOP2_FB_MAX_HEIGHT;
	sc->fb_window_info = ((sc->fb_height - 1) << 16) |
	    (sc->fb_width - 1);
	return (0);
}

static int
rk3588_vop2_fb_alloc(struct rk3588_vop2_softc *sc)
{
	static const uint32_t colors[] = {
		0xffffffff, 0xffffff00, 0xff00ffff, 0xff00ff00,
		0xffff00ff, 0xffff0000, 0xff0000ff, 0xff000000
	};
	uint32_t *pixels;
	unsigned int x, y;
	int error;

	error = bus_dma_tag_create(bus_get_dma_tag(sc->dev), PAGE_SIZE, 0,
	    BUS_SPACE_MAXADDR_32BIT, BUS_SPACE_MAXADDR, NULL, NULL,
	    sc->fb_size, 1, sc->fb_size, 0, NULL, NULL,
	    &sc->fb_tag);
	if (error != 0)
		return (error);
	error = bus_dmamem_alloc(sc->fb_tag, &sc->fb_vaddr,
	    BUS_DMA_WAITOK | BUS_DMA_ZERO | BUS_DMA_COHERENT, &sc->fb_map);
	if (error != 0)
		goto fail_tag;

	sc->error = 0;
	error = bus_dmamap_load(sc->fb_tag, sc->fb_map, sc->fb_vaddr,
	    sc->fb_size, rk3588_vop2_fb_load, sc, BUS_DMA_WAITOK);
	if (error == 0) {
		error = sc->error;
		if (error != 0)
			bus_dmamap_unload(sc->fb_tag, sc->fb_map);
	}
	if (error != 0)
		goto fail_mem;

	pixels = sc->fb_vaddr;
	for (y = 0; y < sc->fb_height; y++)
		for (x = 0; x < sc->fb_width; x++)
			pixels[y * (sc->fb_stride / sizeof(*pixels)) + x] =
			    colors[x * nitems(colors) / sc->fb_width];
	bus_dmamap_sync(sc->fb_tag, sc->fb_map, BUS_DMASYNC_PREWRITE);
	return (0);

fail_mem:
	bus_dmamem_free(sc->fb_tag, sc->fb_vaddr, sc->fb_map);
	sc->fb_vaddr = NULL;
fail_tag:
	bus_dma_tag_destroy(sc->fb_tag);
	sc->fb_tag = NULL;
	return (error);
}

static void
rk3588_vop2_mod(struct rk3588_vop2_softc *sc, uint32_t reg,
    uint32_t value, uint32_t mask)
{
	uint32_t current;

	current = bus_read_4(sc->mem, reg);
	bus_write_4(sc->mem, reg, (current & ~mask) | (value & mask));
}

static int
rk3588_vop2_clear_fs(struct rk3588_vop2_softc *sc)
{
	int i;

	for (i = 0; i < 4; i++) {
		bus_write_4(sc->mem, VOP2_VP0_INT_CLR, VOP2_VP0_FS_CLEAR);
		bus_barrier(sc->mem, VOP2_VP0_INT_CLR, sizeof(uint32_t),
		    BUS_SPACE_BARRIER_READ | BUS_SPACE_BARRIER_WRITE);
		if ((bus_read_4(sc->mem, VOP2_VP0_INT_RAW_STATUS) &
		    VOP2_VP0_FS) == 0)
			return (0);
	}
	return (EBUSY);
}

static int
rk3588_vop2_wait_fs(struct rk3588_vop2_softc *sc)
{
	int i;

	for (i = 0; i < 1000; i++) {
		sc->fs_status = bus_read_4(sc->mem, VOP2_VP0_INT_RAW_STATUS);
		sc->fs_polls++;
		if ((sc->fs_status & VOP2_VP0_FS) != 0) {
			sc->fs_count++;
			return (0);
		}
		DELAY(50);
	}
	return (ETIMEDOUT);
}

static int
rk3588_vop2_drm_set_scanout(struct rk3588_vop2_softc *sc, bus_addr_t addr,
    uint32_t pitch, uint32_t width, uint32_t height, bus_dma_tag_t tag,
    bus_dmamap_t map, bool wait)
{
	uint32_t window_info;
	int error;

	if (addr > BUS_SPACE_MAXADDR_32BIT || pitch == 0 ||
	    (pitch & 3) != 0 || width == 0 || height == 0)
		return (EINVAL);
	window_info = ((height - 1) << 16) | (width - 1);
	error = 0;
	bus_dmamap_sync(tag, map, BUS_DMASYNC_PREWRITE);
	error = rk3588_vop2_clear_fs(sc);
	if (error != 0)
		return (error);
	bus_write_4(sc->mem, VOP2_CLUSTER0_WIN0_YRGB_MST, (uint32_t)addr);
	bus_write_4(sc->mem, VOP2_CLUSTER0_WIN0_VIR,
	    pitch / sizeof(uint32_t));
	bus_write_4(sc->mem, VOP2_CLUSTER0_WIN0_ACT_INFO, window_info);
	bus_write_4(sc->mem, VOP2_CLUSTER0_WIN0_DSP_INFO, window_info);
	bus_write_4(sc->mem, VOP2_CLUSTER0_WIN0_DSP_ST, 0);
	bus_barrier(sc->mem, 0, rman_get_size(sc->mem),
	    BUS_SPACE_BARRIER_WRITE);
	bus_write_4(sc->mem, VOP2_REG_CFG_DONE,
	    VOP2_GLB_CFG_DONE_EN | VOP2_VP0_CFG_DONE);
	bus_barrier(sc->mem, 0, rman_get_size(sc->mem),
	    BUS_SPACE_BARRIER_READ | BUS_SPACE_BARRIER_WRITE);
	if (wait)
		error = rk3588_vop2_wait_fs(sc);
	sc->window_addr_after = bus_read_4(sc->mem,
	    VOP2_CLUSTER0_WIN0_YRGB_MST);
	sc->window_vir_after = bus_read_4(sc->mem, VOP2_CLUSTER0_WIN0_VIR);
	sc->window_act_after = bus_read_4(sc->mem,
	    VOP2_CLUSTER0_WIN0_ACT_INFO);
	sc->window_dsp_after = bus_read_4(sc->mem,
	    VOP2_CLUSTER0_WIN0_DSP_INFO);
	sc->iommu_after_error = rk3588_vop2_iommu_snapshot(sc,
	    sc->mmu_after);
	if (wait && error == 0 &&
	    (sc->window_addr_after != (uint32_t)addr ||
	    sc->window_vir_after != pitch / sizeof(uint32_t) ||
	    sc->window_act_after != window_info ||
	    sc->window_dsp_after != window_info))
		error = EIO;
	if (wait && rk3588_vop2_clear_fs(sc) != 0 && error == 0)
		error = EBUSY;
	return (error);
}

static int
rk3588_vop2_clocks(struct rk3588_vop2_softc *sc)
{
	clk_t mux, parent;
	int error, i;

	if (!sc->clocks_ready) {
		for (i = 0; i < VOP2_CLK_COUNT; i++) {
			error = clk_get_by_ofw_name(sc->dev, 0,
			    rk3588_vop2_clock_names[i], &sc->clocks[i]);
			if (error != 0)
				return (error);
		}

		for (i = VOP2_CLK_ACLK; i <= VOP2_CLK_PCLK; i++) {
			if (i == VOP2_CLK_DCLK)
				continue;
			error = clk_enable(sc->clocks[i]);
			if (error != 0)
				return (error);
		}
		sc->drm_core_clocks_enabled = 1;
		/* Linux routes the selected VP DCLK through the HDMI0 PHY PLL. */
		error = clk_get_parent(sc->clocks[VOP2_CLK_DCLK], &mux);
		if (error != 0)
			return (error);
		if (strcmp(clk_get_name(mux), "dclk_vop0_sel") != 0) {
			clk_release(mux);
			return (ENXIO);
		}
		error = clk_set_parent_by_clk(mux, sc->clocks[VOP2_CLK_PHY0]);
		if (error == 0)
			error = clk_get_parent(mux, &parent);
		if (error == 0) {
			if (strcmp(clk_get_name(parent),
			    "clk_hdmiphy_pixel0") != 0)
				error = ENXIO;
			clk_release(parent);
		}
		clk_release(mux);
		if (error != 0)
			return (error);
	}
	error = clk_set_freq(sc->clocks[VOP2_CLK_DCLK],
	    sc->tmds_rate, CLK_SET_ROUND_EXACT);
	if (error != 0)
		return (error);
	if (!sc->clocks_ready) {
		error = clk_enable(sc->clocks[VOP2_CLK_DCLK]);
		if (error != 0)
			return (error);
		sc->drm_crtc_dclk_enabled = 1;
		sc->drm_crtc_dpms_active = 1;
		sc->clocks_ready = 1;
	}

	for (i = 0; i < VOP2_CLK_COUNT; i++) {
		uint64_t expected;

		error = clk_get_freq(sc->clocks[i], &sc->clock_rate[i]);
		expected = (i == VOP2_CLK_DCLK || i == VOP2_CLK_PHY0) ?
		    sc->tmds_rate : rk3588_vop2_clock_rates[i];
		if (error != 0 || sc->clock_rate[i] != expected)
			return (error != 0 ? error : ERANGE);
	}
	return (0);
}

static int
rk3588_vop2_program(struct rk3588_vop2_softc *sc)
{
	uint32_t hact, hact_end, hact_start, hsync_len, htotal_hsync;
	uint32_t pol, pre_scan, vact, vact_end, vact_start, value;
	uint32_t vsync_len, vtotal_vsync;
	int error, i;

	sc->program_stage = "hpd";
	if ((SYSCON_READ_4(sc->grf, RK3588_GRF_SOC_STATUS1) &
	    RK3588_HDMI0_HPD) == 0)
		return (ENXIO);
	sc->program_stage = "power";
	error = rk3588_power_domain_enable_by_node(sc->dev,
	    ofw_bus_get_node(sc->dev));
	if (error != 0)
		return (error);
	sc->program_stage = "clocks";
	error = rk3588_vop2_clocks(sc);
	if (error != 0)
		return (error);
	hsync_len = sc->mode.hsync_end - sc->mode.hsync_start;
	hact_start = sc->mode.htotal - sc->mode.hsync_start;
	hact_end = hact_start + sc->mode.hdisplay;
	vsync_len = sc->mode.vsync_end - sc->mode.vsync_start;
	vact_start = sc->mode.vtotal - sc->mode.vsync_start;
	vact_end = vact_start + sc->mode.vdisplay;
	htotal_hsync = (sc->mode.htotal << 16) | hsync_len;
	hact = (hact_start << 16) | hact_end;
	vtotal_vsync = (sc->mode.vtotal << 16) | vsync_len;
	vact = (vact_start << 16) | vact_end;
	pre_scan = ((VOP2_VP0_BG_DELAY_PIXELS +
	    ((sc->mode.hdisplay + 1) >> 1) - 1) << 16) | MAX(hsync_len, 8);
	/* RK3588 polarity bits are inverted relative to older VOP2 blocks. */
	pol = ((sc->mode.flags & VID_NHSYNC) != 0 ? 1u : 0) |
	    ((sc->mode.flags & VID_NVSYNC) != 0 ? 2u : 0);
	sc->iommu_before_error = rk3588_vop2_iommu_snapshot(sc,
	    sc->mmu_before);

	sc->version = bus_read_4(sc->mem, VOP2_VERSION_INFO);
	if (sc->version != RK3588_VOP2_VERSION)
		return (ENXIO);
	sc->cfg_before = bus_read_4(sc->mem, VOP2_REG_CFG_DONE);
	sc->pd_before = bus_read_4(sc->mem, VOP2_SYS_PD_CTRL);
	sc->if_en_before = bus_read_4(sc->mem, VOP2_DSP_IF_EN);
	sc->if_ctrl_before = bus_read_4(sc->mem, VOP2_DSP_IF_CTRL);
	sc->if_pol_before = bus_read_4(sc->mem, VOP2_DSP_IF_POL);
	sc->dsp_ctrl_before = bus_read_4(sc->mem, VOP2_VP0_DSP_CTRL);
	sc->ovl_ctrl_before = bus_read_4(sc->mem, VOP2_OVL_CTRL);
	sc->layer_sel_before = bus_read_4(sc->mem, VOP2_OVL_LAYER_SEL);
	sc->port_sel_before = bus_read_4(sc->mem, VOP2_OVL_PORT_SEL);

	sc->program_stage = "power-status";
	/* Linux powers the four cluster domains and the shared ESMART domain. */
	bus_write_4(sc->mem, VOP2_SYS_PD_CTRL,
	    sc->pd_before & ~VOP2_PD_ENABLE_MASK);
	for (i = 0; i < 1000; i++) {
		sc->pd_status = bus_read_4(sc->mem, VOP2_SYS_STATUS0);
		if ((sc->pd_status & VOP2_PD_STATUS_MASK) == 0)
			break;
		DELAY(50);
	}
	if ((sc->pd_status & VOP2_PD_STATUS_MASK) != 0)
		return (ETIMEDOUT);
	sc->pd_after = bus_read_4(sc->mem, VOP2_SYS_PD_CTRL);

	sc->program_stage = "registers";
	bus_write_4(sc->mem, VOP2_REG_CFG_DONE, VOP2_GLB_CFG_DONE_EN);
	rk3588_vop2_mod(sc, VOP2_AUTO_GATING_CTRL, 0,
	    VOP2_AUTO_GATING_EN);
	for (i = 0; i < nitems(rk3588_vop2_window_enable_regs); i++)
		rk3588_vop2_mod(sc, rk3588_vop2_window_enable_regs[i], 0, 1);
	bus_write_4(sc->mem, VOP2_ESMART0_REGION1_CTRL, 0);
	bus_write_4(sc->mem, VOP2_ESMART0_REGION2_CTRL, 0);
	bus_write_4(sc->mem, VOP2_ESMART0_REGION3_CTRL, 0);
	rk3588_vop2_mod(sc, VOP2_OVL_CTRL, 0, VOP2_OVL_VP0_YUV |
	    VOP2_OVL_LAYERSEL_REGDONE_IMD |
	    VOP2_OVL_LAYERSEL_REGDONE_MASK);

	rk3588_vop2_mod(sc, VOP2_DSP_IF_EN, VOP2_HDMI0_EN,
	    VOP2_HDMI0_EN | VOP2_HDMI0_MUX_MASK);
	rk3588_vop2_mod(sc, VOP2_DSP_IF_CTRL,
	    VOP2_HDMI0_DCLK_DIV4 | VOP2_HDMI0_PCLK_DIV2,
	    VOP2_HDMI0_DCLK_DIV_MASK | VOP2_HDMI0_PCLK_DIV2);
	rk3588_vop2_mod(sc, VOP2_DSP_IF_POL,
	    VOP2_IF_CFG_DONE_IMMEDIATE | VOP2_HDMI_DCLK_POL | (pol << 4),
	    VOP2_IF_CFG_DONE_IMMEDIATE | VOP2_HDMI_DCLK_POL |
	    VOP2_HDMI_PIN_POL_MASK);
	SYSCON_WRITE_4(sc->vop_grf, RK3588_VOP_GRF_CON2,
	    (RK3588_HDMITX0_ENABLE << 16) | RK3588_HDMITX0_ENABLE);
	SYSCON_WRITE_4(sc->vo1_grf, RK3588_VO1_GRF_CON0,
	    (RK3588_HDMI0_POL_MASK << 16) | (pol << 5));

	rk3588_vop2_mod(sc, VOP2_VP0_CLK_CTRL,
	    VOP2_VP0_DCLK_CORE_DIV4, VOP2_VP0_CLK_DIV_MASK);
	bus_write_4(sc->mem, VOP2_VP0_HTOTAL_HSYNC, htotal_hsync);
	bus_write_4(sc->mem, VOP2_VP0_HACT, hact);
	bus_write_4(sc->mem, VOP2_VP0_VTOTAL_VSYNC, vtotal_vsync);
	bus_write_4(sc->mem, VOP2_VP0_VACT, vact);
	bus_write_4(sc->mem, VOP2_VP0_BG_MIX_CTRL, VOP2_VP0_BG_DELAY);
	bus_write_4(sc->mem, VOP2_VP0_PRE_SCAN_HTIMING, pre_scan);
	bus_write_4(sc->mem, VOP2_VP0_POST_HACT, hact);
	bus_write_4(sc->mem, VOP2_VP0_POST_VACT, vact);
	bus_write_4(sc->mem, VOP2_VP0_POST_SCALE, VOP2_POST_SCALE_1TO1);
	bus_write_4(sc->mem, VOP2_VP0_POST_SCALE_CTRL, 0);
	bus_write_4(sc->mem, VOP2_VP0_DSP_BG, VOP2_VP0_BG_LINUX);
	rk3588_vop2_mod(sc, VOP2_VP0_COLOR_BAR_CTRL, 0,
	    VOP2_VP0_COLOR_BAR_HORIZONTAL);

	/* Match the live Linux fbcon path: Cluster0-win0 through VP0. */
	bus_write_4(sc->mem, VOP2_OVL_LAYER_SEL,
	    VOP2_OVL_LAYER_SEL_LINUX);
	bus_write_4(sc->mem, VOP2_OVL_PORT_SEL,
	    VOP2_OVL_PORT_SEL_LINUX);
	bus_write_4(sc->mem, VOP2_CLUSTER0_MIX_SRC_COLOR,
	    VOP2_CLUSTER0_MIX_SRC_COLOR_LINUX);
	bus_write_4(sc->mem, VOP2_CLUSTER0_MIX_DST_COLOR,
	    VOP2_CLUSTER0_MIX_DST_COLOR_LINUX);
	bus_write_4(sc->mem, VOP2_CLUSTER0_MIX_SRC_ALPHA,
	    VOP2_CLUSTER0_MIX_SRC_ALPHA_LINUX);
	bus_write_4(sc->mem, VOP2_CLUSTER0_MIX_DST_ALPHA,
	    VOP2_CLUSTER0_MIX_DST_ALPHA_LINUX);
	bus_write_4(sc->mem, VOP2_CLUSTER_DLY_NUM,
	    VOP2_CLUSTER_DLY_NUM_LINUX);
	bus_write_4(sc->mem, VOP2_SMART_DLY_NUM,
	    VOP2_SMART_DLY_NUM_LINUX);

	bus_write_4(sc->mem, VOP2_CLUSTER0_CTRL, 0);
	bus_write_4(sc->mem, VOP2_CLUSTER0_WIN0_CTRL0, 0);
	bus_write_4(sc->mem, VOP2_CLUSTER0_WIN1_CTRL0, 0);
	bus_write_4(sc->mem, VOP2_CLUSTER0_WIN0_CTRL1, 0);
	bus_write_4(sc->mem, VOP2_CLUSTER0_WIN0_CTRL2,
	    VOP2_CLUSTER0_WIN0_AXI_IDS);
	bus_write_4(sc->mem, VOP2_CLUSTER0_WIN0_YRGB_MST,
	    (uint32_t)sc->fb_addr);
	bus_write_4(sc->mem, VOP2_CLUSTER0_WIN0_CBR_MST, 0);
	bus_write_4(sc->mem, VOP2_CLUSTER0_WIN0_VIR,
	    sc->fb_stride / sizeof(uint32_t));
	bus_write_4(sc->mem, VOP2_CLUSTER0_WIN0_ACT_INFO,
	    sc->fb_window_info);
	bus_write_4(sc->mem, VOP2_CLUSTER0_WIN0_DSP_INFO,
	    sc->fb_window_info);
	bus_write_4(sc->mem, VOP2_CLUSTER0_WIN0_DSP_ST, 0);
	bus_write_4(sc->mem, VOP2_CLUSTER0_WIN0_SCL_FACTOR, 0);
	bus_write_4(sc->mem, VOP2_CLUSTER0_WIN0_AFBCD_OUTPUT,
	    VOP2_CLUSTER0_AFBCD_AUTOGATE);
	bus_write_4(sc->mem, VOP2_CLUSTER0_WIN0_AFBCD_CTRL,
	    VOP2_CLUSTER0_AFBCD_HALF_BLOCK);
	bus_dmamap_sync(sc->fb_tag, sc->fb_map, BUS_DMASYNC_PREWRITE);
	bus_write_4(sc->mem, VOP2_CLUSTER0_WIN0_CTRL0,
	    VOP2_CLUSTER0_WIN0_ENABLE);
	bus_write_4(sc->mem, VOP2_CLUSTER0_CTRL, VOP2_CLUSTER0_ENABLE);
	rk3588_vop2_mod(sc, VOP2_VP0_DSP_CTRL, VOP2_VP0_OUT_MODE_AAAA,
	    VOP2_VP0_OUT_MODE_MASK);

	sc->program_stage = "clear-fs-1";
	error = rk3588_vop2_clear_fs(sc);
	if (error != 0)
		return (error);
	bus_barrier(sc->mem, 0, rman_get_size(sc->mem),
	    BUS_SPACE_BARRIER_WRITE);
	bus_write_4(sc->mem, VOP2_REG_CFG_DONE,
	    VOP2_GLB_CFG_DONE_EN | VOP2_VP0_CFG_DONE);
	bus_barrier(sc->mem, 0, rman_get_size(sc->mem),
	    BUS_SPACE_BARRIER_READ | BUS_SPACE_BARRIER_WRITE);
	sc->program_stage = "release-standby";
	rk3588_vop2_mod(sc, VOP2_VP0_DSP_CTRL, VOP2_VP0_OUT_MODE_RGB888,
	    VOP2_VP0_STANDBY | VOP2_VP0_OUT_MODE_MASK);
	bus_barrier(sc->mem, VOP2_VP0_DSP_CTRL, sizeof(uint32_t),
	    BUS_SPACE_BARRIER_READ | BUS_SPACE_BARRIER_WRITE);
	sc->program_stage = "wait-fs-1";
	error = rk3588_vop2_wait_fs(sc);
	if (error == 0) {
		sc->program_stage = "clear-fs-2";
		error = rk3588_vop2_clear_fs(sc);
	}
	if (error == 0) {
		sc->program_stage = "wait-fs-2";
		error = rk3588_vop2_wait_fs(sc);
	}
	if (error != 0)
		return (error);

	sc->program_stage = "verify";
	sc->cfg_after = bus_read_4(sc->mem, VOP2_REG_CFG_DONE);
	sc->if_en_after = bus_read_4(sc->mem, VOP2_DSP_IF_EN);
	sc->if_ctrl_after = bus_read_4(sc->mem, VOP2_DSP_IF_CTRL);
	sc->if_pol_after = bus_read_4(sc->mem, VOP2_DSP_IF_POL);
	sc->dsp_ctrl_after = bus_read_4(sc->mem, VOP2_VP0_DSP_CTRL);
	sc->bg_after = bus_read_4(sc->mem, VOP2_VP0_DSP_BG);
	sc->color_bar_after = bus_read_4(sc->mem,
	    VOP2_VP0_COLOR_BAR_CTRL);
	sc->ovl_ctrl_after = bus_read_4(sc->mem, VOP2_OVL_CTRL);
	sc->layer_sel_after = bus_read_4(sc->mem, VOP2_OVL_LAYER_SEL);
	sc->port_sel_after = bus_read_4(sc->mem, VOP2_OVL_PORT_SEL);
	sc->cluster_dly_after = bus_read_4(sc->mem, VOP2_CLUSTER_DLY_NUM);
	sc->smart_dly_after = bus_read_4(sc->mem, VOP2_SMART_DLY_NUM);
	sc->cluster_ctrl_after = bus_read_4(sc->mem, VOP2_CLUSTER0_CTRL);
	sc->cluster_ctrl2_after = bus_read_4(sc->mem,
	    VOP2_CLUSTER0_WIN0_CTRL2);
	sc->cluster_mix_after[0] = bus_read_4(sc->mem,
	    VOP2_CLUSTER0_MIX_SRC_COLOR);
	sc->cluster_mix_after[1] = bus_read_4(sc->mem,
	    VOP2_CLUSTER0_MIX_DST_COLOR);
	sc->cluster_mix_after[2] = bus_read_4(sc->mem,
	    VOP2_CLUSTER0_MIX_SRC_ALPHA);
	sc->cluster_mix_after[3] = bus_read_4(sc->mem,
	    VOP2_CLUSTER0_MIX_DST_ALPHA);
	sc->cluster_sub_ctrl_after = bus_read_4(sc->mem,
	    VOP2_CLUSTER0_WIN1_CTRL0);
	sc->cluster_afbc_output_after = bus_read_4(sc->mem,
	    VOP2_CLUSTER0_WIN0_AFBCD_OUTPUT);
	sc->cluster_afbc_ctrl_after = bus_read_4(sc->mem,
	    VOP2_CLUSTER0_WIN0_AFBCD_CTRL);
	sc->window_ctrl_after = bus_read_4(sc->mem,
	    VOP2_CLUSTER0_WIN0_CTRL0);
	sc->window_addr_after = bus_read_4(sc->mem,
	    VOP2_CLUSTER0_WIN0_YRGB_MST);
	sc->window_vir_after = bus_read_4(sc->mem,
	    VOP2_CLUSTER0_WIN0_VIR);
	sc->window_act_after = bus_read_4(sc->mem,
	    VOP2_CLUSTER0_WIN0_ACT_INFO);
	sc->window_dsp_after = bus_read_4(sc->mem,
	    VOP2_CLUSTER0_WIN0_DSP_INFO);
	sc->window_st_after = bus_read_4(sc->mem,
	    VOP2_CLUSTER0_WIN0_DSP_ST);
	sc->win_cfg_done_after = bus_read_4(sc->mem,
	    VOP2_SYS_WIN_REG_CFG_DONE);
	sc->sys_status[0] = bus_read_4(sc->mem, VOP2_SYS_STATUS0);
	sc->sys_status[1] = bus_read_4(sc->mem, VOP2_SYS_STATUS1);
	sc->sys_status[2] = bus_read_4(sc->mem, VOP2_SYS_STATUS2);
	sc->sys_status[3] = bus_read_4(sc->mem, VOP2_SYS_STATUS3);
	sc->sys_int_status[0] = bus_read_4(sc->mem, VOP2_SYS0_INT_STATUS);
	sc->sys_int_status[1] = bus_read_4(sc->mem, VOP2_SYS1_INT_STATUS);
	sc->vp_int_status = bus_read_4(sc->mem, VOP2_VP0_INT_STATUS);
	sc->iommu_after_error = rk3588_vop2_iommu_snapshot(sc,
	    sc->mmu_after);
	sc->vop_grf_after = SYSCON_READ_4(sc->vop_grf,
	    RK3588_VOP_GRF_CON2);
	sc->vo1_grf_after = SYSCON_READ_4(sc->vo1_grf,
	    RK3588_VO1_GRF_CON0);

	value = sc->if_en_after & (VOP2_HDMI0_EN | VOP2_HDMI0_MUX_MASK);
	if (value != VOP2_HDMI0_EN ||
	    (sc->if_ctrl_after & (VOP2_HDMI0_DCLK_DIV_MASK |
	    VOP2_HDMI0_PCLK_DIV2)) != (VOP2_HDMI0_DCLK_DIV4 |
	    VOP2_HDMI0_PCLK_DIV2) ||
	    (sc->if_pol_after & (VOP2_HDMI_DCLK_POL |
	    VOP2_HDMI_PIN_POL_MASK)) != (VOP2_HDMI_DCLK_POL | (pol << 4)) ||
	    (sc->dsp_ctrl_after & (VOP2_VP0_STANDBY |
	    VOP2_VP0_OUT_MODE_MASK)) != VOP2_VP0_OUT_MODE_AAAA ||
	    sc->bg_after != VOP2_VP0_BG_LINUX ||
	    (sc->color_bar_after & VOP2_VP0_COLOR_BAR_HORIZONTAL) != 0 ||
	    (sc->ovl_ctrl_after & (VOP2_OVL_VP0_YUV |
	    VOP2_OVL_LAYERSEL_REGDONE_IMD |
	    VOP2_OVL_LAYERSEL_REGDONE_MASK)) != 0 ||
	    sc->layer_sel_after != VOP2_OVL_LAYER_SEL_LINUX ||
	    sc->port_sel_after != VOP2_OVL_PORT_SEL_LINUX ||
	    sc->cluster_dly_after != VOP2_CLUSTER_DLY_NUM_LINUX ||
	    sc->smart_dly_after != VOP2_SMART_DLY_NUM_LINUX ||
	    (sc->cluster_ctrl_after & 3u) != VOP2_CLUSTER0_ENABLE ||
	    sc->cluster_ctrl2_after != VOP2_CLUSTER0_WIN0_AXI_IDS ||
	    sc->cluster_mix_after[0] !=
	    VOP2_CLUSTER0_MIX_SRC_COLOR_LINUX ||
	    sc->cluster_mix_after[1] !=
	    VOP2_CLUSTER0_MIX_DST_COLOR_LINUX ||
	    sc->cluster_mix_after[2] !=
	    VOP2_CLUSTER0_MIX_SRC_ALPHA_LINUX ||
	    sc->cluster_mix_after[3] !=
	    VOP2_CLUSTER0_MIX_DST_ALPHA_LINUX ||
	    sc->cluster_sub_ctrl_after != 0 ||
	    sc->cluster_afbc_output_after !=
	    VOP2_CLUSTER0_AFBCD_AUTOGATE ||
	    sc->cluster_afbc_ctrl_after != VOP2_CLUSTER0_AFBCD_HALF_BLOCK ||
	    sc->window_ctrl_after != VOP2_CLUSTER0_WIN0_ENABLE ||
	    sc->window_addr_after != (uint32_t)sc->fb_addr ||
	    sc->window_vir_after != sc->fb_stride / sizeof(uint32_t) ||
	    sc->window_act_after != sc->fb_window_info ||
	    sc->window_dsp_after != sc->fb_window_info ||
	    sc->window_st_after != 0 ||
	    (sc->vop_grf_after & RK3588_HDMITX0_ENABLE) == 0 ||
	    (sc->vo1_grf_after & RK3588_HDMI0_POL_MASK) !=
	    (pol << 5))
		return (EIO);
	sc->program_stage = "complete";
	return (0);
}

static void
rk3588_vop2_fb_register(struct rk3588_vop2_softc *sc)
{
	struct fb_info *info;

	info = &sc->fb_info;
	info->fb_type = FBTYPE_MEMCOLOR;
	info->fb_height = sc->fb_height;
	info->fb_width = sc->fb_width;
	info->fb_depth = 32;
	info->fb_size = sc->fb_size;
	info->fb_pbase = (vm_paddr_t)sc->fb_addr;
	info->fb_vbase = (vm_offset_t)sc->fb_vaddr;
	info->fb_name = device_get_nameunit(sc->dev);
	info->fb_flags = FB_FLAG_NOMMAP;
	info->fb_stride = sc->fb_stride;
	info->fb_bpp = 32;
	sc->fb_error = fbd_register(info);
}

static void
rk3588_vop2_configure(void *arg)
{
	struct rk3588_vop2_softc *sc;

	sc = arg;
	config_intrhook_disestablish(&sc->config_hook);
	sc->attempted = 1;
	sc->error = rk3588_vop2_get_mode(sc);
	if (sc->error == 0)
		sc->error = rk3588_vop2_prepare_mode(0);
	if (sc->error == 0)
		sc->error = rk3588_hdptx_configure_mode(0, &sc->mode);
	if (sc->error == 0)
		sc->error = rk3588_vop2_fb_alloc(sc);
	if (sc->error == 0)
		sc->error = rk3588_vop2_program(sc);
	sc->configured = sc->error == 0;
	if (bootverbose || sc->error != 0)
		device_printf(sc->dev,
	    "RK3588-DIAG V2272 selected=%ux%u clock=%ju XRGB8888 cluster0 "
	    "attempted=%d "
	    "configured=%d error=%d version=%#x clocks=%ju/%ju/%ju/%ju/%ju "
	    "pd=%#x->%#x status=%#x cfg=%#x->%#x "
	    "if=%#x/%#x->%#x/%#x ifpol=%#x->%#x dsp=%#x->%#x "
	    "bg=%#x colorbar=%#x "
	    "fb=%#jx/%u/%d ovl=%#x->%#x layer=%#x->%#x "
	    "port=%#x->%#x dly=%#x/%#x "
	    "cluster=%#x/%#x/%#x/%#x/%#x/%#x/%#x/%#x/%#x "
	    "win=%#x/%#x/%#x/%#x/%#x/%#x "
	    "frames=%d polls=%d fs=%#x/%#x sys=%#x/%#x/%#x/%#x "
	    "sysirq=%#x/%#x wincfg=%#x "
	    "mmuerr=%d/%d mmu0=%#x/%#x/%#x/%#x->%#x/%#x/%#x/%#x "
	    "mmu1=%#x/%#x/%#x/%#x->%#x/%#x/%#x/%#x grf=%#x/%#x\n",
	    sc->fb_width, sc->fb_height, (uintmax_t)sc->tmds_rate,
	    sc->attempted, sc->configured, sc->error, sc->version,
	    (uintmax_t)sc->clock_rate[0], (uintmax_t)sc->clock_rate[1],
	    (uintmax_t)sc->clock_rate[2], (uintmax_t)sc->clock_rate[3],
	    (uintmax_t)sc->clock_rate[4], sc->pd_before, sc->pd_after,
	    sc->pd_status, sc->cfg_before, sc->cfg_after, sc->if_en_before,
	    sc->if_ctrl_before, sc->if_en_after, sc->if_ctrl_after,
	    sc->if_pol_before, sc->if_pol_after,
	    sc->dsp_ctrl_before, sc->dsp_ctrl_after, sc->bg_after,
	    sc->color_bar_after,
	    (uintmax_t)sc->fb_addr, sc->fb_size, sc->fb_nsegs,
	    sc->ovl_ctrl_before, sc->ovl_ctrl_after, sc->layer_sel_before,
	    sc->layer_sel_after, sc->port_sel_before, sc->port_sel_after,
	    sc->cluster_dly_after, sc->smart_dly_after,
	    sc->cluster_ctrl_after, sc->cluster_ctrl2_after,
	    sc->cluster_mix_after[0], sc->cluster_mix_after[1],
	    sc->cluster_mix_after[2], sc->cluster_mix_after[3],
	    sc->cluster_sub_ctrl_after, sc->cluster_afbc_output_after,
	    sc->cluster_afbc_ctrl_after,
	    sc->window_ctrl_after, sc->window_addr_after, sc->window_vir_after,
	    sc->window_act_after, sc->window_dsp_after, sc->window_st_after,
	    sc->fs_count, sc->fs_polls, sc->fs_status, sc->vp_int_status,
	    sc->sys_status[0], sc->sys_status[1], sc->sys_status[2],
	    sc->sys_status[3], sc->sys_int_status[0], sc->sys_int_status[1],
	    sc->win_cfg_done_after, sc->iommu_before_error,
	    sc->iommu_after_error, sc->mmu_before[0].dte,
	    sc->mmu_before[0].status, sc->mmu_before[0].fault,
	    sc->mmu_before[0].raw, sc->mmu_after[0].dte,
	    sc->mmu_after[0].status, sc->mmu_after[0].fault,
	    sc->mmu_after[0].raw, sc->mmu_before[1].dte,
	    sc->mmu_before[1].status, sc->mmu_before[1].fault,
	    sc->mmu_before[1].raw, sc->mmu_after[1].dte,
	    sc->mmu_after[1].status, sc->mmu_after[1].fault,
	    sc->mmu_after[1].raw, sc->vop_grf_after, sc->vo1_grf_after);
	if (sc->configured)
		rk3588_vop2_fb_register(sc);
	if (sc->configured && sc->fb_error == 0)
		rk3588_vop2_drm_init(sc);
	if (bootverbose || sc->fb_error != 0 || sc->drm_error != 0)
		device_printf(sc->dev,
	    "RK3588-DIAG V2272 framebuffer=%ux%u stride=%u bpp=32 "
	    "registered=%d error=%d DRM-KMS-V2318=%d error=%d fb=%u "
	    "modesets=%d/%d\n", sc->fb_width, sc->fb_height, sc->fb_stride,
	    sc->configured && sc->fb_error == 0, sc->fb_error,
	    sc->drm_inited, sc->drm_error, sc->drm_fb.base.id,
	    sc->drm_modeset_count, sc->drm_modeset_error);
}

int
rk3588_vop2_prepare_mode(int port)
{
	struct rk3588_vop2_softc *sc;
	uint32_t status;
	int i;

	if (port != 0)
		return (EINVAL);
	sc = rk3588_vop2_sc;
	if (sc == NULL)
		return (ENXIO);
	/* Quiesce an active display inherited from firmware before takeover. */
	if (!sc->configured &&
	    (bus_read_4(sc->mem, VOP2_DSP_IF_EN) & VOP2_HDMI0_EN) == 0)
		return (0);

	bus_write_4(sc->mem, VOP2_VP0_INT_CLR,
	    VOP2_VP0_DSP_HOLD_CLEAR);
	bus_barrier(sc->mem, VOP2_VP0_INT_CLR, sizeof(uint32_t),
	    BUS_SPACE_BARRIER_READ | BUS_SPACE_BARRIER_WRITE);
	rk3588_vop2_mod(sc, VOP2_VP0_DSP_CTRL, VOP2_VP0_STANDBY,
	    VOP2_VP0_STANDBY);
	bus_barrier(sc->mem, VOP2_VP0_DSP_CTRL, sizeof(uint32_t),
	    BUS_SPACE_BARRIER_READ | BUS_SPACE_BARRIER_WRITE);
	sc->drm_crtc_hold_polls = 0;
	for (i = 0; i < 1000; i++) {
		status = bus_read_4(sc->mem, VOP2_VP0_INT_RAW_STATUS);
		sc->drm_crtc_hold_status = status;
		sc->drm_crtc_hold_polls++;
		if ((status & VOP2_VP0_DSP_HOLD_VALID) != 0)
			return (0);
		DELAY(50);
	}
	if ((bus_read_4(sc->mem, VOP2_VP0_DSP_CTRL) &
	    VOP2_VP0_STANDBY) == 0)
		return (EIO);
	rk3588_vop2_mod(sc, VOP2_VP0_DSP_CTRL, 0, VOP2_VP0_STANDBY);
	bus_write_4(sc->mem, VOP2_REG_CFG_DONE,
	    VOP2_GLB_CFG_DONE_EN | VOP2_VP0_CFG_DONE);
	bus_barrier(sc->mem, 0, rman_get_size(sc->mem),
	    BUS_SPACE_BARRIER_READ | BUS_SPACE_BARRIER_WRITE);
	return (ETIMEDOUT);
}

static int
rk3588_vop2_release_standby(struct rk3588_vop2_softc *sc)
{
	int error;

	error = rk3588_vop2_clear_fs(sc);
	if (error != 0)
		return (error);
	bus_write_4(sc->mem, VOP2_REG_CFG_DONE,
	    VOP2_GLB_CFG_DONE_EN | VOP2_VP0_CFG_DONE);
	bus_barrier(sc->mem, 0, rman_get_size(sc->mem),
	    BUS_SPACE_BARRIER_WRITE);
	rk3588_vop2_mod(sc, VOP2_VP0_DSP_CTRL, 0, VOP2_VP0_STANDBY);
	bus_barrier(sc->mem, VOP2_VP0_DSP_CTRL, sizeof(uint32_t),
	    BUS_SPACE_BARRIER_READ | BUS_SPACE_BARRIER_WRITE);
	error = rk3588_vop2_wait_fs(sc);
	if (error == 0)
		error = rk3588_vop2_clear_fs(sc);
	return (error);
}

static int
rk3588_vop2_core_clocks_set(struct rk3588_vop2_softc *sc, bool enable)
{
	int error;

	if (enable) {
		if (sc->drm_core_clocks_enabled)
			return (0);
		error = clk_enable(sc->clocks[VOP2_CLK_HCLK]);
		if (error != 0)
			return (error);
		error = clk_enable(sc->clocks[VOP2_CLK_ACLK]);
		if (error != 0) {
			(void)clk_disable(sc->clocks[VOP2_CLK_HCLK]);
			return (error);
		}
		error = clk_enable(sc->clocks[VOP2_CLK_PCLK]);
		if (error != 0) {
			(void)clk_disable(sc->clocks[VOP2_CLK_ACLK]);
			(void)clk_disable(sc->clocks[VOP2_CLK_HCLK]);
			return (error);
		}
		sc->drm_core_clocks_enabled = 1;
		return (0);
	}
	if (!sc->drm_core_clocks_enabled)
		return (0);
	error = clk_disable(sc->clocks[VOP2_CLK_PCLK]);
	if (error != 0)
		return (error);
	error = clk_disable(sc->clocks[VOP2_CLK_ACLK]);
	if (error != 0) {
		(void)clk_enable(sc->clocks[VOP2_CLK_PCLK]);
		return (error);
	}
	error = clk_disable(sc->clocks[VOP2_CLK_HCLK]);
	if (error != 0) {
		(void)clk_enable(sc->clocks[VOP2_CLK_ACLK]);
		(void)clk_enable(sc->clocks[VOP2_CLK_PCLK]);
		return (error);
	}
	sc->drm_core_clocks_enabled = 0;
	return (0);
}

static int
rk3588_vop2_crtc_set_output(struct rk3588_vop2_softc *sc, bool enable)
{
	int error;

	if (enable) {
		if (sc->drm_crtc_dpms_active)
			return (0);
		error = rk3588_vop2_core_clocks_set(sc, true);
		if (error != 0)
			return (error);
		error = clk_enable(sc->clocks[VOP2_CLK_DCLK]);
		if (error != 0) {
			(void)rk3588_vop2_core_clocks_set(sc, false);
			return (error);
		}
		sc->drm_crtc_dclk_enabled = 1;
		error = rk3588_vop2_release_standby(sc);
		if (error != 0) {
			(void)clk_disable(sc->clocks[VOP2_CLK_DCLK]);
			sc->drm_crtc_dclk_enabled = 0;
			(void)rk3588_vop2_core_clocks_set(sc, false);
			return (error);
		}
		sc->drm_crtc_dpms_active = 1;
		return (0);
	}
	if (!sc->drm_crtc_dpms_active)
		return (0);
	mtx_lock(&sc->drm_dev.event_lock);
	if (sc->flip_event != NULL) {
		mtx_unlock(&sc->drm_dev.event_lock);
		return (EBUSY);
	}
	mtx_unlock(&sc->drm_dev.event_lock);
	drm_vblank_off(&sc->drm_dev, 0);
	error = rk3588_vop2_prepare_mode(0);
	if (error != 0)
		return (error);
	error = clk_disable(sc->clocks[VOP2_CLK_DCLK]);
	if (error != 0) {
		(void)rk3588_vop2_release_standby(sc);
		return (error);
	}
	sc->drm_crtc_dclk_enabled = 0;
	error = rk3588_vop2_core_clocks_set(sc, false);
	if (error != 0) {
		(void)clk_enable(sc->clocks[VOP2_CLK_DCLK]);
		sc->drm_crtc_dclk_enabled = 1;
		(void)rk3588_vop2_release_standby(sc);
		return (error);
	}
	sc->drm_crtc_dpms_active = 0;
	return (0);
}

int
rk3588_vop2_configure_mode(int port)
{
	struct rk3588_vop2_softc *sc;
	bool register_fb;

	if (port != 0)
		return (EINVAL);
	sc = rk3588_vop2_sc;
	if (sc == NULL)
		return (ENXIO);

	register_fb = sc->fb_tag == NULL;
	sc->attempted = 1;
	sc->error = rk3588_vop2_get_mode(sc);
	if (sc->error == 0 && register_fb)
		sc->error = rk3588_vop2_fb_alloc(sc);
	if (sc->error == 0)
		sc->error = rk3588_vop2_program(sc);
	sc->configured = sc->error == 0;
	if (sc->error != 0)
		device_printf(sc->dev,
		    "runtime program failed stage=%s error=%d "
		    "clock=%ju/%ju fs=%#x starts=%d polls=%d dsp=%#x\n",
		    sc->program_stage, sc->error,
		    (uintmax_t)sc->clock_rate[VOP2_CLK_DCLK],
		    (uintmax_t)sc->clock_rate[VOP2_CLK_PHY0],
		    sc->fs_status, sc->fs_count, sc->fs_polls,
		    bus_read_4(sc->mem, VOP2_VP0_DSP_CTRL));
	if (sc->configured && register_fb)
		rk3588_vop2_fb_register(sc);
	if (sc->configured && sc->fb_error == 0 && !sc->drm_inited)
		rk3588_vop2_drm_init(sc);
	return (sc->error != 0 ? sc->error :
	    register_fb ? sc->fb_error : 0);
}

static void
rk3588_vop2_add_sysctls(struct rk3588_vop2_softc *sc)
{
	struct sysctl_ctx_list *ctx;
	struct sysctl_oid_list *children;

	ctx = device_get_sysctl_ctx(sc->dev);
	children = SYSCTL_CHILDREN(device_get_sysctl_tree(sc->dev));
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "attempted", CTLFLAG_RD,
	    &sc->attempted, 0, "Selected-mode programming was attempted");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "configured", CTLFLAG_RD,
	    &sc->configured, 0, "Selected-mode register verification passed");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "error", CTLFLAG_RD,
	    &sc->error, 0, "Selected-mode programming error");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "drm_inited", CTLFLAG_RD,
	    &sc->drm_inited, 0, "DRM/KMS object registration completed");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "drm_error", CTLFLAG_RD,
	    &sc->drm_error, 0, "DRM/KMS registration error");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "drm_modeset_count",
	    CTLFLAG_RD, &sc->drm_modeset_count, 0,
	    "Successful DRM legacy modesets");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "drm_modeset_error",
	    CTLFLAG_RD, &sc->drm_modeset_error, 0,
	    "Last DRM legacy modeset error");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "drm_dpms_count",
	    CTLFLAG_RD, &sc->drm_dpms_count, 0,
	    "Successful DRM DPMS transitions");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "drm_dpms_error",
	    CTLFLAG_RD, &sc->drm_dpms_error, 0,
	    "Last DRM DPMS transition error");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "drm_crtc_dpms_active",
	    CTLFLAG_RD, &sc->drm_crtc_dpms_active, 0,
	    "VP0 scanout is active after DRM DPMS");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "drm_crtc_dclk_enabled",
	    CTLFLAG_RD, &sc->drm_crtc_dclk_enabled, 0,
	    "VP0 DCLK is enabled after DRM DPMS");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "drm_core_clocks_enabled",
	    CTLFLAG_RD, &sc->drm_core_clocks_enabled, 0,
	    "VOP2 HCLK, ACLK and PCLK are enabled after DRM DPMS");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "drm_crtc_hold_polls",
	    CTLFLAG_RD, &sc->drm_crtc_hold_polls, 0,
	    "VP0 DSP hold polling iterations");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "drm_crtc_hold_status",
	    CTLFLAG_RD, &sc->drm_crtc_hold_status, 0,
	    "VP0 raw interrupt status at DSP hold");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "vblank_irq_count",
	    CTLFLAG_RD, &sc->vblank_irq_count, 0,
	    "Handled VP0 frame-start-field interrupts");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "vblank_irq_status",
	    CTLFLAG_RD, &sc->vblank_irq_status, 0,
	    "Last handled VP0 interrupt status");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "page_flip_count",
	    CTLFLAG_RD, &sc->page_flip_count, 0,
	    "Completed DRM legacy page flips");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "page_flip_cancel_count",
	    CTLFLAG_RD, &sc->page_flip_cancel_count, 0,
	    "Cancelled DRM legacy page flips");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "page_flip_error",
	    CTLFLAG_RD, &sc->page_flip_error, 0,
	    "Last DRM legacy page flip error");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "version", CTLFLAG_RD,
	    &sc->version, 0, "VOP2 hardware version");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "pd_status", CTLFLAG_RD,
	    &sc->pd_status, 0, "VOP2 internal power-domain status");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "if_en", CTLFLAG_RD,
	    &sc->if_en_after, 0, "VOP2 display interface enable register");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "if_ctrl", CTLFLAG_RD,
	    &sc->if_ctrl_after, 0, "VOP2 display interface divider register");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "dsp_ctrl", CTLFLAG_RD,
	    &sc->dsp_ctrl_after, 0, "VOP2 VP0 display control register");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "background", CTLFLAG_RD,
	    &sc->bg_after, 0, "VOP2 VP0 RGB background value");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "color_bar", CTLFLAG_RD,
	    &sc->color_bar_after, 0, "VOP2 VP0 color-bar control register");
	SYSCTL_ADD_U64(ctx, children, OID_AUTO, "framebuffer_address", CTLFLAG_RD,
	    &sc->fb_addr, 0, "Cluster0 framebuffer DMA address");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "layer_sel", CTLFLAG_RD,
	    &sc->layer_sel_after, 0, "VOP2 overlay layer selection register");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "port_sel", CTLFLAG_RD,
	    &sc->port_sel_after, 0, "VOP2 overlay port selection register");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "cluster_delay", CTLFLAG_RD,
	    &sc->cluster_dly_after, 0, "VOP2 Cluster window delay register");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "cluster_ctrl", CTLFLAG_RD,
	    &sc->cluster_ctrl_after, 0, "VOP2 Cluster0 control register");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "cluster_axi_ids", CTLFLAG_RD,
	    &sc->cluster_ctrl2_after, 0, "Cluster0 window 0 AXI IDs");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "cluster_window_ctrl", CTLFLAG_RD,
	    &sc->window_ctrl_after, 0, "Cluster0 window 0 control register");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "cluster_address", CTLFLAG_RD,
	    &sc->window_addr_after, 0, "Cluster0 window 0 address register");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "frame_starts", CTLFLAG_RD,
	    &sc->fs_count, 0, "Observed VP0 frame-start events");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "frame_polls", CTLFLAG_RD,
	    &sc->fs_polls, 0, "VP0 frame-start status polls");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "frame_status", CTLFLAG_RD,
	    &sc->fs_status, 0, "VP0 raw interrupt status at the last frame start");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "sys0_interrupt", CTLFLAG_RD,
	    &sc->sys_int_status[0], 0, "VOP2 AXI0 interrupt status");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "sys1_interrupt", CTLFLAG_RD,
	    &sc->sys_int_status[1], 0, "VOP2 AXI1 interrupt status");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "vp0_interrupt", CTLFLAG_RD,
	    &sc->vp_int_status, 0, "VP0 masked interrupt status");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "win_cfg_done", CTLFLAG_RD,
	    &sc->win_cfg_done_after, 0, "RK3588 per-window config-done register");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "iommu_before_error", CTLFLAG_RD,
	    &sc->iommu_before_error, 0, "VOP IOMMU pre-program snapshot error");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "iommu_after_error", CTLFLAG_RD,
	    &sc->iommu_after_error, 0, "VOP IOMMU post-program snapshot error");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "iommu0_status", CTLFLAG_RD,
	    &sc->mmu_after[0].status, 0, "VOP IOMMU bank 0 status");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "iommu0_fault", CTLFLAG_RD,
	    &sc->mmu_after[0].fault, 0, "VOP IOMMU bank 0 fault address");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "iommu0_raw", CTLFLAG_RD,
	    &sc->mmu_after[0].raw, 0, "VOP IOMMU bank 0 raw interrupt status");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "iommu1_status", CTLFLAG_RD,
	    &sc->mmu_after[1].status, 0, "VOP IOMMU bank 1 status");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "iommu1_fault", CTLFLAG_RD,
	    &sc->mmu_after[1].fault, 0, "VOP IOMMU bank 1 fault address");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "iommu1_raw", CTLFLAG_RD,
	    &sc->mmu_after[1].raw, 0, "VOP IOMMU bank 1 raw interrupt status");
}

static int
rk3588_vop2_probe(device_t dev)
{

	if (!ofw_bus_status_okay(dev) ||
	    !ofw_bus_is_compatible(dev, "rockchip,rk3588-vop"))
		return (ENXIO);
	device_set_desc(dev, "Rockchip RK3588 VOP2 HDMI0 output");
	return (BUS_PROBE_DEFAULT);
}

static int
rk3588_vop2_attach(device_t dev)
{
	struct rk3588_vop2_softc *sc;
	phandle_t node;
	rman_res_t base, size;
	int error, rid;

	sc = device_get_softc(dev);
	sc->dev = dev;
	node = ofw_bus_get_node(dev);
	if (bus_get_resource(dev, SYS_RES_MEMORY, 0, &base, &size) != 0 ||
	    size < 0x1e14)
		return (ENXIO);
	error = syscon_get_by_ofw_property(dev, node, "rockchip,grf", &sc->grf);
	if (error != 0)
		return (error);
	error = syscon_get_by_ofw_property(dev, node, "rockchip,vop-grf",
	    &sc->vop_grf);
	if (error != 0)
		return (error);
	error = syscon_get_by_ofw_property(dev, node, "rockchip,vo1-grf",
	    &sc->vo1_grf);
	if (error != 0)
		return (error);
	rid = 0;
	sc->mem = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid, RF_ACTIVE);
	if (sc->mem == NULL)
		return (ENXIO);
	bus_write_4(sc->mem, VOP2_VP0_INT_EN, VOP2_VP0_INT_MASK << 16);
	bus_write_4(sc->mem, VOP2_VP0_INT_CLR,
	    (VOP2_VP0_INT_MASK << 16) | VOP2_VP0_INT_MASK);
	rid = 0;
	sc->irq = bus_alloc_resource_any(dev, SYS_RES_IRQ, &rid,
	    RF_ACTIVE | RF_SHAREABLE);
	if (sc->irq == NULL) {
		bus_release_resource(dev, SYS_RES_MEMORY, 0, sc->mem);
		return (ENXIO);
	}
	error = bus_setup_intr(dev, sc->irq, INTR_TYPE_MISC | INTR_MPSAFE,
	    NULL, rk3588_vop2_intr, sc, &sc->irq_cookie);
	if (error != 0) {
		bus_release_resource(dev, SYS_RES_IRQ, 0, sc->irq);
		bus_release_resource(dev, SYS_RES_MEMORY, 0, sc->mem);
		return (error);
	}

	rk3588_vop2_add_sysctls(sc);
	sc->config_hook.ich_func = rk3588_vop2_configure;
	sc->config_hook.ich_arg = sc;
	error = config_intrhook_establish(&sc->config_hook);
	if (error != 0)
		device_printf(dev, "Cannot establish deferred configuration: %d\n",
		    error);
	rk3588_vop2_sc = sc;
	return (0);
}

static device_method_t rk3588_vop2_methods[] = {
	DEVMETHOD(device_probe,		rk3588_vop2_probe),
	DEVMETHOD(device_attach,	rk3588_vop2_attach),

	DEVMETHOD_END
};

static driver_t rk3588_vop2_driver = {
	"rk3588_vop2",
	rk3588_vop2_methods,
	sizeof(struct rk3588_vop2_softc),
};

EARLY_DRIVER_MODULE(rk3588_vop2, simplebus, rk3588_vop2_driver, 0, 0,
    BUS_PASS_DEFAULT);
MODULE_VERSION(rk3588_vop2, 1);
