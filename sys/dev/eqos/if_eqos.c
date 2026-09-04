/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2022 Soren Schmidt <sos@deepcore.dk>
 * Copyright (c) 2022 Jared McNeill <jmcneill@invisible.ca>
 * All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $Id: eqos.c 1059 2022-12-08 19:32:32Z sos $
 */

/*
 * DesignWare Ethernet Quality-of-Service controller
 */

#include "opt_platform.h"
#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/mutex.h>
#include <sys/rman.h>
#include <sys/endian.h>
#include <sys/module.h>
#include <sys/bus.h>
#include <sys/buf_ring.h>
#include <sys/callout.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/mbuf.h>
#include <machine/bus.h>

#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_var.h>
#include <net/if_dl.h>
#include <net/if_media.h>
#include <net/if_types.h>
#include <net/bpf.h>

#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netinet/tcp_lro.h>

#include <dev/mii/mii.h>
#include <dev/mii/miivar.h>
#include <dev/mdio/mdio.h>

#include "miibus_if.h"
#include "mdio_if.h"
#include "if_eqos_if.h"

#ifdef FDT
#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#include <dev/extres/clk/clk.h>
#endif

#include <dev/eqos/if_eqos_reg.h>
#include <dev/eqos/if_eqos_var.h>

#define	DESC_BOUNDARY		(1ULL << 32)
#define	DESC_ALIGN		sizeof(struct eqos_dma_desc)
#define	DESC_OFFSET(n)		((n) * sizeof(struct eqos_dma_desc))

#define	TX_DESC_COUNT		EQOS_DMA_DESC_COUNT
#define	TX_DESC_SIZE		(TX_DESC_COUNT * DESC_ALIGN)
#define	TX_MAX_SEGS		(TX_DESC_COUNT / 2)
#define	TX_NEXT(n)		(((n) + 1 ) % TX_DESC_COUNT)
#define	TX_QUEUED(h, t)		((((h) - (t)) + TX_DESC_COUNT) % TX_DESC_COUNT)
#define	TX_CONTROL_QUEUE_COUNT	256
#define	TX_DATA_QUEUE_COUNT	1024
#define	TX_CONTROL_MAX_LEN	256
#define	TX_TSO_COAL_FRAMES	4

#define	RX_DESC_COUNT		EQOS_DMA_DESC_COUNT
#define	RX_DESC_SIZE		(RX_DESC_COUNT * DESC_ALIGN)
#define	RX_NEXT(n)		(((n) + 1) % RX_DESC_COUNT)
#define	RX_TX_SERVICE_FRAMES	64
#define	RX_DMA_SIZE(sc)		(((sc)->rx_buf_size - ETHER_ALIGN) & ~7U)

#define	MII_BUSY_RETRY		1000
#define	WATCHDOG_TIMEOUT_SECS	3
#define	EQOS_MAX_MTU		9000

#define	EQOS_LOCK(sc)		mtx_lock(&(sc)->lock)
#define	EQOS_UNLOCK(sc)		mtx_unlock(&(sc)->lock)
#define	EQOS_ASSERT_LOCKED(sc)	mtx_assert(&(sc)->lock, MA_OWNED)

#define	RD4(sc, o)		bus_read_4(sc->res[EQOS_RES_MEM], (o))
#define	WR4(sc, o, v)		bus_write_4(sc->res[EQOS_RES_MEM], (o), (v))


static struct resource_spec eqos_spec[] = {
	{ SYS_RES_MEMORY,	0,	RF_ACTIVE },
	{ SYS_RES_IRQ,		0,	RF_ACTIVE },
	{ -1, 0 }
};

static void eqos_tick(void *softc);
static void eqos_axi_configure(struct eqos_softc *sc);
static void eqos_txintr(struct eqos_softc *sc);
static void eqos_start_locked(if_t ifp);


static int
eqos_miibus_readreg(device_t dev, int phy, int reg)
{
	struct eqos_softc *sc = device_get_softc(dev);
	uint32_t addr;
	int retry, val;

	addr = sc->csr_clock_range |
	    (phy << GMAC_MAC_MDIO_ADDRESS_PA_SHIFT) |
	    (reg << GMAC_MAC_MDIO_ADDRESS_RDA_SHIFT) |
	    GMAC_MAC_MDIO_ADDRESS_GOC_READ | GMAC_MAC_MDIO_ADDRESS_GB;
	WR4(sc, GMAC_MAC_MDIO_ADDRESS, addr);

	DELAY(100);

	for (retry = MII_BUSY_RETRY; retry > 0; retry--) {
		addr = RD4(sc, GMAC_MAC_MDIO_ADDRESS);
		if (!(addr & GMAC_MAC_MDIO_ADDRESS_GB)) {
			val = RD4(sc, GMAC_MAC_MDIO_DATA) & 0xFFFF;
			break;
		}
		DELAY(10);
	}
	if (!retry) {
		device_printf(dev, "phy read timeout, phy=%d reg=%d\n",
		    phy, reg);
		return (ETIMEDOUT);
	}
	return (val);
}

static int
eqos_miibus_writereg(device_t dev, int phy, int reg, int val)
{
	struct eqos_softc *sc = device_get_softc(dev);
	uint32_t addr;
	int retry;

	WR4(sc, GMAC_MAC_MDIO_DATA, val);

	addr = sc->csr_clock_range |
	    (phy << GMAC_MAC_MDIO_ADDRESS_PA_SHIFT) |
	    (reg << GMAC_MAC_MDIO_ADDRESS_RDA_SHIFT) |
	    GMAC_MAC_MDIO_ADDRESS_GOC_WRITE | GMAC_MAC_MDIO_ADDRESS_GB;
	WR4(sc, GMAC_MAC_MDIO_ADDRESS, addr);

	DELAY(100);

	for (retry = MII_BUSY_RETRY; retry > 0; retry--) {
		addr = RD4(sc, GMAC_MAC_MDIO_ADDRESS);
		if (!(addr & GMAC_MAC_MDIO_ADDRESS_GB))
			break;
		DELAY(10);
	}
	if (!retry) {
		device_printf(dev, "phy write timeout, phy=%d reg=%d\n",
		    phy, reg);
		return (ETIMEDOUT);
	}
	return (0);
}

static void
eqos_set_link(struct eqos_softc *sc, int media_status, int media_active)
{
	uint32_t reg;

	EQOS_ASSERT_LOCKED(sc);

	if (media_status & IFM_ACTIVE)
		sc->link_up = true;
	else
		sc->link_up = false;

	reg = RD4(sc, GMAC_MAC_CONFIGURATION);

	switch (IFM_SUBTYPE(media_active)) {
	case IFM_10_T:
		reg |= GMAC_MAC_CONFIGURATION_PS;
		reg &= ~GMAC_MAC_CONFIGURATION_FES;
		break;
	case IFM_100_TX:
		reg |= GMAC_MAC_CONFIGURATION_PS;
		reg |= GMAC_MAC_CONFIGURATION_FES;
		break;
	case IFM_1000_T:
        case IFM_1000_SX:
		reg &= ~GMAC_MAC_CONFIGURATION_PS;
		reg &= ~GMAC_MAC_CONFIGURATION_FES;
		break;
	case IFM_2500_T:
	case IFM_2500_SX:
		reg &= ~GMAC_MAC_CONFIGURATION_PS;
		reg |= GMAC_MAC_CONFIGURATION_FES;
		break;
	default:
		sc->link_up = false;
		return;
	}

	if ((IFM_OPTIONS(media_active) & IFM_FDX))
		reg |= GMAC_MAC_CONFIGURATION_DM;
	else
		reg &= ~GMAC_MAC_CONFIGURATION_DM;

	WR4(sc, GMAC_MAC_CONFIGURATION, reg);

	IF_EQOS_SET_SPEED(sc->dev, IFM_SUBTYPE(media_active));

	WR4(sc, GMAC_MAC_1US_TIC_COUNTER, (sc->csr_clock / 1000000) - 1);
}

static void
eqos_miibus_statchg(device_t dev)
{
	struct eqos_softc *sc = device_get_softc(dev);
	struct mii_data *mii = device_get_softc(sc->miibus);

	eqos_set_link(sc, mii->mii_media_status, mii->mii_media_active);
}

static void
eqos_media_status(if_t ifp, struct ifmediareq *ifmr)
{
	struct eqos_softc *sc = if_getsoftc(ifp);
	struct mii_data *mii = device_get_softc(sc->miibus);

	EQOS_LOCK(sc);
	mii_pollstat(mii);
	ifmr->ifm_active = mii->mii_media_active;
	ifmr->ifm_status = mii->mii_media_status;
	EQOS_UNLOCK(sc);
}

static int
eqos_media_change(if_t ifp)
{
	struct eqos_softc *sc = if_getsoftc(ifp);
	int error;

	EQOS_LOCK(sc);
	error = mii_mediachg(device_get_softc(sc->miibus)); 
	EQOS_UNLOCK(sc);
	return (error);
}

#ifdef FDT
static int
eqos_fixed_media_change(if_t ifp)
{

	if_printf(ifp, "Cannot change media in fixed-link mode\n");
	return (0);
}

static void
eqos_fixed_media_status(if_t ifp, struct ifmediareq *ifmr)
{
	struct eqos_softc *sc;

	sc = if_getsoftc(ifp);
	ifmr->ifm_active = sc->fixed_ifmedia.ifm_cur->ifm_media;
	ifmr->ifm_status = IFM_AVALID | IFM_ACTIVE;
}

static int
eqos_setup_fixed(struct eqos_softc *sc, phandle_t node)
{
	int media, speed;

	if (OF_getencprop(node, "speed", &speed, sizeof(speed)) <= 0) {
		device_printf(sc->dev,
		    "fixed-link node has no link speed\n");
		return (ENXIO);
	}

	switch (speed) {
	case 10:
		media = IFM_10_T;
		break;
	case 100:
		media = IFM_100_TX;
		break;
	case 1000:
		media = IFM_1000_T;
		break;
	case 2500:
		media = IFM_2500_T;
		break;
	default:
		device_printf(sc->dev, "unsupported fixed-link speed %d\n",
		    speed);
		return (EINVAL);
	}

	media |= IFM_ETHER;
	media |= OF_hasprop(node, "full-duplex") ? IFM_FDX : IFM_HDX;
	if (OF_hasprop(node, "pause"))
		media |= IFM_ETH_RXPAUSE | IFM_ETH_TXPAUSE;

	ifmedia_init(&sc->fixed_ifmedia, 0, eqos_fixed_media_change,
	    eqos_fixed_media_status);
	ifmedia_add(&sc->fixed_ifmedia, media, 0, NULL);
	ifmedia_set(&sc->fixed_ifmedia, media);
	sc->fixed_link = true;
	return (0);
}

static int
eqos_attach_mdio(struct eqos_softc *sc)
{
	phandle_t child, node;

	node = ofw_bus_get_node(sc->dev);
	for (child = OF_child(node); child != 0; child = OF_peer(child)) {
		if (ofw_bus_node_is_compatible(child, "snps,dwmac-mdio"))
			break;
	}
	if (child == 0)
		return (0);

	sc->mdio = device_add_child(sc->dev, "mdio", -1);
	if (sc->mdio == NULL) {
		device_printf(sc->dev, "cannot add MDIO bus\n");
		return (ENXIO);
	}
	return (bus_generic_attach(sc->dev));
}
#endif

static void
eqos_setup_txdesc(struct eqos_softc *sc, int index, int flags,
    bus_addr_t paddr, u_int len, u_int total_len, bool ioc)
{
	uint32_t tdes2, tdes3;

	if (!paddr || !len) {
		tdes2 = 0;
		tdes3 = flags;
	} else {
		tdes2 = (flags & EQOS_TDES3_LD) && ioc ? EQOS_TDES2_IOC : 0;
		tdes3 = flags;
	}
	bus_dmamap_sync(sc->tx.desc_tag, sc->tx.desc_map, BUS_DMASYNC_PREWRITE);
	sc->tx.desc_ring[index].des0 = htole32((uint32_t)paddr);
	sc->tx.desc_ring[index].des1 = htole32((uint32_t)(paddr >> 32));
	sc->tx.desc_ring[index].des2 = htole32(tdes2 | len);
	sc->tx.desc_ring[index].des3 = htole32(tdes3 | total_len);
}

static void
eqos_setup_mssdesc(struct eqos_softc *sc, int index, uint32_t mss)
{

	bus_dmamap_sync(sc->tx.desc_tag, sc->tx.desc_map,
	    BUS_DMASYNC_PREWRITE);
	sc->tx.desc_ring[index].des0 = 0;
	sc->tx.desc_ring[index].des1 = 0;
	sc->tx.desc_ring[index].des2 = htole32(mss);
	sc->tx.desc_ring[index].des3 = htole32(EQOS_TDES3_CTXT |
	    EQOS_TDES3_TCMSSV);
}

static int
eqos_setup_txbuf(struct eqos_softc *sc, struct mbuf **mp)
{
	bus_dma_segment_t segs[TX_MAX_SEGS];
	bus_dmamap_t map;
	struct mbuf *m;
	struct ether_header *eh;
	struct ether_vlan_header *evh;
	struct ip *ip;
	struct tcphdr *tcp;
	int first = sc->tx.head;
	int context, error, first_data, idx, last, ndescs, nsegs;
	bool csum, ioc, tso;
	u_int header_len, l2hlen, l3hlen, l4hlen, len, offset, payload_len;
	uint16_t ether_type;
	uint32_t flags;

	m = *mp;
	tso = (m->m_pkthdr.csum_flags & CSUM_IP_TSO) != 0;
	if (tso) {
		m = m_pullup(m, MIN(m->m_pkthdr.len,
		    sizeof(*evh) + 2 * TCP_MAXHLEN));
		if (m == NULL) {
			*mp = NULL;
			return (ENOMEM);
		}
		*mp = m;
		eh = mtod(m, struct ether_header *);
		ether_type = ntohs(eh->ether_type);
		l2hlen = ETHER_HDR_LEN;
		if (ether_type == ETHERTYPE_VLAN || ether_type == ETHERTYPE_QINQ) {
			if (m->m_len < sizeof(*evh))
				return (EINVAL);
			evh = mtod(m, struct ether_vlan_header *);
			ether_type = ntohs(evh->evl_proto);
			l2hlen = sizeof(*evh);
		}
		if (ether_type != ETHERTYPE_IP ||
		    m->m_len < l2hlen + sizeof(*ip))
			return (EINVAL);
		ip = (struct ip *)(mtod(m, char *) + l2hlen);
		l3hlen = ip->ip_hl << 2;
		if (ip->ip_v != IPVERSION || ip->ip_p != IPPROTO_TCP ||
		    l3hlen < sizeof(*ip) ||
		    m->m_len < l2hlen + l3hlen + sizeof(*tcp))
			return (EINVAL);
		tcp = (struct tcphdr *)(mtod(m, char *) + l2hlen + l3hlen);
		l4hlen = tcp->th_off << 2;
		header_len = l2hlen + l3hlen + l4hlen;
		if (l4hlen < sizeof(*tcp) || l4hlen / 4 > 0xf ||
		    m->m_len < header_len || header_len >= m->m_pkthdr.len ||
		    m->m_pkthdr.tso_segsz < 64)
			return (EINVAL);
		payload_len = m->m_pkthdr.len - header_len;
		if (payload_len > EQOS_TDES3_TCPPAYLOAD_MASK)
			return (EINVAL);
	}

	error = bus_dmamap_load_mbuf_sg(sc->tx.buf_tag,
	    sc->tx.buf_map[first].map, m, segs, &nsegs, 0);
	if (error == EFBIG) {
		struct mbuf *mb;

		device_printf(sc->dev, "TX packet too big trying defrag\n");
		bus_dmamap_unload(sc->tx.buf_tag, sc->tx.buf_map[first].map);
		if (!(mb = m_defrag(m, M_NOWAIT)))
			return (ENOMEM);
		*mp = m = mb;
		error = bus_dmamap_load_mbuf_sg(sc->tx.buf_tag,
		    sc->tx.buf_map[first].map, m, segs, &nsegs, 0);
	}
	if (error)
		return (ENOMEM);

	ndescs = nsegs;
	if (tso) {
		if (header_len > segs[0].ds_len) {
			bus_dmamap_unload(sc->tx.buf_tag,
			    sc->tx.buf_map[first].map);
			return (EINVAL);
		}
		if (segs[0].ds_len > header_len)
			ndescs++;
		if (sc->tx_mss != m->m_pkthdr.tso_segsz)
			ndescs++;
	}
	if (TX_QUEUED(sc->tx.head, sc->tx.tail) + ndescs >= TX_DESC_COUNT) {
		bus_dmamap_unload(sc->tx.buf_tag, sc->tx.buf_map[first].map);
		return (ENOMEM);
	}
	if (tso) {
		ioc = ++sc->tx_tso_frames >= TX_TSO_COAL_FRAMES;
		if (ioc)
			sc->tx_tso_frames = 0;
	} else {
		ioc = sc->tx_coal_frames == 0 ||
		    ++sc->tx_frames >= sc->tx_coal_frames;
		if (ioc)
			sc->tx_frames = 0;
	}
	csum = (m->m_pkthdr.csum_flags & (CSUM_IP | CSUM_TCP | CSUM_UDP |
	    CSUM_TCP_IPV6 | CSUM_UDP_IPV6)) != 0;

	bus_dmamap_sync(sc->tx.buf_tag, sc->tx.buf_map[first].map,
	    BUS_DMASYNC_PREWRITE);

	context = -1;
	if (tso && sc->tx_mss != m->m_pkthdr.tso_segsz) {
		context = sc->tx.head;
		eqos_setup_mssdesc(sc, context, m->m_pkthdr.tso_segsz);
		sc->tx.head = TX_NEXT(sc->tx.head);
	}
	first_data = sc->tx.head;
	last = first_data;
	if (tso) {
		flags = EQOS_TDES3_FD | EQOS_TDES3_TSE |
		    (l4hlen / 4 << EQOS_TDES3_TCPHDRLEN_SHIFT) |
		    payload_len;
		eqos_setup_txdesc(sc, sc->tx.head, flags, segs[0].ds_addr,
		    header_len, 0, false);
		sc->tx.head = TX_NEXT(sc->tx.head);
		for (idx = 0; idx < nsegs; idx++) {
			offset = idx == 0 ? header_len : 0;
			len = segs[idx].ds_len - offset;
			if (len == 0)
				continue;
			last = sc->tx.head;
			flags = EQOS_TDES3_OWN;
			if (idx == nsegs - 1)
				flags |= EQOS_TDES3_LD;
			eqos_setup_txdesc(sc, sc->tx.head, flags,
			    segs[idx].ds_addr + offset, len, 0, ioc);
			sc->tx.head = TX_NEXT(sc->tx.head);
		}
	} else {
		flags = EQOS_TDES3_FD | (csum ? EQOS_TDES3_CIC_FULL : 0);
		for (idx = 0; idx < nsegs; idx++) {
			last = sc->tx.head;
			if (idx == nsegs - 1)
				flags |= EQOS_TDES3_LD;
			eqos_setup_txdesc(sc, sc->tx.head, flags,
			    segs[idx].ds_addr, segs[idx].ds_len,
			    m->m_pkthdr.len, ioc);
			flags &= ~(EQOS_TDES3_FD | EQOS_TDES3_CIC_FULL);
			flags |= EQOS_TDES3_OWN;
			sc->tx.head = TX_NEXT(sc->tx.head);
		}
	}
	if (last != first) {
		map = sc->tx.buf_map[first].map;
		sc->tx.buf_map[first].map = sc->tx.buf_map[last].map;
		sc->tx.buf_map[last].map = map;
	}
	sc->tx.buf_map[last].mbuf = m;

	/*
	 * Defer setting OWN bit on the first descriptor
	 * until all descriptors have been updated
	 */
	bus_dmamap_sync(sc->tx.desc_tag, sc->tx.desc_map, BUS_DMASYNC_PREWRITE);
	sc->tx.desc_ring[first_data].des3 |= htole32(EQOS_TDES3_OWN);
	if (context >= 0) {
		bus_dmamap_sync(sc->tx.desc_tag, sc->tx.desc_map,
		    BUS_DMASYNC_PREWRITE);
		sc->tx.desc_ring[context].des3 |= htole32(EQOS_TDES3_OWN);
		sc->tx_mss = m->m_pkthdr.tso_segsz;
	}

	return (0);
}

static void
eqos_setup_rxdesc(struct eqos_softc *sc, int index, bus_addr_t paddr)
{
	uint32_t flags;

	flags = EQOS_RDES3_OWN | EQOS_RDES3_BUF1V;
	if (sc->rx_coal_frames == 0 ||
	    (index + 1) % sc->rx_coal_frames == 0)
		flags |= EQOS_RDES3_IOC;
	sc->rx.desc_ring[index].des0 = htole32((uint32_t)paddr);
	sc->rx.desc_ring[index].des1 = htole32((uint32_t)(paddr >> 32));
	sc->rx.desc_ring[index].des2 = htole32(0);
	bus_dmamap_sync(sc->rx.desc_tag, sc->rx.desc_map, BUS_DMASYNC_PREWRITE);
	sc->rx.desc_ring[index].des3 = htole32(flags);
}

static int
eqos_setup_rxbuf(struct eqos_softc *sc, int index, struct mbuf *m)
{
	struct bus_dma_segment seg;
	int error, nsegs;

	m_adj(m, ETHER_ALIGN);

	error = bus_dmamap_load_mbuf_sg(sc->rx.buf_tag,
	    sc->rx.buf_map[index].map, m, &seg, &nsegs, 0);
	if (error)
		return (error);

	bus_dmamap_sync(sc->rx.buf_tag, sc->rx.buf_map[index].map,
	    BUS_DMASYNC_PREREAD);

	sc->rx.buf_map[index].mbuf = m;
	sc->rx.buf_map[index].paddr = seg.ds_addr;
	eqos_setup_rxdesc(sc, index, seg.ds_addr);

	return (0);
}

static struct mbuf *
eqos_alloc_rxbuf(struct eqos_softc *sc)
{
	struct mbuf *m;

	if ((m = m_getjcl(M_NOWAIT, MT_DATA, M_PKTHDR,
	    sc->rx_buf_size)) != NULL)
		m->m_pkthdr.len = m->m_len = m->m_ext.ext_size;
	return (m);
}

static void
eqos_enable_intr(struct eqos_softc *sc)
{

	WR4(sc, GMAC_DMA_CHAN0_INTR_ENABLE,
	    GMAC_DMA_CHAN0_INTR_ENABLE_NIE | GMAC_DMA_CHAN0_INTR_ENABLE_AIE |
	    GMAC_DMA_CHAN0_INTR_ENABLE_FBE | GMAC_DMA_CHAN0_INTR_ENABLE_RIE |
	    GMAC_DMA_CHAN0_INTR_ENABLE_TIE);
}

static void
eqos_disable_intr(struct eqos_softc *sc)
{

	WR4(sc, GMAC_DMA_CHAN0_INTR_ENABLE, 0);
}

static uint32_t
eqos_bitrev32(uint32_t x)
{

	x = (((x & 0xaaaaaaaa) >> 1) | ((x & 0x55555555) << 1));
	x = (((x & 0xcccccccc) >> 2) | ((x & 0x33333333) << 2));
	x = (((x & 0xf0f0f0f0) >> 4) | ((x & 0x0f0f0f0f) << 4));
	x = (((x & 0xff00ff00) >> 8) | ((x & 0x00ff00ff) << 8));
	return ((x >> 16) | (x << 16));
}

static u_int
eqos_hash_maddr(void *arg, struct sockaddr_dl *sdl, u_int cnt)
{
	uint32_t crc, *hash = arg;

	crc = ether_crc32_le(LLADDR(sdl), ETHER_ADDR_LEN);
	crc &= 0x7f;
	crc = eqos_bitrev32(~crc) >> 26;
	hash[crc >> 5] |= 1 << (crc & 0x1f);
	return (1);
}

static void
eqos_setup_rxfilter(struct eqos_softc *sc)
{
	if_t ifp = sc->ifp;
	uint32_t pfil, hash[2];
	const uint8_t *eaddr;
	uint32_t val;

	EQOS_ASSERT_LOCKED(sc);

	pfil = RD4(sc, GMAC_MAC_PACKET_FILTER);
	pfil &= ~(GMAC_MAC_PACKET_FILTER_PR |
	    GMAC_MAC_PACKET_FILTER_PM |
	    GMAC_MAC_PACKET_FILTER_HMC |
	    GMAC_MAC_PACKET_FILTER_PCF_MASK);
	hash[0] = hash[1] = 0xffffffff;

	if ((if_getflags(ifp) & IFF_PROMISC)) {
		pfil |= GMAC_MAC_PACKET_FILTER_PR |
		    GMAC_MAC_PACKET_FILTER_PCF_ALL;
	} else if ((if_getflags(ifp) & IFF_ALLMULTI)) {
		pfil |= GMAC_MAC_PACKET_FILTER_PM;
	} else {
		hash[0] = hash[1] = 0;
		pfil |= GMAC_MAC_PACKET_FILTER_HMC;
		if_foreach_llmaddr(ifp, eqos_hash_maddr, hash);
	}

	/* Write our unicast address */
	eaddr = if_getlladdr(ifp);
	val = eaddr[4] | (eaddr[5] << 8);
	WR4(sc, GMAC_MAC_ADDRESS0_HIGH, val);
	val = eaddr[0] | (eaddr[1] << 8) | (eaddr[2] << 16) |
	    (eaddr[3] << 24);
	WR4(sc, GMAC_MAC_ADDRESS0_LOW, val);

	/* Multicast hash filters */
	WR4(sc, GMAC_MAC_HASH_TABLE_REG0, hash[1]);
	WR4(sc, GMAC_MAC_HASH_TABLE_REG1, hash[0]);

	/* Packet filter config */
	WR4(sc, GMAC_MAC_PACKET_FILTER, pfil);
}

static int
eqos_reset(struct eqos_softc *sc)
{
	uint32_t before, val;
	int retry;

	before = RD4(sc, GMAC_DMA_MODE);
	WR4(sc, GMAC_DMA_MODE, GMAC_DMA_MODE_SWR);
	for (retry = 2000; retry > 0; retry--) {
		DELAY(1000);
		val = RD4(sc, GMAC_DMA_MODE);
		if (!(val & GMAC_DMA_MODE_SWR))
			return (0);
	}
	device_printf(sc->dev,
	    "DMA reset stuck: before=%#x mode=%#x mac=%#x debug=%#x dma=%#x\n",
	    before, val, RD4(sc, GMAC_MAC_VERSION), RD4(sc, GMAC_MAC_DEBUG),
	    RD4(sc, GMAC_DMA_DEBUG_STATUS0));
	return (ETIMEDOUT);
}

static void
eqos_init_rings(struct eqos_softc *sc)
{
	struct eqos_bufmap *bmap;
	int i;

	if (sc->rx_mbuf_head != NULL)
		m_freem(sc->rx_mbuf_head);
	sc->rx_mbuf_head = sc->rx_mbuf_tail = NULL;
	sc->rx_mbuf_len = 0;
	sc->rx_mbuf_error = false;

	for (i = 0; i < TX_DESC_COUNT; i++) {
		bmap = &sc->tx.buf_map[i];
		if (bmap->mbuf != NULL) {
			bus_dmamap_sync(sc->tx.buf_tag, bmap->map,
			    BUS_DMASYNC_POSTWRITE);
			bus_dmamap_unload(sc->tx.buf_tag, bmap->map);
			m_freem(bmap->mbuf);
			bmap->mbuf = NULL;
		}
		eqos_setup_txdesc(sc, i, 0, 0, 0, 0, false);
	}
	for (i = 0; i < RX_DESC_COUNT; i++) {
		bmap = &sc->rx.buf_map[i];
		bus_dmamap_sync(sc->rx.buf_tag, bmap->map,
		    BUS_DMASYNC_POSTREAD);
		bus_dmamap_sync(sc->rx.buf_tag, bmap->map,
		    BUS_DMASYNC_PREREAD);
		eqos_setup_rxdesc(sc, i, bmap->paddr);
	}
	sc->tx.head = sc->tx.tail = 0;
	sc->rx.head = sc->rx.tail = 0;
	sc->tx_frames = 0;
	sc->tx_tso_frames = 0;
	sc->tx_mss = 0;
	sc->tx_watchdog = 0;

	WR4(sc, GMAC_DMA_CHAN0_TX_BASE_ADDR_HI,
	    (uint32_t)(sc->tx.desc_ring_paddr >> 32));
	WR4(sc, GMAC_DMA_CHAN0_TX_BASE_ADDR,
	    (uint32_t)sc->tx.desc_ring_paddr);
	WR4(sc, GMAC_DMA_CHAN0_TX_RING_LEN, TX_DESC_COUNT - 1);

	WR4(sc, GMAC_DMA_CHAN0_RX_BASE_ADDR_HI,
	    (uint32_t)(sc->rx.desc_ring_paddr >> 32));
	WR4(sc, GMAC_DMA_CHAN0_RX_BASE_ADDR,
	    (uint32_t)sc->rx.desc_ring_paddr);
	WR4(sc, GMAC_DMA_CHAN0_RX_RING_LEN, RX_DESC_COUNT - 1);

	WR4(sc, GMAC_DMA_CHAN0_RX_END_ADDR,
	    (uint32_t)sc->rx.desc_ring_paddr + DESC_OFFSET(RX_DESC_COUNT));
}

static void
eqos_init(void *if_softc)
{
	struct eqos_softc *sc = if_softc;
	if_t ifp = sc->ifp;
	struct mii_data *mii;
	uint32_t rqs, tqs, val;
	int error;

	if (if_getdrvflags(ifp) & IFF_DRV_RUNNING)
		return;

	EQOS_LOCK(sc);

	if (!sc->dma_reset_done) {
		if ((error = eqos_reset(sc)) != 0) {
			device_printf(sc->dev, "reset timeout!\n");
			EQOS_UNLOCK(sc);
			return;
		}
		eqos_axi_configure(sc);
		sc->dma_reset_done = true;
	}

	eqos_init_rings(sc);

	eqos_setup_rxfilter(sc);

	WR4(sc, GMAC_MAC_1US_TIC_COUNTER, (sc->csr_clock / 1000000) - 1);

	/* Enable transmit and receive DMA */
	val = RD4(sc, GMAC_DMA_CHAN0_CONTROL);
	val &= ~GMAC_DMA_CHAN0_CONTROL_DSL_MASK;
	val |= ((DESC_ALIGN - 16) / 8) << GMAC_DMA_CHAN0_CONTROL_DSL_SHIFT;
	val |= GMAC_DMA_CHAN0_CONTROL_PBLX8;
	WR4(sc, GMAC_DMA_CHAN0_CONTROL, val);
	val = RD4(sc, GMAC_DMA_CHAN0_TX_CONTROL);
	val &= ~GMAC_DMA_CHAN0_TXRX_PBL_MASK;
	if (sc->txpbl > 0)
		val |= sc->txpbl << GMAC_DMA_CHAN0_TXRX_PBL_SHIFT;
	if ((if_getcapenable(ifp) & IFCAP_TSO4) != 0)
		val |= GMAC_DMA_CHAN0_TX_CONTROL_TSE;
	else
		val &= ~GMAC_DMA_CHAN0_TX_CONTROL_TSE;
	val |= GMAC_DMA_CHAN0_TX_CONTROL_OSP;
	val |= GMAC_DMA_CHAN0_TX_CONTROL_START;
	WR4(sc, GMAC_DMA_CHAN0_TX_CONTROL, val);
	val = RD4(sc, GMAC_DMA_CHAN0_RX_CONTROL);
	val &= ~GMAC_DMA_CHAN0_TXRX_PBL_MASK;
	if (sc->rxpbl > 0)
		val |= sc->rxpbl << GMAC_DMA_CHAN0_TXRX_PBL_SHIFT;
	val &= ~GMAC_DMA_CHAN0_RX_CONTROL_RBSZ_MASK;
	val |= (RX_DMA_SIZE(sc) << GMAC_DMA_CHAN0_RX_CONTROL_RBSZ_SHIFT);
	WR4(sc, GMAC_DMA_CHAN0_RX_WATCHDOG, sc->rx_riwt);
	val |= GMAC_DMA_CHAN0_RX_CONTROL_START;
	WR4(sc, GMAC_DMA_CHAN0_RX_CONTROL, val);

	/* Disable counters */
	WR4(sc, GMAC_MMC_CONTROL,
	    GMAC_MMC_CONTROL_CNTFREEZ |
	    GMAC_MMC_CONTROL_CNTPRST |
	    GMAC_MMC_CONTROL_CNTPRSTLVL);

	/* Configure operation modes */
	tqs = MAX(128U << ((sc->hw_feature[1] &
	    GMAC_MAC_HW_FEATURE1_TXFIFO_MASK) >>
	    GMAC_MAC_HW_FEATURE1_TXFIFO_SHIFT), 256U) / 256 - 1;
	rqs = MAX(128U << ((sc->hw_feature[1] &
	    GMAC_MAC_HW_FEATURE1_RXFIFO_MASK) >>
	    GMAC_MAC_HW_FEATURE1_RXFIFO_SHIFT), 256U) / 256 - 1;
	val = RD4(sc, GMAC_MTL_TXQ0_OPERATION_MODE);
	val &= ~(GMAC_MTL_TXQ0_OPERATION_MODE_TQS_MASK |
	    GMAC_MTL_TXQ0_OPERATION_MODE_TXQEN_MASK);
	val |= tqs << GMAC_MTL_TXQ0_OPERATION_MODE_TQS_SHIFT |
	    GMAC_MTL_TXQ0_OPERATION_MODE_TSF |
	    GMAC_MTL_TXQ0_OPERATION_MODE_TXQEN_EN;
	WR4(sc, GMAC_MTL_TXQ0_OPERATION_MODE, val);
	val = RD4(sc, GMAC_MTL_RXQ0_OPERATION_MODE);
	val &= ~GMAC_MTL_RXQ0_OPERATION_MODE_RQS_MASK;
	val |= rqs << GMAC_MTL_RXQ0_OPERATION_MODE_RQS_SHIFT |
	    GMAC_MTL_RXQ0_OPERATION_MODE_RSF |
	    GMAC_MTL_RXQ0_OPERATION_MODE_FEP |
	    GMAC_MTL_RXQ0_OPERATION_MODE_FUP;
	WR4(sc, GMAC_MTL_RXQ0_OPERATION_MODE, val);

	/* Enable flow control */
	val = RD4(sc, GMAC_MAC_Q0_TX_FLOW_CTRL);
	val |= 0xFFFFU << GMAC_MAC_Q0_TX_FLOW_CTRL_PT_SHIFT;
	val |= GMAC_MAC_Q0_TX_FLOW_CTRL_TFE;
	WR4(sc, GMAC_MAC_Q0_TX_FLOW_CTRL, val);
	val = RD4(sc, GMAC_MAC_RX_FLOW_CTRL);
	val |= GMAC_MAC_RX_FLOW_CTRL_RFE;
	WR4(sc, GMAC_MAC_RX_FLOW_CTRL, val);

	/* set RX queue mode. must be in DCB mode. */
	WR4(sc, GMAC_RXQ_CTRL0, (GMAC_RXQ_CTRL0_EN_MASK << 16) |
	    GMAC_RXQ_CTRL0_EN_DCB);

	/* Enable transmitter and receiver */
	val = RD4(sc, GMAC_MAC_CONFIGURATION);
	val |= GMAC_MAC_CONFIGURATION_BE;
	val |= GMAC_MAC_CONFIGURATION_JD;
	val |= GMAC_MAC_CONFIGURATION_JE;
	val |= GMAC_MAC_CONFIGURATION_DCRS;
	if ((if_getcapenable(ifp) & IFCAP_RXCSUM) != 0)
		val |= GMAC_MAC_CONFIGURATION_IPC;
	else
		val &= ~GMAC_MAC_CONFIGURATION_IPC;
	val |= GMAC_MAC_CONFIGURATION_TE;
	val |= GMAC_MAC_CONFIGURATION_RE;
	WR4(sc, GMAC_MAC_CONFIGURATION, val);

	eqos_enable_intr(sc);

	if_setdrvflagbits(ifp, IFF_DRV_RUNNING, IFF_DRV_OACTIVE);

	if (sc->fixed_link) {
		eqos_set_link(sc, IFM_AVALID | IFM_ACTIVE,
		    sc->fixed_ifmedia.ifm_cur->ifm_media);
	} else {
		mii = device_get_softc(sc->miibus);
		mii_mediachg(mii);
	}
	callout_reset(&sc->callout, hz, eqos_tick, sc);

	EQOS_UNLOCK(sc);
}

static void
eqos_start_locked(if_t ifp)
{
	struct eqos_softc *sc = if_getsoftc(ifp);
	struct buf_ring *br;
	struct mbuf *m;
	int pending = 0;

	if (!sc->link_up)
		return;

	if ((if_getdrvflags(ifp) & (IFF_DRV_RUNNING|IFF_DRV_OACTIVE)) !=
	    IFF_DRV_RUNNING)
		return;

	while (true) {
		if (TX_QUEUED(sc->tx.head, sc->tx.tail) >=
		    TX_DESC_COUNT - TX_MAX_SEGS) {
			if_setdrvflagbits(ifp, IFF_DRV_OACTIVE, 0);
			break;
		}

		br = !buf_ring_empty(sc->tx_control_br) ?
		    sc->tx_control_br : sc->tx_data_br;
		if ((m = drbr_peek(ifp, br)) == NULL)
			break;

		if (eqos_setup_txbuf(sc, &m)) {
			if (m != NULL) {
				drbr_putback(ifp, br, m);
				if_setdrvflagbits(ifp, IFF_DRV_OACTIVE, 0);
				break;
			}
			drbr_advance(ifp, br);
			if_inc_counter(ifp, IFCOUNTER_OERRORS, 1);
			continue;
		}
		drbr_advance(ifp, br);
		if_bpfmtap(ifp, m);
		pending++;
	}

	if (pending) {
		bus_dmamap_sync(sc->tx.desc_tag, sc->tx.desc_map,
		    BUS_DMASYNC_PREREAD | BUS_DMASYNC_PREWRITE);

		/* Start and run TX DMA */
		WR4(sc, GMAC_DMA_CHAN0_TX_END_ADDR,
		    (uint32_t)sc->tx.desc_ring_paddr + DESC_OFFSET(sc->tx.head));
		sc->tx_watchdog = WATCHDOG_TIMEOUT_SECS;
	}
}

static int
eqos_transmit(if_t ifp, struct mbuf *m)
{
	struct eqos_softc *sc = if_getsoftc(ifp);
	struct buf_ring *br;
	int error;

	EQOS_LOCK(sc);
	br = m->m_pkthdr.len <= TX_CONTROL_MAX_LEN ?
	    sc->tx_control_br : sc->tx_data_br;
	error = drbr_enqueue(ifp, br, m);
	if (error == 0)
		eqos_start_locked(ifp);
	else
		if_inc_counter(ifp, IFCOUNTER_OQDROPS, 1);
	EQOS_UNLOCK(sc);

	return (error);
}

static void
eqos_qflush(if_t ifp)
{
	struct eqos_softc *sc = if_getsoftc(ifp);

	EQOS_LOCK(sc);
	drbr_flush(ifp, sc->tx_control_br);
	drbr_flush(ifp, sc->tx_data_br);
	EQOS_UNLOCK(sc);
}

static void
eqos_stop(struct eqos_softc *sc)
{
	if_t ifp = sc->ifp;
	uint32_t val;
	int retry;

	EQOS_LOCK(sc);

	if_setdrvflagbits(ifp, 0, IFF_DRV_RUNNING | IFF_DRV_OACTIVE);

	callout_stop(&sc->callout);

	/* Disable receiver */
	val = RD4(sc, GMAC_MAC_CONFIGURATION);
	val &= ~GMAC_MAC_CONFIGURATION_RE;
	WR4(sc, GMAC_MAC_CONFIGURATION, val);

	/* Stop receive DMA */
	val = RD4(sc, GMAC_DMA_CHAN0_RX_CONTROL);
	val &= ~GMAC_DMA_CHAN0_RX_CONTROL_START;
	WR4(sc, GMAC_DMA_CHAN0_RX_CONTROL, val);

	/* Stop transmit DMA */
	val = RD4(sc, GMAC_DMA_CHAN0_TX_CONTROL);
	val &= ~GMAC_DMA_CHAN0_TX_CONTROL_START;
	WR4(sc, GMAC_DMA_CHAN0_TX_CONTROL, val);

	/* Flush data in the TX FIFO */
	val = RD4(sc, GMAC_MTL_TXQ0_OPERATION_MODE);
	val |= GMAC_MTL_TXQ0_OPERATION_MODE_FTQ;
	WR4(sc, GMAC_MTL_TXQ0_OPERATION_MODE, val);
	for (retry = 10000; retry > 0; retry--) {
		val = RD4(sc, GMAC_MTL_TXQ0_OPERATION_MODE);
		if (!(val & GMAC_MTL_TXQ0_OPERATION_MODE_FTQ))
			break;
		DELAY(10);
	}
	if (!retry)
		device_printf(sc->dev, "timeout flushing TX queue\n");

	/* Disable transmitter */
	val = RD4(sc, GMAC_MAC_CONFIGURATION);
	val &= ~GMAC_MAC_CONFIGURATION_TE;
	WR4(sc, GMAC_MAC_CONFIGURATION, val);

	eqos_disable_intr(sc);
	if (sc->rx_mbuf_head != NULL)
		m_freem(sc->rx_mbuf_head);
	sc->rx_mbuf_head = sc->rx_mbuf_tail = NULL;
	sc->rx_mbuf_len = 0;
	sc->rx_mbuf_error = false;

	EQOS_UNLOCK(sc);
}

static void
eqos_rxintr(struct eqos_softc *sc)
{
	if_t ifp = sc->ifp;
	struct mbuf *m, *new_m;
	bool lro;
	uint32_t rdes1, rdes3;
	int error, length, rx_frames, seglen;

	lro = sc->lro_initialized &&
	    (if_getcapenable(ifp) & IFCAP_LRO) != 0;
	rx_frames = 0;
	while (true) {
		rdes3 = le32toh(sc->rx.desc_ring[sc->rx.head].des3);
		if ((rdes3 & EQOS_RDES3_OWN))
			break;
		rdes1 = le32toh(sc->rx.desc_ring[sc->rx.head].des1);

		if (rdes3 & (EQOS_RDES3_OE | EQOS_RDES3_RE))
			printf("Receive error rdes3=%08x\n", rdes3);

		bus_dmamap_sync(sc->rx.buf_tag,
		    sc->rx.buf_map[sc->rx.head].map, BUS_DMASYNC_POSTREAD);
		bus_dmamap_unload(sc->rx.buf_tag,
		    sc->rx.buf_map[sc->rx.head].map);

		m = sc->rx.buf_map[sc->rx.head].mbuf;
		sc->rx.buf_map[sc->rx.head].mbuf = NULL;
		length = rdes3 & EQOS_RDES3_LENGTH_MASK;
		if ((rdes3 & EQOS_RDES3_FD) != 0) {
			if (sc->rx_mbuf_head != NULL) {
				m_freem(sc->rx_mbuf_head);
				if_inc_counter(ifp, IFCOUNTER_IERRORS, 1);
			}
			sc->rx_mbuf_head = sc->rx_mbuf_tail = m;
			sc->rx_mbuf_len = 0;
			sc->rx_mbuf_error = false;
		} else if (sc->rx_mbuf_head != NULL) {
			m->m_flags &= ~M_PKTHDR;
			sc->rx_mbuf_tail->m_next = m;
			sc->rx_mbuf_tail = m;
		} else {
			m_freem(m);
			m = NULL;
			if_inc_counter(ifp, IFCOUNTER_IERRORS, 1);
		}

		if (m != NULL) {
			sc->rx_mbuf_error |=
			    (rdes3 & (EQOS_RDES3_ES | EQOS_RDES3_OE |
			    EQOS_RDES3_RE)) != 0;
			seglen = RX_DMA_SIZE(sc);
			if ((rdes3 & EQOS_RDES3_LD) != 0) {
				/* The FCS may straddle the final two DMA buffers. */
				if (length <= sc->rx_mbuf_len ||
				    length > sc->rx_mbuf_len + seglen) {
					sc->rx_mbuf_error = true;
				} else
					seglen = length - sc->rx_mbuf_len;
			}
			m->m_len = seglen;
			sc->rx_mbuf_len += seglen;
		}

		if (m != NULL && (rdes3 & EQOS_RDES3_LD) != 0) {
			m = sc->rx_mbuf_head;
			sc->rx_mbuf_head = sc->rx_mbuf_tail = NULL;
			if (sc->rx_mbuf_error) {
				m_freem(m);
				if_inc_counter(ifp, IFCOUNTER_IERRORS, 1);
				m = NULL;
			} else {
				m->m_pkthdr.rcvif = ifp;
				m->m_pkthdr.len = sc->rx_mbuf_len;
				m->m_nextpkt = NULL;
				if ((if_getcapenable(ifp) & IFCAP_RXCSUM) != 0 &&
				    (rdes1 & (EQOS_RDES1_IP_HDR_ERROR |
				    EQOS_RDES1_IP_CSUM_BYPASSED |
				    EQOS_RDES1_IP_PAYLOAD_ERROR)) == 0 &&
				    (rdes1 & (EQOS_RDES1_IPV4_HEADER |
				    EQOS_RDES1_IPV6_HEADER)) != 0) {
					m->m_pkthdr.csum_flags = CSUM_DATA_VALID |
					    CSUM_PSEUDO_HDR;
					m->m_pkthdr.csum_data = 0xffff;
					if ((rdes1 & EQOS_RDES1_IPV4_HEADER) != 0)
						m->m_pkthdr.csum_flags |=
						    CSUM_IP_CHECKED | CSUM_IP_VALID;
				}

				/* Remove trailing FCS */
				m_adj(m, -ETHER_CRC_LEN);

				EQOS_UNLOCK(sc);
				if (!lro ||
				    (m->m_pkthdr.csum_flags & (CSUM_DATA_VALID |
				    CSUM_PSEUDO_HDR)) != (CSUM_DATA_VALID |
				    CSUM_PSEUDO_HDR) ||
				    tcp_lro_rx(&sc->lro, m, 0) != 0)
					if_input(ifp, m);
				EQOS_LOCK(sc);
				if_inc_counter(ifp, IFCOUNTER_IPACKETS, 1);
			}
			sc->rx_mbuf_len = 0;
			sc->rx_mbuf_error = false;
		}

		if ((new_m = eqos_alloc_rxbuf(sc))) {
			if ((error = eqos_setup_rxbuf(sc, sc->rx.head, new_m)))
				printf("ERROR: Hole in RX ring!!\n");
		}
		else {
			if_inc_counter(ifp, IFCOUNTER_IERRORS, 1);
		}

		WR4(sc, GMAC_DMA_CHAN0_RX_END_ADDR,
		    (uint32_t)sc->rx.desc_ring_paddr + DESC_OFFSET(sc->rx.head));

		sc->rx.head = RX_NEXT(sc->rx.head);
		if (++rx_frames == RX_TX_SERVICE_FRAMES) {
			eqos_txintr(sc);
			rx_frames = 0;
		}
	}
	if (lro && !LIST_EMPTY(&sc->lro.lro_active)) {
		EQOS_UNLOCK(sc);
		tcp_lro_flush_all(&sc->lro);
		EQOS_LOCK(sc);
	}
}

static void
eqos_txintr(struct eqos_softc *sc)
{
	if_t ifp = sc->ifp;
	struct eqos_bufmap *bmap;
	uint32_t tdes3;
	u_int pktlen;

	EQOS_ASSERT_LOCKED(sc);

	while (sc->tx.tail != sc->tx.head) {
		pktlen = 0;
		tdes3 = le32toh(sc->tx.desc_ring[sc->tx.tail].des3);
		if ((tdes3 & EQOS_TDES3_OWN))
			break;

		bmap = &sc->tx.buf_map[sc->tx.tail];
		if (bmap->mbuf) {
			pktlen = bmap->mbuf->m_pkthdr.len;
			bus_dmamap_sync(sc->tx.buf_tag, bmap->map,
			    BUS_DMASYNC_POSTWRITE);
			bus_dmamap_unload(sc->tx.buf_tag, bmap->map);
			m_freem(bmap->mbuf);
			bmap->mbuf = NULL;
		}

		eqos_setup_txdesc(sc, sc->tx.tail, 0, 0, 0, 0, false);

		if_setdrvflagbits(ifp, 0, IFF_DRV_OACTIVE);

		/* Last descriptor in a packet contains DMA status */
		if ((tdes3 & EQOS_TDES3_LD)) {
			if ((tdes3 & EQOS_TDES3_DE)) {
				if_inc_counter(ifp, IFCOUNTER_OERRORS, 1);
			} else if ((tdes3 & EQOS_TDES3_ES)) {
				if_inc_counter(ifp, IFCOUNTER_OERRORS, 1);
			} else {
				if_inc_counter(ifp, IFCOUNTER_OPACKETS, 1);
				if_inc_counter(ifp, IFCOUNTER_OBYTES, pktlen);
			}
		}
		sc->tx.tail = TX_NEXT(sc->tx.tail);
	}
	if (sc->tx.tail == sc->tx.head)
		sc->tx_watchdog = 0;
	eqos_start_locked(sc->ifp);
}

static void
eqos_intr_mtl(struct eqos_softc *sc, uint32_t mtl_status)
{
	uint32_t mtl_istat = 0;

	if ((mtl_status & GMAC_MTL_INTERRUPT_STATUS_Q0IS)) {
		uint32_t mtl_clear = 0;

		mtl_istat = RD4(sc, GMAC_MTL_Q0_INTERRUPT_CTRL_STATUS);
		if ((mtl_istat & GMAC_MTL_Q0_INTERRUPT_CTRL_STATUS_RXOVFIS)) {
			mtl_clear |= GMAC_MTL_Q0_INTERRUPT_CTRL_STATUS_RXOVFIS;
		}
		if ((mtl_istat & GMAC_MTL_Q0_INTERRUPT_CTRL_STATUS_TXUNFIS)) {
			mtl_clear |= GMAC_MTL_Q0_INTERRUPT_CTRL_STATUS_TXUNFIS;
		}
		if (mtl_clear) {
			mtl_clear |= (mtl_istat &
			    (GMAC_MTL_Q0_INTERRUPT_CTRL_STATUS_RXOIE |
			    GMAC_MTL_Q0_INTERRUPT_CTRL_STATUS_TXUIE));
			WR4(sc, GMAC_MTL_Q0_INTERRUPT_CTRL_STATUS, mtl_clear);
		}
	}
	if (bootverbose)
		device_printf(sc->dev,
		    "GMAC_MTL_INTERRUPT_STATUS = 0x%08X, "
		    "GMAC_MTL_INTERRUPT_STATUS_Q0IS = 0x%08X\n",
		    mtl_status, mtl_istat);
}

static void
eqos_tick(void *softc)
{
	struct eqos_softc *sc = softc;
	struct mii_data *mii;
	bool link_status;

	EQOS_ASSERT_LOCKED(sc);

	if (sc->tx.tail != sc->tx.head)
		eqos_txintr(sc);
	if (sc->tx_watchdog > 0)
		if (!--sc->tx_watchdog) {
			device_printf(sc->dev, "watchdog timeout\n");
			eqos_txintr(sc);
		}

	link_status = sc->link_up;
	if (!sc->fixed_link) {
		mii = device_get_softc(sc->miibus);
		mii_tick(mii);
	}
	if (sc->link_up && !link_status)
		eqos_start_locked(sc->ifp);

	callout_reset(&sc->callout, hz, eqos_tick, sc);
}

static void
eqos_intr(void *arg)
{
	struct eqos_softc *sc = arg;
	uint32_t mac_status, mtl_status, dma_status, rx_tx_status;

	mac_status = RD4(sc, GMAC_MAC_INTERRUPT_STATUS);
	mac_status &= RD4(sc, GMAC_MAC_INTERRUPT_ENABLE);

	if (mac_status)
		device_printf(sc->dev, "MAC interrupt\n");

	if ((mtl_status = RD4(sc, GMAC_MTL_INTERRUPT_STATUS)))
		eqos_intr_mtl(sc, mtl_status);

	dma_status = RD4(sc, GMAC_DMA_CHAN0_STATUS);
	dma_status &= RD4(sc, GMAC_DMA_CHAN0_INTR_ENABLE);

	if (dma_status)
		WR4(sc, GMAC_DMA_CHAN0_STATUS, dma_status);

	EQOS_LOCK(sc);

	if (dma_status & GMAC_DMA_CHAN0_STATUS_TI)
		eqos_txintr(sc);

	if (dma_status & GMAC_DMA_CHAN0_STATUS_RI)
		eqos_rxintr(sc);

	EQOS_UNLOCK(sc);

	if (!(mac_status | mtl_status | dma_status)) {
		device_printf(sc->dev,
		    "spurious interrupt mac=%08x mtl=%08x dma=%08x\n",
		    RD4(sc, GMAC_MAC_INTERRUPT_STATUS),
		    RD4(sc, GMAC_MTL_INTERRUPT_STATUS),
		    RD4(sc, GMAC_DMA_CHAN0_STATUS));
	}
	if ((rx_tx_status = RD4(sc, GMAC_MAC_RX_TX_STATUS)))
		device_printf(sc->dev, "RX/TX status interrupt\n");
}

static int
eqos_ioctl(if_t ifp, u_long cmd, caddr_t data)
{
	struct eqos_softc *sc = if_getsoftc(ifp);
	struct ifreq *ifr = (struct ifreq *)data;
	struct mii_data *mii;
	uint32_t val;
	int flags, mask;
	int error = 0;

	switch (cmd) {
	case SIOCSIFMTU:
		if (ifr->ifr_mtu < ETHERMIN || ifr->ifr_mtu > sc->max_mtu)
			error = EINVAL;
		else
			if_setmtu(ifp, ifr->ifr_mtu);
		break;

	case SIOCSIFFLAGS:
		if (if_getflags(ifp) & IFF_UP) {
			if (if_getdrvflags(ifp) & IFF_DRV_RUNNING) {
				flags = if_getflags(ifp);
				if ((flags & (IFF_PROMISC|IFF_ALLMULTI))) {
					EQOS_LOCK(sc);
					eqos_setup_rxfilter(sc);
					EQOS_UNLOCK(sc);
				}
			}
			else {
				eqos_init(sc);
			}
		}
		else {
			if (if_getdrvflags(ifp) & IFF_DRV_RUNNING)
				eqos_stop(sc);
		}
		break;

	case SIOCADDMULTI:
	case SIOCDELMULTI:
		if (if_getdrvflags(ifp) & IFF_DRV_RUNNING) {
			EQOS_LOCK(sc);
			eqos_setup_rxfilter(sc);
			EQOS_UNLOCK(sc);
		}
		break;

	case SIOCSIFMEDIA:
	case SIOCGIFMEDIA:
		if (sc->fixed_link) {
			error = ifmedia_ioctl(ifp, ifr, &sc->fixed_ifmedia,
			    cmd);
		} else {
			mii = device_get_softc(sc->miibus);
			error = ifmedia_ioctl(ifp, ifr, &mii->mii_media, cmd);
		}
		break;

	case SIOCSIFCAP:
		mask = ifr->ifr_reqcap ^ if_getcapenable(ifp);
		if (mask & IFCAP_VLAN_MTU)
			if_togglecapenable(ifp, IFCAP_VLAN_MTU);
		if (mask & IFCAP_RXCSUM)
			if_togglecapenable(ifp, IFCAP_RXCSUM);
		if (mask & IFCAP_TXCSUM)
			if_togglecapenable(ifp, IFCAP_TXCSUM);
		if (mask & IFCAP_TXCSUM_IPV6)
			if_togglecapenable(ifp, IFCAP_TXCSUM_IPV6);
		if (mask & IFCAP_TSO4)
			if_togglecapenable(ifp, IFCAP_TSO4);
		if (mask & IFCAP_VLAN_HWTSO)
			if_togglecapenable(ifp, IFCAP_VLAN_HWTSO);
		if ((if_getcapenable(ifp) & IFCAP_TXCSUM))
			if_sethwassistbits(ifp,
			    CSUM_IP | CSUM_UDP | CSUM_TCP, 0);
		else
			if_sethwassistbits(ifp,
			    0, CSUM_IP | CSUM_UDP | CSUM_TCP);
		if ((if_getcapenable(ifp) & IFCAP_TXCSUM_IPV6))
			if_sethwassistbits(ifp,
			    CSUM_UDP_IPV6 | CSUM_TCP_IPV6, 0);
		else
			if_sethwassistbits(ifp,
			    0, CSUM_UDP_IPV6 | CSUM_TCP_IPV6);
		if ((if_getcapenable(ifp) & IFCAP_TSO4))
			if_sethwassistbits(ifp, CSUM_IP_TSO, 0);
		else
			if_sethwassistbits(ifp, 0, CSUM_IP_TSO);
		if (mask & IFCAP_TSO4) {
			EQOS_LOCK(sc);
			val = RD4(sc, GMAC_DMA_CHAN0_TX_CONTROL);
			if ((if_getcapenable(ifp) & IFCAP_TSO4) != 0)
				val |= GMAC_DMA_CHAN0_TX_CONTROL_TSE;
			else
				val &= ~GMAC_DMA_CHAN0_TX_CONTROL_TSE;
			WR4(sc, GMAC_DMA_CHAN0_TX_CONTROL, val);
			EQOS_UNLOCK(sc);
		}
		if (mask & IFCAP_LRO)
			if_togglecapenable(ifp, IFCAP_LRO);
		break;

	default:
		error = ether_ioctl(ifp, cmd, data);
		break;
	}

	return (error);
}

static void
eqos_get_eaddr(struct eqos_softc *sc, uint8_t *eaddr)
{
	uint32_t maclo, machi;

	maclo = htobe32(RD4(sc, GMAC_MAC_ADDRESS0_LOW));
	machi = htobe16(RD4(sc, GMAC_MAC_ADDRESS0_HIGH) & 0xFFFF);

	/* if no valid MAC address generate random */
	if (maclo == 0xffffffff && machi == 0xffff) {
		maclo = 0xf2 | (arc4random() & 0xffff0000);
		machi = arc4random() & 0x0000ffff;
	}
	eaddr[0] = maclo & 0xff;
	eaddr[1] = (maclo >> 8) & 0xff;
	eaddr[2] = (maclo >> 16) & 0xff;
	eaddr[3] = (maclo >> 24) & 0xff;
	eaddr[4] = machi & 0xff;
	eaddr[5] = (machi >> 8) & 0xff;
}

static void
eqos_axi_configure(struct eqos_softc *sc)
{
	uint32_t val;

	val = RD4(sc, GMAC_DMA_SYSBUS_MODE);

	/* Max Write Outstanding Req Limit */
	val &= ~GMAC_DMA_SYSBUS_MODE_WR_OSR_LMT_MASK;
	val |= 0x03 << GMAC_DMA_SYSBUS_MODE_WR_OSR_LMT_SHIFT;

	/* Max Read Outstanding Req Limit */
	val &= ~GMAC_DMA_SYSBUS_MODE_RD_OSR_LMT_MASK;
	val |= 0x07 << GMAC_DMA_SYSBUS_MODE_RD_OSR_LMT_SHIFT;

	/* Allowed Burst Length's */
	val |= GMAC_DMA_SYSBUS_MODE_BLEN16;
	val |= GMAC_DMA_SYSBUS_MODE_BLEN8;
	val |= GMAC_DMA_SYSBUS_MODE_BLEN4;

	/* Fixed Burst Length */
	val |= GMAC_DMA_SYSBUS_MODE_MB;

	WR4(sc, GMAC_DMA_SYSBUS_MODE, val);
}

static void
eqos_get1paddr(void *arg, bus_dma_segment_t *segs, int nsegs, int error)
{

	if (!error)
		*(bus_addr_t *)arg = segs[0].ds_addr;
}

static int
eqos_setup_dma(struct eqos_softc *sc)
{
	struct mbuf *m;
	int error, i;

	/* Set up TX descriptor ring, descriptors, and dma maps */
	if ((error = bus_dma_tag_create(bus_get_dma_tag(sc->dev),
					DESC_ALIGN, DESC_BOUNDARY,
					BUS_SPACE_MAXADDR_32BIT,
					BUS_SPACE_MAXADDR, NULL, NULL,
					TX_DESC_SIZE, 1, TX_DESC_SIZE, 0,
					NULL, NULL, &sc->tx.desc_tag))) {
		device_printf(sc->dev, "could not create TX ring DMA tag\n");
		return (error);
	}

	if ((error = bus_dmamem_alloc(sc->tx.desc_tag,
	    (void**)&sc->tx.desc_ring,
	    BUS_DMA_COHERENT | BUS_DMA_WAITOK | BUS_DMA_ZERO,
	    &sc->tx.desc_map))) {
		device_printf(sc->dev,
		    "could not allocate TX descriptor ring.\n");
		return (error);
	}

	if ((error = bus_dmamap_load(sc->tx.desc_tag, sc->tx.desc_map,
	    sc->tx.desc_ring,
	    TX_DESC_SIZE, eqos_get1paddr, &sc->tx.desc_ring_paddr, 0))) {
		device_printf(sc->dev,
		    "could not load TX descriptor ring map.\n");
		return (error);
	}

	if ((error = bus_dma_tag_create(bus_get_dma_tag(sc->dev), 1, 0,
					BUS_SPACE_MAXADDR_32BIT,
					BUS_SPACE_MAXADDR, NULL, NULL,
					MCLBYTES*TX_MAX_SEGS, TX_MAX_SEGS,
					MCLBYTES, 0, NULL, NULL,
					&sc->tx.buf_tag))) {
		device_printf(sc->dev, "could not create TX buffer DMA tag.\n");
		return (error);
	}

	for (i = 0; i < TX_DESC_COUNT; i++) {
		if ((error = bus_dmamap_create(sc->tx.buf_tag, BUS_DMA_COHERENT,
		    &sc->tx.buf_map[i].map))) {
			device_printf(sc->dev, "cannot create TX buffer map\n");
			return (error);
		}
		eqos_setup_txdesc(sc, i, EQOS_TDES3_OWN, 0, 0, 0, false);
	}

	/* Set up RX descriptor ring, descriptors, dma maps, and mbufs */
	if ((error = bus_dma_tag_create(bus_get_dma_tag(sc->dev),
					DESC_ALIGN, DESC_BOUNDARY,
					BUS_SPACE_MAXADDR_32BIT,
					BUS_SPACE_MAXADDR, NULL, NULL,
					RX_DESC_SIZE, 1, RX_DESC_SIZE, 0,
					NULL, NULL, &sc->rx.desc_tag))) {
		device_printf(sc->dev, "could not create RX ring DMA tag.\n");
		return (error);
	}

	if ((error = bus_dmamem_alloc(sc->rx.desc_tag,
	    (void **)&sc->rx.desc_ring,
	    BUS_DMA_COHERENT | BUS_DMA_WAITOK | BUS_DMA_ZERO,
	    &sc->rx.desc_map))) {
		device_printf(sc->dev,
		    "could not allocate RX descriptor ring.\n");
		return (error);
	}

	if ((error = bus_dmamap_load(sc->rx.desc_tag, sc->rx.desc_map,
	    sc->rx.desc_ring, RX_DESC_SIZE, eqos_get1paddr,
	    &sc->rx.desc_ring_paddr, 0))) {
		device_printf(sc->dev,
		    "could not load RX descriptor ring map.\n");
		return (error);
	}

	if ((error = bus_dma_tag_create(bus_get_dma_tag(sc->dev), 1, 0,
					BUS_SPACE_MAXADDR_32BIT,
					BUS_SPACE_MAXADDR, NULL, NULL,
					sc->rx_buf_size, 1,
					sc->rx_buf_size, 0, NULL, NULL,
					&sc->rx.buf_tag))) {
		device_printf(sc->dev, "could not create RX buf DMA tag.\n");
		return (error);
	}

	for (i = 0; i < RX_DESC_COUNT; i++) {
		if ((error = bus_dmamap_create(sc->rx.buf_tag, BUS_DMA_COHERENT,
		    &sc->rx.buf_map[i].map))) {
			device_printf(sc->dev, "cannot create RX buffer map\n");
			return (error);
		}
		if (!(m = eqos_alloc_rxbuf(sc))) {
			device_printf(sc->dev, "cannot allocate RX mbuf\n");
			return (ENOMEM);
		}
		if ((error = eqos_setup_rxbuf(sc, i, m))) {
			device_printf(sc->dev, "cannot create RX buffer\n");
			return (error);
		}
	}

	if (bootverbose)
		device_printf(sc->dev, "TX ring @ 0x%lx, RX ring @ 0x%lx\n",
		    sc->tx.desc_ring_paddr, sc->rx.desc_ring_paddr);
	return (0);
}

static int
eqos_attach(device_t dev)
{
	struct eqos_softc *sc = device_get_softc(dev);
#ifdef FDT
	phandle_t fixed_link;
#endif
	if_t ifp;
	uint32_t ver;
	uint8_t eaddr[ETHER_ADDR_LEN];
	u_int userver, snpsver;
	int error;
	int n;

	/* setup resources */
	if (bus_alloc_resources(dev, eqos_spec, sc->res)) {
		device_printf(dev, "Could not allocate resources\n");
		bus_release_resources(dev, eqos_spec, sc->res);
		return (ENXIO);
	}

	if ((error = IF_EQOS_INIT(dev)))
		return (error);
	if (sc->max_mtu == 0)
		sc->max_mtu = ETHERMTU;
	if (sc->max_mtu < ETHERMTU || sc->max_mtu > EQOS_MAX_MTU) {
		device_printf(dev, "invalid max-frame-size %u\n", sc->max_mtu);
		return (EINVAL);
	}
	sc->rx_buf_size = MCLBYTES;

	sc->dev = dev;
	ver  = RD4(sc, GMAC_MAC_VERSION);
	userver = (ver & GMAC_MAC_VERSION_USERVER_MASK) >>
	    GMAC_MAC_VERSION_USERVER_SHIFT;
	snpsver = ver & GMAC_MAC_VERSION_SNPSVER_MASK;

	if (snpsver != 0x51) {
		device_printf(dev, "EQOS version 0x%02x not supported\n",
		    snpsver);
		return (ENXIO);
	}

	for (n = 0; n < 4; n++)
		sc->hw_feature[n] = RD4(sc, GMAC_MAC_HW_FEATURE(n));

	if (bootverbose) {
		device_printf(dev, "DesignWare EQOS ver 0x%02x (0x%02x)\n",
		    snpsver, userver);
		device_printf(dev, "hw features %08x %08x %08x %08x\n",
		    sc->hw_feature[0], sc->hw_feature[1],
		    sc->hw_feature[2], sc->hw_feature[3]);
	}

	mtx_init(&sc->lock, "eqos lock", MTX_NETWORK_LOCK, MTX_DEF);
	callout_init_mtx(&sc->callout, &sc->lock, 0);
	sc->tx_control_br = buf_ring_alloc(TX_CONTROL_QUEUE_COUNT, M_DEVBUF,
	    M_WAITOK,
	    &sc->lock);
	sc->tx_data_br = buf_ring_alloc(TX_DATA_QUEUE_COUNT, M_DEVBUF, M_WAITOK,
	    &sc->lock);

	eqos_get_eaddr(sc, eaddr);
	if (bootverbose)
		device_printf(sc->dev, "Ethernet address %6D\n", eaddr, ":");

	/* Setup DMA descriptors */
	if (eqos_setup_dma(sc)) {
		device_printf(sc->dev, "failed to setup DMA descriptors\n");
		return (EINVAL);
	}

	/* setup interrupt delivery */
	if ((bus_setup_intr(dev, sc->res[EQOS_RES_IRQ0], EQOS_INTR_FLAGS,
	    NULL, eqos_intr, sc, &sc->irq_handle))) {
		device_printf(dev, "unable to setup 1st interrupt\n");
		bus_release_resources(dev, eqos_spec, sc->res);
		return (ENXIO);
	}

	/* Setup ethernet interface */
	ifp = sc->ifp = if_alloc(IFT_ETHER);
	if_setsoftc(ifp, sc);
	if_initname(ifp, device_get_name(sc->dev), device_get_unit(sc->dev));
	if_setflags(sc->ifp, IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST);
	if_settransmitfn(ifp, eqos_transmit);
	if_setqflushfn(ifp, eqos_qflush);
	if_setioctlfn(ifp, eqos_ioctl);
	if_setinitfn(ifp, eqos_init);
	if_setcapabilities(ifp, IFCAP_VLAN_MTU);
	if ((sc->hw_feature[0] & GMAC_MAC_HW_FEATURE0_TXCOESEL) != 0)
		if_setcapabilitiesbit(ifp, IFCAP_TXCSUM | IFCAP_TXCSUM_IPV6, 0);
	if ((sc->hw_feature[0] & GMAC_MAC_HW_FEATURE0_TXCOESEL) != 0 &&
	    (sc->hw_feature[1] & GMAC_MAC_HW_FEATURE1_TSOEN) != 0) {
		if_setcapabilitiesbit(ifp, IFCAP_TSO4 | IFCAP_VLAN_HWTSO, 0);
		if_sethwtsomax(ifp, IP_MAXPACKET);
		if_sethwtsomaxsegcount(ifp, TX_MAX_SEGS);
		if_sethwtsomaxsegsize(ifp, MCLBYTES);
	}
	if ((sc->hw_feature[0] & GMAC_MAC_HW_FEATURE0_RXCOESEL) != 0)
		if_setcapabilitiesbit(ifp, IFCAP_RXCSUM, 0);
	if ((if_getcapabilities(ifp) & IFCAP_RXCSUM) != 0 &&
	    tcp_lro_init(&sc->lro) == 0) {
		sc->lro.ifp = ifp;
		sc->lro_initialized = true;
		if_setcapabilitiesbit(ifp, IFCAP_LRO, 0);
	}
	if_setcapenable(ifp, if_getcapabilities(ifp));
	if ((if_getcapenable(ifp) & IFCAP_TXCSUM) != 0)
		if_sethwassistbits(ifp, CSUM_IP | CSUM_UDP | CSUM_TCP, 0);
	if ((if_getcapenable(ifp) & IFCAP_TXCSUM_IPV6) != 0)
		if_sethwassistbits(ifp, CSUM_UDP_IPV6 | CSUM_TCP_IPV6, 0);
	if ((if_getcapenable(ifp) & IFCAP_TSO4) != 0)
		if_sethwassistbits(ifp, CSUM_IP_TSO, 0);

#ifdef FDT
	fixed_link = ofw_bus_find_child(ofw_bus_get_node(dev), "fixed-link");
	if (fixed_link != 0)
		error = eqos_setup_fixed(sc, fixed_link);
	else
#endif
		error = mii_attach(sc->dev, &sc->miibus, ifp,
		    eqos_media_change, eqos_media_status, BMSR_DEFCAPMASK,
		    MII_PHY_ANY, MII_OFFSET_ANY, 0);
	if (error != 0) {
		device_printf(sc->dev, "media attach failed\n");
		return (error);
	}

#ifdef FDT
	error = eqos_attach_mdio(sc);
	if (error != 0)
		return (error);
#endif

	/* Attach ethernet interface */
	ether_ifattach(ifp, eaddr);

	return (0);
}

static int
eqos_detach(device_t dev)
{
	struct eqos_softc *sc = device_get_softc(dev);
	int i;

	if (device_is_attached(dev)) {
		EQOS_LOCK(sc);
		eqos_stop(sc);
		EQOS_UNLOCK(sc);
		if_setflagbits(sc->ifp, 0, IFF_UP);
		ether_ifdetach(sc->ifp);
	}

	if (sc->miibus)
		device_delete_child(dev, sc->miibus);
	if (sc->fixed_link)
		ifmedia_removeall(&sc->fixed_ifmedia);
	bus_generic_detach(dev);
	if (sc->mdio)
		device_delete_child(dev, sc->mdio);

	if (sc->irq_handle)
		bus_teardown_intr(dev, sc->res[EQOS_RES_IRQ0],
		    sc->irq_handle);

	if (sc->lro_initialized)
		tcp_lro_free(&sc->lro);
	if (sc->ifp) {
		eqos_qflush(sc->ifp);
		if_free(sc->ifp);
	}
	if (sc->tx_control_br)
		buf_ring_free(sc->tx_control_br, M_DEVBUF);
	if (sc->tx_data_br)
		buf_ring_free(sc->tx_data_br, M_DEVBUF);

	bus_release_resources(dev, eqos_spec, sc->res);

	if (sc->tx.desc_tag) {
		if (sc->tx.desc_map) {
			bus_dmamap_unload(sc->tx.desc_tag, sc->tx.desc_map);
			bus_dmamem_free(sc->tx.desc_tag, sc->tx.desc_ring,
			    sc->tx.desc_map);
		}
		bus_dma_tag_destroy(sc->tx.desc_tag);
	}
	if (sc->tx.buf_tag) {
		for (i = 0; i < TX_DESC_COUNT; i++) {
			m_free(sc->tx.buf_map[i].mbuf);
			bus_dmamap_destroy(sc->tx.buf_tag,
			    sc->tx.buf_map[i].map);
		}
		bus_dma_tag_destroy(sc->tx.buf_tag);
	}

	if (sc->rx.desc_tag) {
		if (sc->rx.desc_map) {
			bus_dmamap_unload(sc->rx.desc_tag, sc->rx.desc_map);
			bus_dmamem_free(sc->rx.desc_tag, sc->rx.desc_ring,
			    sc->rx.desc_map);
		}
		bus_dma_tag_destroy(sc->rx.desc_tag);
	}
	if (sc->rx.buf_tag) {
		for (i = 0; i < RX_DESC_COUNT; i++) {
			m_free(sc->rx.buf_map[i].mbuf);
			bus_dmamap_destroy(sc->rx.buf_tag,
			    sc->rx.buf_map[i].map);
		}
		bus_dma_tag_destroy(sc->rx.buf_tag);
	}

	mtx_destroy(&sc->lock);

	return (0);
}


static device_method_t eqos_methods[] = {
	/* Device Interface */
	DEVMETHOD(device_attach,	eqos_attach),
	DEVMETHOD(device_detach,	eqos_detach),

	/* MII Interface */
	DEVMETHOD(miibus_readreg,	eqos_miibus_readreg),
	DEVMETHOD(miibus_writereg,	eqos_miibus_writereg),
	DEVMETHOD(miibus_statchg,	eqos_miibus_statchg),

	/* MDIO Interface */
	DEVMETHOD(mdio_readreg,		eqos_miibus_readreg),
	DEVMETHOD(mdio_writereg,	eqos_miibus_writereg),

	/* Bus Interface */
	DEVMETHOD(bus_add_child,	device_add_child_ordered),

	DEVMETHOD_END
};

driver_t eqos_driver = {
	"eqos",
	eqos_methods,
	sizeof(struct eqos_softc),
};

DRIVER_MODULE(miibus, eqos, miibus_driver, 0, 0);
DRIVER_MODULE(mdio, eqos, mdio_driver, 0, 0);
MODULE_DEPEND(eqos, mdio, 1, 1, 1);
