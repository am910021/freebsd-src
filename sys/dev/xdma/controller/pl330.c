/*-
 * Copyright (c) 2017-2018 Ruslan Bukin <br@bsdpad.com>
 * All rights reserved.
 *
 * This software was developed by SRI International and the University of
 * Cambridge Computer Laboratory under DARPA/AFRL contract FA8750-10-C-0237
 * ("CTSRD"), as part of the DARPA CRASH research programme.
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
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/* ARM PrimeCell DMA Controller (PL330) driver. */

#include <sys/cdefs.h>
#include "opt_platform.h"
#include <sys/param.h>
#include <sys/endian.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/kthread.h>
#include <sys/sglist.h>
#include <sys/module.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/resource.h>
#include <sys/rman.h>
#include <sys/sysctl.h>

#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_kern.h>
#include <vm/pmap.h>

#include <machine/bus.h>

#include <dev/extres/clk/clk.h>

#ifdef FDT
#include <dev/fdt/fdt_common.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#endif

#include <dev/xdma/xdma.h>
#include <dev/xdma/controller/pl330.h>

#include "xdma_if.h"

#define PL330_DEBUG
#undef PL330_DEBUG

#ifdef PL330_DEBUG
#define dprintf(fmt, ...)  printf(fmt, ##__VA_ARGS__)
#else
#define dprintf(fmt, ...)
#endif

#define	READ4(_sc, _reg)	\
	bus_read_4(_sc->res[0], _reg)
#define	WRITE4(_sc, _reg, _val)	\
	bus_write_4(_sc->res[0], _reg, _val)

#define	PL330_NCHANNELS	32
#define	PL330_MAXLOAD	2048
#define	PL330_IBUF_SIZE	PAGE_SIZE
#define	PL330_SELFTEST_LEN	4096
#define	PL330_SELFTEST_TIMEOUT	(2 * hz)

struct pl330_channel {
	struct pl330_softc	*sc;
	xdma_channel_t		*xchan;
	int			used;
	int			index;
	uint8_t			*ibuf;
	bus_addr_t		ibuf_phys;
	bus_dma_tag_t		ibuf_tag;
	bus_dmamap_t		ibuf_map;
	uint32_t		enqueued;
	uint32_t		capacity;
	uint32_t		transfer_len;
	uint32_t		program_len;
	bool			active;
};

struct pl330_fdt_data {
	uint32_t periph_id;
};

struct pl330_softc {
	device_t		dev;
	struct resource		*res[PL330_NCHANNELS + 1];
	void			*ih[PL330_NCHANNELS];
	struct pl330_channel	channels[PL330_NCHANNELS];
	clk_t			pclk;
	struct mtx		test_mtx;
	uint32_t		nchannels;
	uint32_t		num_events;
	uint32_t		cr0;
	int			nirqs;
	int			test_done;
	int			test_error;
	bool			test_running;
	int			selftest_result;
};

struct pl330_test_mem {
	bus_dma_tag_t		tag;
	bus_dmamap_t		src_map;
	bus_dmamap_t		dst_map;
	uint8_t			*src;
	uint8_t			*dst;
	bus_addr_t		src_phys;
	bus_addr_t		dst_phys;
};

static struct resource_spec pl330_spec[] = {
	{ SYS_RES_MEMORY,	0,	RF_ACTIVE },
	{ SYS_RES_IRQ,		0,	RF_ACTIVE },
	{ SYS_RES_IRQ,		1,	RF_ACTIVE | RF_OPTIONAL },
	{ SYS_RES_IRQ,		2,	RF_ACTIVE | RF_OPTIONAL },
	{ SYS_RES_IRQ,		3,	RF_ACTIVE | RF_OPTIONAL },
	{ SYS_RES_IRQ,		4,	RF_ACTIVE | RF_OPTIONAL },
	{ SYS_RES_IRQ,		5,	RF_ACTIVE | RF_OPTIONAL },
	{ SYS_RES_IRQ,		6,	RF_ACTIVE | RF_OPTIONAL },
	{ SYS_RES_IRQ,		7,	RF_ACTIVE | RF_OPTIONAL },
	{ SYS_RES_IRQ,		8,	RF_ACTIVE | RF_OPTIONAL },
	{ SYS_RES_IRQ,		9,	RF_ACTIVE | RF_OPTIONAL },
	{ SYS_RES_IRQ,		10,	RF_ACTIVE | RF_OPTIONAL },
	{ SYS_RES_IRQ,		11,	RF_ACTIVE | RF_OPTIONAL },
	{ SYS_RES_IRQ,		12,	RF_ACTIVE | RF_OPTIONAL },
	{ SYS_RES_IRQ,		13,	RF_ACTIVE | RF_OPTIONAL },
	{ SYS_RES_IRQ,		14,	RF_ACTIVE | RF_OPTIONAL },
	{ SYS_RES_IRQ,		15,	RF_ACTIVE | RF_OPTIONAL },
	{ SYS_RES_IRQ,		16,	RF_ACTIVE | RF_OPTIONAL },
	{ SYS_RES_IRQ,		17,	RF_ACTIVE | RF_OPTIONAL },
	{ SYS_RES_IRQ,		18,	RF_ACTIVE | RF_OPTIONAL },
	{ SYS_RES_IRQ,		19,	RF_ACTIVE | RF_OPTIONAL },
	{ SYS_RES_IRQ,		20,	RF_ACTIVE | RF_OPTIONAL },
	{ SYS_RES_IRQ,		21,	RF_ACTIVE | RF_OPTIONAL },
	{ SYS_RES_IRQ,		22,	RF_ACTIVE | RF_OPTIONAL },
	{ SYS_RES_IRQ,		23,	RF_ACTIVE | RF_OPTIONAL },
	{ SYS_RES_IRQ,		24,	RF_ACTIVE | RF_OPTIONAL },
	{ SYS_RES_IRQ,		25,	RF_ACTIVE | RF_OPTIONAL },
	{ SYS_RES_IRQ,		26,	RF_ACTIVE | RF_OPTIONAL },
	{ SYS_RES_IRQ,		27,	RF_ACTIVE | RF_OPTIONAL },
	{ SYS_RES_IRQ,		28,	RF_ACTIVE | RF_OPTIONAL },
	{ SYS_RES_IRQ,		29,	RF_ACTIVE | RF_OPTIONAL },
	{ SYS_RES_IRQ,		30,	RF_ACTIVE | RF_OPTIONAL },
	{ SYS_RES_IRQ,		31,	RF_ACTIVE | RF_OPTIONAL },
	{ -1, 0 }
};

#define	HWTYPE_NONE	0
#define	HWTYPE_STD	1

static struct ofw_compat_data compat_data[] = {
	{ "arm,pl330",		HWTYPE_STD },
	{ NULL,			HWTYPE_NONE },
};

static int pl330_selftest_sysctl(SYSCTL_HANDLER_ARGS);
static void pl330_dmamap_cb(void *, bus_dma_segment_t *, int, int);

static void
pl330_intr(void *arg)
{
	xdma_transfer_status_t status;
	struct xdma_transfer_status st;
	struct pl330_channel *chan;
	struct xdma_channel *xchan;
	struct pl330_softc *sc;
	uint32_t faults, manager_fault, pending;
	uint32_t mask;
	uint32_t transferred;
	int error;
	int i;
	int c;

	sc = arg;

	pending = READ4(sc, INTMIS);
	faults = READ4(sc, FSRC);
	manager_fault = READ4(sc, FSRD);

	if (manager_fault != 0)
		device_printf(sc->dev, "manager fault %#x type %#x\n",
		    manager_fault, READ4(sc, FTRD));

	WRITE4(sc, INTCLR, pending);

	for (c = 0; c < sc->nchannels; c++) {
		mask = 1u << c;
		error = ((faults & mask) != 0 || manager_fault != 0) ? EIO : 0;
		if ((pending & mask) == 0 && error == 0)
			continue;
		chan = &sc->channels[c];
		if (!chan->used || !chan->active || chan->xchan == NULL)
			continue;
		if (error != 0)
			device_printf(sc->dev,
			    "channel %d fault status %#x type %#x\n", c,
			    faults, READ4(sc, FTR(c)));

		WRITE4(sc, INTEN, READ4(sc, INTEN) & ~mask);
		chan->active = false;
		bus_dmamap_sync(chan->ibuf_tag, chan->ibuf_map,
		    BUS_DMASYNC_POSTWRITE);
		xchan = chan->xchan;
		st.error = error;
		transferred = 0;
		for (i = 0; i < chan->enqueued; i++) {
			if (error == 0)
				transferred += xchan->sg[i].len;
			st.transferred = transferred;
			xchan_seg_done(xchan, &st);
			if (xchan->sg[i].last)
				transferred = 0;
		}

		/* Accept new requests. */
		chan->capacity = PL330_MAXLOAD;

		/* Finish operation */
		status.error = error;
		status.transferred = st.transferred;
		xdma_callback(chan->xchan, &status);
	}
}

static uint32_t
emit_mov(uint8_t *buf, uint32_t reg, uint32_t val)
{

	buf[0] = DMAMOV;
	buf[1] = reg;
	buf[2] = val;
	buf[3] = val >> 8;
	buf[4] = val >> 16;
	buf[5] = val >> 24;

	return (6);
}

static uint32_t
emit_lp(uint8_t *buf, uint8_t idx, uint32_t iter)
{

	if (idx > 1)
		return (0); /* We have two loops only. */

	buf[0] = DMALP;
	buf[0] |= (idx << 1);
	buf[1] = (iter - 1) & 0xff;

	return (2);
}

static uint32_t
emit_lpend(uint8_t *buf, uint8_t idx,
    uint8_t burst, uint8_t jump_addr_relative)
{

	buf[0] = DMALPEND;
	buf[0] |= DMALPEND_NF;
	buf[0] |= (idx << 2);
	if (burst)
		buf[0] |= (1 << 1) | (1 << 0);
	else
		buf[0] |= (0 << 1) | (1 << 0);
	buf[1] = jump_addr_relative;

	return (2);
}

static uint32_t
emit_ld(uint8_t *buf, uint8_t burst)
{

	buf[0] = DMALD;
	if (burst)
		buf[0] |= (1 << 1) | (1 << 0);
	else
		buf[0] |= (0 << 1) | (1 << 0);

	return (1);
}

static uint32_t
emit_st(uint8_t *buf, uint8_t burst)
{

	buf[0] = DMAST;
	if (burst)
		buf[0] |= (1 << 1) | (1 << 0);
	else
		buf[0] |= (0 << 1) | (1 << 0);

	return (1);
}

static uint32_t
emit_end(uint8_t *buf)
{

	buf[0] = DMAEND;

	return (1);
}

static uint32_t
emit_sev(uint8_t *buf, uint32_t ev)
{

	buf[0] = DMASEV;
	buf[1] = (ev << 3);

	return (2);
}

static uint32_t
emit_flushp(uint8_t *buf, uint32_t p_id)
{

	buf[0] = DMAFLUSHP;
	buf[1] = (p_id << 3);

	return (2);
}

static uint32_t
emit_ldp(uint8_t *buf, uint32_t p_id, bool burst)
{

	buf[0] = DMALDP | (burst ? (1 << 1) : 0);
	buf[1] = (p_id << 3);

	return (2);
}

static uint32_t
emit_stp(uint8_t *buf, uint32_t p_id, bool burst)
{

	buf[0] = DMASTP | (burst ? (1 << 1) : 0);
	buf[1] = (p_id << 3);

	return (2);
}

static uint32_t
emit_wfp(uint8_t *buf, uint32_t p_id, bool burst)
{

	buf[0] = DMAWFP | (burst ? (1 << 1) : 0);
	buf[1] = (p_id << 3);

	return (2);
}

static uint32_t
emit_peripheral_body(uint8_t *buf, enum xdma_direction direction,
    uint32_t p_id)
{
	uint32_t offs;

	offs = emit_wfp(buf, p_id, false);
	if (direction == XDMA_MEM_TO_DEV) {
		offs += emit_ld(&buf[offs], false);
		offs += emit_stp(&buf[offs], p_id, false);
	} else {
		offs += emit_ldp(&buf[offs], p_id, false);
		offs += emit_st(&buf[offs], false);
	}

	return (offs);
}

static uint32_t
peripheral_program_size(uint32_t count)
{
	uint32_t size;

	size = (count / 65536) * 13;
	count %= 65536;
	if (count > 256) {
		size += 13;
		count %= 256;
	}
	if (count != 0)
		size += 9;

	return (size);
}

static uint32_t
emit_peripheral(uint8_t *buf, enum xdma_direction direction,
    uint32_t p_id, uint32_t count)
{
	uint32_t inner_start, outer_count, outer_start, offs;

	offs = 0;
	while (count > 256) {
		outer_count = MIN(count / 256, 256);
		offs += emit_lp(&buf[offs], 0, outer_count);
		outer_start = offs;
		offs += emit_lp(&buf[offs], 1, 256);
		inner_start = offs;
		offs += emit_peripheral_body(&buf[offs], direction, p_id);
		offs += emit_lpend(&buf[offs], 1, false,
		    offs - inner_start);
		offs += emit_lpend(&buf[offs], 0, false,
		    offs - outer_start);
		count -= outer_count * 256;
	}
	if (count != 0) {
		offs += emit_lp(&buf[offs], 0, count);
		inner_start = offs;
		offs += emit_peripheral_body(&buf[offs], direction, p_id);
		offs += emit_lpend(&buf[offs], 0, false,
		    offs - inner_start);
	}

	return (offs);
}

static uint32_t
emit_go(uint8_t *buf, uint32_t chan_id,
    uint32_t addr, uint8_t non_secure)
{

	buf[0] = DMAGO;
	buf[0] |= (non_secure << 1);

	buf[1] = chan_id;
	buf[2] = addr;
	buf[3] = addr >> 8;
	buf[4] = addr >> 16;
	buf[5] = addr >> 24;

	return (6);
}

static uint32_t
emit_lpend_always(uint8_t *buf, uint8_t idx, uint8_t jump)
{

	buf[0] = DMALPEND | DMALPEND_NF | (idx << 2);
	buf[1] = jump;

	return (2);
}

static int
pl330_channel_request(device_t dev, struct xdma_channel *xchan,
    struct xdma_request *req)
{
	struct pl330_channel *chan;
	struct pl330_softc *sc;
	bus_size_t len;
	uint32_t burst_len, bursts, ccr, offs, reg;
	uint32_t loop_start;
	uint8_t dbuf[6];
	uint8_t width_shift;
	uint8_t width;

	sc = device_get_softc(dev);
	chan = xchan->chan;
	if (req->operation != XDMA_MEMCPY ||
	    req->direction != XDMA_MEM_TO_MEM ||
	    req->req_type != XR_TYPE_PHYS)
		return (EOPNOTSUPP);
	if (req->block_num == 0 ||
	    req->block_len > SIZE_MAX / req->block_num)
		return (EINVAL);
	len = req->block_len * req->block_num;
	width = req->src_width;
	if (width != req->dst_width ||
	    (width != 1 && width != 2 && width != 4 && width != 8) ||
	    len == 0 || (len % width) != 0 ||
	    (req->src_addr & (width - 1)) != 0 ||
	    (req->dst_addr & (width - 1)) != 0 ||
	    req->src_addr > BUS_SPACE_MAXADDR_32BIT ||
	    req->dst_addr > BUS_SPACE_MAXADDR_32BIT)
		return (EINVAL);
	if (chan->active || (READ4(sc, DBGSTATUS) & DBGSTATUS_BUSY) != 0)
		return (EBUSY);

	burst_len = MIN((bus_size_t)16, len / width);
	while (((len / width) % burst_len) != 0)
		burst_len--;
	bursts = len / width / burst_len;
	if (bursts == 0 || bursts > 256)
		return (EFBIG);

	width_shift = ffs(width) - 1;
	ccr = CCR_SRC_INC | CCR_DST_INC |
	    (width_shift << CCR_SRC_BURST_SIZE_S) |
	    (width_shift << CCR_DST_BURST_SIZE_S) |
	    ((burst_len - 1) << CCR_SRC_BURST_LEN_S) |
	    ((burst_len - 1) << CCR_DST_BURST_LEN_S);
	if ((sc->cr0 & CR0_BOOT_MAN_NS) != 0)
		ccr |= CCR_SRC_NS | CCR_DST_NS;

	bzero(chan->ibuf, PL330_IBUF_SIZE);
	offs = 0;
	offs += emit_mov(&chan->ibuf[offs], R_SAR, (uint32_t)req->src_addr);
	offs += emit_mov(&chan->ibuf[offs], R_DAR, (uint32_t)req->dst_addr);
	offs += emit_mov(&chan->ibuf[offs], R_CCR, ccr);
	offs += emit_lp(&chan->ibuf[offs], 0, bursts);
	loop_start = offs;
	chan->ibuf[offs++] = DMALD;
	chan->ibuf[offs++] = DMAST;
	offs += emit_lpend_always(&chan->ibuf[offs], 0, offs - loop_start);
	offs += emit_sev(&chan->ibuf[offs], chan->index);
	offs += emit_end(&chan->ibuf[offs]);
	KASSERT(offs <= PL330_IBUF_SIZE, ("PL330 program overflow"));

	emit_go(dbuf, chan->index, chan->ibuf_phys,
	    (sc->cr0 & CR0_BOOT_MAN_NS) != 0);
	reg = (dbuf[1] << 24) | (dbuf[0] << 16);
	WRITE4(sc, DBGINST0, reg);
	reg = (dbuf[5] << 24) | (dbuf[4] << 16) |
	    (dbuf[3] << 8) | dbuf[2];
	WRITE4(sc, DBGINST1, reg);

	bus_dmamap_sync(chan->ibuf_tag, chan->ibuf_map,
	    BUS_DMASYNC_PREWRITE);
	WRITE4(sc, INTCLR, 1u << chan->index);
	WRITE4(sc, INTEN, READ4(sc, INTEN) | (1u << chan->index));
	chan->enqueued = 0;
	chan->transfer_len = len;
	chan->program_len = offs;
	chan->active = true;
	WRITE4(sc, DBGCMD, 0);

	return (0);
}

static void
pl330_dump_channel(struct pl330_channel *chan, const char *reason)
{
	struct pl330_softc *sc;

	sc = chan->sc;
	device_printf(sc->dev,
	    "%s ch%d DS=%#x DPC=%#x DBGSTATUS=%#x FSRD=%#x FSRC=%#x "
	    "CSR=%#x CPC=%#x SAR=%#x DAR=%#x CCR=%#x\n", reason,
	    chan->index, READ4(sc, DSR), READ4(sc, DPC),
	    READ4(sc, DBGSTATUS), READ4(sc, FSRD), READ4(sc, FSRC),
	    READ4(sc, CSR(chan->index)), READ4(sc, CPC(chan->index)),
	    READ4(sc, SAR(chan->index)), READ4(sc, DAR(chan->index)),
	    READ4(sc, CCR(chan->index)));
	device_printf(sc->dev, "ch%d program pa=%#jx len=%u: %*D\n",
	    chan->index, (uintmax_t)chan->ibuf_phys, chan->program_len,
	    (int)chan->program_len, chan->ibuf, " ");
}

static int
pl330_probe(device_t dev)
{
	int hwtype;

	if (!ofw_bus_status_okay(dev))
		return (ENXIO);

	hwtype = ofw_bus_search_compatible(dev, compat_data)->ocd_data;
	if (hwtype == HWTYPE_NONE)
		return (ENXIO);

	device_set_desc(dev, "ARM PrimeCell DMA Controller (PL330)");

	return (BUS_PROBE_DEFAULT);
}

static int
pl330_attach(device_t dev)
{
	struct pl330_softc *sc;
	struct sysctl_ctx_list *ctx;
	struct sysctl_oid *tree;
	phandle_t xref, node;
	int err;
	int i;

	sc = device_get_softc(dev);
	sc->dev = dev;
	node = ofw_bus_get_node(dev);

	if (OF_hasprop(node, "clocks")) {
		err = clk_get_by_ofw_name(dev, node, "apb_pclk", &sc->pclk);
		if (err != 0)
			err = clk_get_by_ofw_index(dev, node, 0, &sc->pclk);
		if (err != 0) {
			device_printf(dev, "cannot get bus clock: %d\n", err);
			return (err);
		}
		err = clk_enable(sc->pclk);
		if (err != 0) {
			device_printf(dev, "cannot enable bus clock: %d\n", err);
			clk_release(sc->pclk);
			sc->pclk = NULL;
			return (err);
		}
	}

	if (bus_alloc_resources(dev, pl330_spec, sc->res)) {
		device_printf(dev, "could not allocate resources for device\n");
		err = ENXIO;
		goto fail;
	}

	sc->cr0 = READ4(sc, CR0);
	sc->nchannels = ((sc->cr0 >> CR0_NUM_CHANS_SHIFT) &
	    CR0_NUM_CHANS_MASK) + 1;
	sc->num_events = ((sc->cr0 >> CR0_NUM_EVENTS_SHIFT) &
	    CR0_NUM_EVENTS_MASK) + 1;
	if (sc->nchannels > PL330_NCHANNELS ||
	    sc->nchannels > sc->num_events) {
		device_printf(dev, "invalid hardware configuration: %u channels, "
		    "%u events\n", sc->nchannels, sc->num_events);
		err = ENXIO;
		goto fail;
	}

	/* Setup interrupt handler */
	for (i = 0; i < PL330_NCHANNELS; i++) {
		if (sc->res[i + 1] == NULL)
			break;
		err = bus_setup_intr(dev, sc->res[i + 1], INTR_TYPE_MISC | INTR_MPSAFE,
		    NULL, pl330_intr, sc, &sc->ih[i]);
		if (err) {
			device_printf(dev, "Unable to alloc interrupt resource.\n");
			goto fail;
		}
		sc->nirqs++;
	}
	if (sc->nirqs == 0) {
		err = ENXIO;
		goto fail;
	}

	mtx_init(&sc->test_mtx, device_get_nameunit(dev), "pl330 test",
	    MTX_DEF);
	xref = OF_xref_from_node(node);
	OF_device_register_xref(xref, dev);

	ctx = device_get_sysctl_ctx(dev);
	tree = device_get_sysctl_tree(dev);
	SYSCTL_ADD_U32(ctx, SYSCTL_CHILDREN(tree), OID_AUTO, "channels",
	    CTLFLAG_RD, &sc->nchannels, 0, "Implemented DMA channels");
	SYSCTL_ADD_U32(ctx, SYSCTL_CHILDREN(tree), OID_AUTO, "events",
	    CTLFLAG_RD, &sc->num_events, 0, "Implemented event lines");
	SYSCTL_ADD_INT(ctx, SYSCTL_CHILDREN(tree), OID_AUTO, "selftest_result",
	    CTLFLAG_RD, &sc->selftest_result, 0,
	    "Last self-test result: 1 pass, negative errno fail, 0 not run");
	SYSCTL_ADD_PROC(ctx, SYSCTL_CHILDREN(tree), OID_AUTO, "selftest",
	    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE, sc, 0,
	    pl330_selftest_sysctl, "I", "Write 1 to run a 4 KiB memcpy test");

	device_printf(dev, "%u channels, %u events, %d IRQs, manager %s\n",
	    sc->nchannels, sc->num_events, sc->nirqs,
	    (sc->cr0 & CR0_BOOT_MAN_NS) != 0 ? "non-secure" : "secure");

	return (0);

fail:
	while (sc->nirqs > 0) {
		i = --sc->nirqs;
		bus_teardown_intr(dev, sc->res[i + 1], sc->ih[i]);
	}
	bus_release_resources(dev, pl330_spec, sc->res);
	if (sc->pclk != NULL) {
		clk_disable(sc->pclk);
		clk_release(sc->pclk);
		sc->pclk = NULL;
	}
	return (err);
}

static int
pl330_detach(device_t dev)
{
	struct pl330_softc *sc;
	int i;

	sc = device_get_softc(dev);
	for (i = 0; i < sc->nchannels; i++) {
		if (sc->channels[i].used)
			return (EBUSY);
	}
	while (sc->nirqs > 0) {
		i = --sc->nirqs;
		bus_teardown_intr(dev, sc->res[i + 1], sc->ih[i]);
	}
	bus_release_resources(dev, pl330_spec, sc->res);
	if (sc->pclk != NULL) {
		clk_disable(sc->pclk);
		clk_release(sc->pclk);
	}
	mtx_destroy(&sc->test_mtx);

	return (0);
}

static int
pl330_channel_alloc(device_t dev, struct xdma_channel *xchan)
{
	struct pl330_channel *chan;
	struct pl330_softc *sc;
	int error;
	int i;

	sc = device_get_softc(dev);

	for (i = 0; i < sc->nchannels; i++) {
		chan = &sc->channels[i];
		if (chan->used == 0) {
			chan->index = i;
			chan->sc = sc;
			error = bus_dma_tag_create(bus_get_dma_tag(dev), 16, 0,
			    BUS_SPACE_MAXADDR_32BIT, BUS_SPACE_MAXADDR, NULL, NULL,
			    PL330_IBUF_SIZE, 1, PL330_IBUF_SIZE, 0, NULL, NULL,
			    &chan->ibuf_tag);
			if (error != 0)
				return (error);
			error = bus_dmamem_alloc(chan->ibuf_tag,
			    (void **)&chan->ibuf, BUS_DMA_WAITOK | BUS_DMA_ZERO,
			    &chan->ibuf_map);
			if (error != 0) {
				bus_dma_tag_destroy(chan->ibuf_tag);
				chan->ibuf_tag = NULL;
				return (error);
			}
			chan->ibuf_phys = BUS_SPACE_MAXADDR;
			error = bus_dmamap_load(chan->ibuf_tag, chan->ibuf_map,
			    chan->ibuf, PL330_IBUF_SIZE, pl330_dmamap_cb,
			    &chan->ibuf_phys, BUS_DMA_WAITOK);
			if (error != 0 ||
			    chan->ibuf_phys == BUS_SPACE_MAXADDR) {
				bus_dmamem_free(chan->ibuf_tag, chan->ibuf,
				    chan->ibuf_map);
				bus_dma_tag_destroy(chan->ibuf_tag);
				chan->ibuf = NULL;
				chan->ibuf_tag = NULL;
				return (error != 0 ? error : EFAULT);
			}

			chan->xchan = xchan;
			xchan->chan = chan;
			xchan->caps |= XCHAN_CAP_BUSDMA;
			chan->used = 1;

			return (0);
		}
	}

	return (-1);
}

static int
pl330_channel_free(device_t dev, struct xdma_channel *xchan)
{
	struct pl330_channel *chan;

	chan = (struct pl330_channel *)xchan->chan;
	if (chan->active)
		return (EBUSY);
	bus_dmamap_unload(chan->ibuf_tag, chan->ibuf_map);
	bus_dmamem_free(chan->ibuf_tag, chan->ibuf, chan->ibuf_map);
	bus_dma_tag_destroy(chan->ibuf_tag);
	chan->ibuf = NULL;
	chan->ibuf_phys = 0;
	chan->ibuf_tag = NULL;
	chan->xchan = NULL;
	chan->used = 0;

	return (0);
}

static int
pl330_channel_kill(struct pl330_channel *chan)
{
	struct pl330_softc *sc;
	uint32_t reg;
	int timeout;

	if (!chan->active)
		return (0);
	sc = chan->sc;
	for (timeout = 5000;
	    (READ4(sc, DBGSTATUS) & DBGSTATUS_BUSY) != 0 && timeout > 0;
	    timeout--)
		DELAY(1);
	if (timeout == 0)
		return (ETIMEDOUT);

	reg = (DMAKILL << 16) | (chan->index << 8) | 1;
	WRITE4(sc, DBGINST0, reg);
	WRITE4(sc, DBGINST1, 0);
	WRITE4(sc, DBGCMD, 0);
	WRITE4(sc, INTEN, READ4(sc, INTEN) & ~(1u << chan->index));
	WRITE4(sc, INTCLR, 1u << chan->index);
	chan->active = false;
	bus_dmamap_sync(chan->ibuf_tag, chan->ibuf_map,
	    BUS_DMASYNC_POSTWRITE);

	return (0);
}

static int
pl330_channel_capacity(device_t dev, xdma_channel_t *xchan,
    uint32_t *capacity)
{
	struct pl330_channel *chan;

	chan = (struct pl330_channel *)xchan->chan;

	*capacity = chan->capacity;

	return (0);
}

static int
pl330_ccr_port_width(struct xdma_sglist *sg, uint32_t *addr)
{
	uint32_t reg;

	reg = 0;

	switch (sg->src_width) {
	case 1:
		reg |= CCR_SRC_BURST_SIZE_1;
		break;
	case 2:
		reg |= CCR_SRC_BURST_SIZE_2;
		break;
	case 4:
		reg |= CCR_SRC_BURST_SIZE_4;
		break;
	default:
		return (-1);
	}

	switch (sg->dst_width) {
	case 1:
		reg |= CCR_DST_BURST_SIZE_1;
		break;
	case 2:
		reg |= CCR_DST_BURST_SIZE_2;
		break;
	case 4:
		reg |= CCR_DST_BURST_SIZE_4;
		break;
	default:
		return (-1);
	}

	*addr |= reg;

	return (0);
}

static int
pl330_channel_submit_sg(device_t dev, struct xdma_channel *xchan,
    struct xdma_sglist *sg, uint32_t sg_n)
{
	struct pl330_fdt_data *data;
	xdma_controller_t *xdma;
	struct pl330_channel *chan;
	struct pl330_softc *sc;
	uint32_t src_addr_lo;
	uint32_t dst_addr_lo;
	uint32_t len, total_len;
	uint32_t need;
	uint32_t reg;
	uint32_t offs;
	uint32_t cnt;
	uint8_t *ibuf;
	uint8_t dbuf[6];
	int err;
	int i;

	sc = device_get_softc(dev);

	xdma = xchan->xdma;
	data = (struct pl330_fdt_data *)xdma->data;
	if (data == NULL || data->periph_id >= 32 || sg_n == 0)
		return (EINVAL);

	chan = (struct pl330_channel *)xchan->chan;
	if (chan->active)
		return (EBUSY);
	ibuf = chan->ibuf;
	bzero(ibuf, PL330_IBUF_SIZE);

	dprintf("%s: chan->index %d\n", __func__, chan->index);

	offs = 0;
	total_len = 0;

	for (i = 0; i < sg_n; i++) {
		if (sg[i].direction == XDMA_DEV_TO_MEM)
			reg = CCR_DST_INC;
		else if (sg[i].direction == XDMA_MEM_TO_DEV) {
			reg = CCR_SRC_INC;
			reg |= (CCR_DST_PROT_PRIV);
		} else
			return (EINVAL);
		if ((sc->cr0 & CR0_BOOT_MAN_NS) != 0)
			reg |= CCR_SRC_NS | CCR_DST_NS;

		if (sg[i].src_width != sg[i].dst_width)
			return (EINVAL);
		err = pl330_ccr_port_width(&sg[i], &reg);
		if (err != 0)
			return (EINVAL);

		if (sg[i].len == 0 || sg[i].len > UINT32_MAX ||
		    (sg[i].len % sg[i].src_width) != 0 ||
		    sg[i].src_addr > BUS_SPACE_MAXADDR_32BIT ||
		    sg[i].dst_addr > BUS_SPACE_MAXADDR_32BIT)
			return (EINVAL);
		src_addr_lo = (uint32_t)sg[i].src_addr;
		dst_addr_lo = (uint32_t)sg[i].dst_addr;
		len = (uint32_t)sg[i].len;
		if (UINT32_MAX - total_len < len)
			return (EINVAL);
		total_len += len;
		cnt = len / sg[i].src_width;
		need = 20 + peripheral_program_size(cnt);
		if (offs > PL330_IBUF_SIZE - 3 ||
		    need > PL330_IBUF_SIZE - 3 - offs)
			return (EFBIG);

		dprintf("%s: src %x dst %x len %d periph_id %d\n", __func__,
		    src_addr_lo, dst_addr_lo, len, data->periph_id);

		offs += emit_mov(&ibuf[offs], R_CCR, reg);
		offs += emit_mov(&ibuf[offs], R_SAR, src_addr_lo);
		offs += emit_mov(&ibuf[offs], R_DAR, dst_addr_lo);
		offs += emit_flushp(&ibuf[offs], data->periph_id);
		offs += emit_peripheral(&ibuf[offs], sg[i].direction,
		    data->periph_id, cnt);
	}

	offs += emit_sev(&ibuf[offs], chan->index);
	offs += emit_end(&ibuf[offs]);
	if (offs > PL330_IBUF_SIZE)
		return (EFBIG);

	emit_go(dbuf, chan->index, chan->ibuf_phys,
	    (sc->cr0 & CR0_BOOT_MAN_NS) != 0);

	reg = (dbuf[1] << 24) | (dbuf[0] << 16);
	WRITE4(sc, DBGINST0, reg);
	reg = (dbuf[5] << 24) | (dbuf[4] << 16) | (dbuf[3] << 8) | dbuf[2];
	WRITE4(sc, DBGINST1, reg);

	bus_dmamap_sync(chan->ibuf_tag, chan->ibuf_map,
	    BUS_DMASYNC_PREWRITE);
	WRITE4(sc, INTCLR, 1u << chan->index);
	WRITE4(sc, INTEN, READ4(sc, INTEN) | (1u << chan->index));

	chan->enqueued = sg_n;
	chan->capacity = 0;
	chan->transfer_len = total_len;
	chan->program_len = offs;
	chan->active = true;

	/* Start operation */
	WRITE4(sc, DBGCMD, 0);

	return (0);
}

static int
pl330_channel_prep_sg(device_t dev, struct xdma_channel *xchan)
{
	struct pl330_channel *chan;

	dprintf("%s(%d)\n", __func__, device_get_unit(dev));

	chan = (struct pl330_channel *)xchan->chan;
	chan->capacity = PL330_MAXLOAD;

	return (0);
}

static int
pl330_channel_control(device_t dev, xdma_channel_t *xchan, int cmd)
{
	switch (cmd) {
	case XDMA_CMD_BEGIN:
		return (0);
	case XDMA_CMD_TERMINATE:
		return (pl330_channel_kill(xchan->chan));
	case XDMA_CMD_PAUSE:
		return (EOPNOTSUPP);
	}

	return (EINVAL);
}

#ifdef FDT
static int
pl330_ofw_md_data(device_t dev, pcell_t *cells, int ncells, void **ptr)
{
	struct pl330_fdt_data *data;

	if (ncells != 1)
		return (-1);

	data = malloc(sizeof(struct pl330_fdt_data),
	    M_DEVBUF, (M_WAITOK | M_ZERO));
	data->periph_id = cells[0];

	*ptr = data;

	return (0);
}
#endif

static void
pl330_dmamap_cb(void *arg, bus_dma_segment_t *segs, int nseg, int error)
{
	bus_addr_t *addr;

	if (error != 0 || nseg != 1)
		return;
	addr = arg;
	*addr = segs[0].ds_addr;
}

static int
pl330_selftest_intr(void *arg, xdma_transfer_status_t *status)
{
	struct pl330_softc *sc;

	sc = arg;
	mtx_lock(&sc->test_mtx);
	sc->test_error = status->error;
	sc->test_done = 1;
	wakeup(&sc->test_done);
	mtx_unlock(&sc->test_mtx);

	return (0);
}

static int
pl330_selftest(struct pl330_softc *sc)
{
	struct pl330_test_mem mem;
	struct xdma_request req;
	xdma_controller_t *xdma;
	xdma_channel_t *xchan;
	bool data_match, dst_loaded, src_loaded;
	int error, i;

	bzero(&mem, sizeof(mem));
	bzero(&req, sizeof(req));
	xdma = NULL;
	xchan = NULL;
	src_loaded = false;
	dst_loaded = false;
	data_match = false;

	mtx_lock(&sc->test_mtx);
	if (sc->test_running) {
		mtx_unlock(&sc->test_mtx);
		return (EBUSY);
	}
	sc->test_running = true;
	sc->test_done = 0;
	sc->test_error = 0;
	mtx_unlock(&sc->test_mtx);

	error = bus_dma_tag_create(bus_get_dma_tag(sc->dev), 128, 0,
	    BUS_SPACE_MAXADDR_32BIT, BUS_SPACE_MAXADDR, NULL, NULL,
	    PL330_SELFTEST_LEN, 1, PL330_SELFTEST_LEN, 0, NULL, NULL,
	    &mem.tag);
	if (error != 0)
		goto out;
	error = bus_dmamem_alloc(mem.tag, (void **)&mem.src,
	    BUS_DMA_WAITOK | BUS_DMA_COHERENT | BUS_DMA_ZERO, &mem.src_map);
	if (error != 0)
		goto out;
	mem.src_phys = BUS_SPACE_MAXADDR;
	error = bus_dmamap_load(mem.tag, mem.src_map, mem.src,
	    PL330_SELFTEST_LEN, pl330_dmamap_cb, &mem.src_phys, BUS_DMA_WAITOK);
	if (error != 0 || mem.src_phys == BUS_SPACE_MAXADDR) {
		error = error != 0 ? error : EFAULT;
		goto out;
	}
	src_loaded = true;

	error = bus_dmamem_alloc(mem.tag, (void **)&mem.dst,
	    BUS_DMA_WAITOK | BUS_DMA_COHERENT | BUS_DMA_ZERO, &mem.dst_map);
	if (error != 0)
		goto out;
	mem.dst_phys = BUS_SPACE_MAXADDR;
	error = bus_dmamap_load(mem.tag, mem.dst_map, mem.dst,
	    PL330_SELFTEST_LEN, pl330_dmamap_cb, &mem.dst_phys, BUS_DMA_WAITOK);
	if (error != 0 || mem.dst_phys == BUS_SPACE_MAXADDR) {
		error = error != 0 ? error : EFAULT;
		goto out;
	}
	dst_loaded = true;

	for (i = 0; i < PL330_SELFTEST_LEN; i++) {
		mem.src[i] = (i * 37) ^ (i >> 3);
		mem.dst[i] = 0xa5;
	}
	bus_dmamap_sync(mem.tag, mem.src_map, BUS_DMASYNC_PREWRITE);
	bus_dmamap_sync(mem.tag, mem.dst_map, BUS_DMASYNC_PREREAD);

	xdma = xdma_get(sc->dev, sc->dev);
	xchan = xdma_channel_alloc(xdma, 0);
	if (xchan == NULL) {
		error = ENXIO;
		goto sync_out;
	}
	error = xdma_setup_intr(xchan, 0, pl330_selftest_intr, sc, NULL);
	if (error != 0) {
		error = EIO;
		goto sync_out;
	}

	req.operation = XDMA_MEMCPY;
	req.req_type = XR_TYPE_PHYS;
	req.direction = XDMA_MEM_TO_MEM;
	req.src_addr = mem.src_phys;
	req.dst_addr = mem.dst_phys;
	req.src_width = 8;
	req.dst_width = 8;
	req.block_num = 1;
	req.block_len = PL330_SELFTEST_LEN;

	mtx_lock(&sc->test_mtx);
	error = xdma_request(xchan, &req);
	if (error != 0) {
		error = EIO;
	} else {
		while (!sc->test_done && error == 0)
			error = msleep(&sc->test_done, &sc->test_mtx, 0,
			    "pl330t", PL330_SELFTEST_TIMEOUT);
		if (error == EWOULDBLOCK)
			error = ETIMEDOUT;
		else if (error == 0)
			error = sc->test_error;
	}
	mtx_unlock(&sc->test_mtx);
	if (error == ETIMEDOUT) {
		pl330_dump_channel(xchan->chan, "self-test timeout");
		(void)xdma_control(xchan, XDMA_CMD_TERMINATE);
	}

sync_out:
	bus_dmamap_sync(mem.tag, mem.src_map, BUS_DMASYNC_POSTWRITE);
	bus_dmamap_sync(mem.tag, mem.dst_map, BUS_DMASYNC_POSTREAD);
	data_match = bcmp(mem.src, mem.dst, PL330_SELFTEST_LEN) == 0;
	if (error == 0 && !data_match)
		error = EIO;
	if (xchan != NULL)
		(void)xdma_channel_free(xchan);
	if (xdma != NULL)
		(void)xdma_put(xdma);

out:
	if (dst_loaded)
		bus_dmamap_unload(mem.tag, mem.dst_map);
	if (mem.dst != NULL)
		bus_dmamem_free(mem.tag, mem.dst, mem.dst_map);
	if (src_loaded)
		bus_dmamap_unload(mem.tag, mem.src_map);
	if (mem.src != NULL)
		bus_dmamem_free(mem.tag, mem.src, mem.src_map);
	if (mem.tag != NULL)
		bus_dma_tag_destroy(mem.tag);

	mtx_lock(&sc->test_mtx);
	sc->test_running = false;
	sc->selftest_result = error == 0 ? 1 : -error;
	mtx_unlock(&sc->test_mtx);
	if (error == 0)
		device_printf(sc->dev, "4 KiB xDMA memcpy self-test passed\n");
	else
		device_printf(sc->dev,
		    "4 KiB xDMA memcpy self-test failed: error %d, data %s\n",
		    error, data_match ? "matches" : "differs");

	return (error);
}

static int
pl330_selftest_sysctl(SYSCTL_HANDLER_ARGS)
{
	struct pl330_softc *sc;
	int error, value;

	sc = arg1;
	value = sc->selftest_result;
	error = sysctl_handle_int(oidp, &value, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	if (value != 1)
		return (EINVAL);

	return (pl330_selftest(sc));
}

static device_method_t pl330_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,			pl330_probe),
	DEVMETHOD(device_attach,		pl330_attach),
	DEVMETHOD(device_detach,		pl330_detach),

	/* xDMA Interface */
	DEVMETHOD(xdma_channel_request,		pl330_channel_request),
	DEVMETHOD(xdma_channel_alloc,		pl330_channel_alloc),
	DEVMETHOD(xdma_channel_free,		pl330_channel_free),
	DEVMETHOD(xdma_channel_control,		pl330_channel_control),

	/* xDMA SG Interface */
	DEVMETHOD(xdma_channel_capacity,	pl330_channel_capacity),
	DEVMETHOD(xdma_channel_prep_sg,		pl330_channel_prep_sg),
	DEVMETHOD(xdma_channel_submit_sg,	pl330_channel_submit_sg),

#ifdef FDT
	DEVMETHOD(xdma_ofw_md_data,		pl330_ofw_md_data),
#endif

	DEVMETHOD_END
};

static driver_t pl330_driver = {
	"pl330",
	pl330_methods,
	sizeof(struct pl330_softc),
};

EARLY_DRIVER_MODULE(pl330, simplebus, pl330_driver, 0, 0,
    BUS_PASS_INTERRUPT + BUS_PASS_ORDER_LATE);
EARLY_DRIVER_MODULE(pl330, ofwbus, pl330_driver, 0, 0,
    BUS_PASS_INTERRUPT + BUS_PASS_ORDER_LATE);
MODULE_DEPEND(pl330, xdma, 1, 1, 1);
