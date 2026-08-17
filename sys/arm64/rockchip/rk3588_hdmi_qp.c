/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/callout.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/rman.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/taskqueue.h>

#include <machine/bus.h>

#include <crypto/sha2/sha256.h>

#include <dev/extres/clk/clk.h>
#include <dev/extres/syscon/syscon.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#include <dev/ofw/ofw_graph.h>
#include <dev/videomode/videomode.h>
#include <dev/videomode/edidvar.h>

#include "syscon_if.h"

#include "rk3588_hdmi_mode.h"

#define	RK3588_GRF_SOC_CON7		0x031c
#define	RK3588_GRF_SOC_CON2		0x0308
#define	RK3588_GRF_SOC_STATUS1		0x0384
#define	RK3588_HDMI0_HPD_INT_CLR	(1u << 12)
#define	RK3588_HDMI0_HPD_INT_MSK	(1u << 13)
#define	RK3588_HDMI1_HPD_INT_CLR	(1u << 14)
#define	RK3588_HDMI1_HPD_INT_MSK	(1u << 15)
#define	RK3588_HPD_HDMI0_IO_EN		(1u << 12)
#define	RK3588_HPD_HDMI1_IO_EN		(1u << 13)
#define	RK3588_HDMI0_LEVEL_INT		(1u << 16)
#define	RK3588_HDMI1_LEVEL_INT		(1u << 24)

#define	RK3588_GRF_VO1_CON3		0x000c
#define	RK3588_GRF_VO1_CON6		0x0018
#define	RK3588_GRF_VO1_CON9		0x0024
#define	RK3588_SCLIN_MASK		(1u << 9)
#define	RK3588_SDAIN_MASK		(1u << 10)
#define	RK3588_MODE_MASK		(1u << 11)
#define	RK3588_I2S_SEL_MASK		(1u << 13)
#define	RK3588_HDMI0_GRANT_SEL		(1u << 10)
#define	RK3588_HDMI1_GRANT_SEL		(1u << 12)

#define	RK3588_HDMI0_BASE		0xfde80000u
#define	RK3588_HDMI1_BASE		0xfdea0000u

#define	DW_HDMI_QP_CORE_ID		0x0000
#define	DW_HDMI_QP_VER_NUMBER		0x0004
#define	DW_HDMI_QP_VER_TYPE		0x0008
#define	DW_HDMI_QP_CONFIG_REG		0x000c
#define	DW_HDMI_QP_TIMER_BASE_CONFIG0	0x0080
#define	DW_HDMI_QP_I2CM_FM_SCL_CONFIG0	0x00e4
#define	DW_HDMI_QP_I2CM_CONTROL0	0x00ec
#define	DW_HDMI_QP_I2CM_INTERFACE_CONTROL0 0x00f4
#define	DW_HDMI_QP_I2CM_RDDATA_0_3	0x010c
#define	DW_HDMI_QP_MAINUNIT_0_INT_MASK_N 0x3014
#define	DW_HDMI_QP_MAINUNIT_1_INT_STATUS 0x3020
#define	DW_HDMI_QP_MAINUNIT_1_INT_MASK_N 0x3024
#define	DW_HDMI_QP_MAINUNIT_1_INT_CLEAR 0x3028
#define	DW_HDMI_QP_HDCP2LOGIC_CONFIG0	0x08e0
#define	DW_HDMI_QP_SCRAMB_CONFIG0	0x0960
#define	DW_HDMI_QP_LINK_CONFIG0		0x0968
#define	DW_HDMI_QP_PKTSCHED_PKT_CONFIG1	0x0a9c
#define	DW_HDMI_QP_PKTSCHED_PKT_EN	0x0aa8
#define	DW_HDMI_QP_PKT_AVI_CONTENTS0	0x0be0
#define	DW_HDMI_QP_PKT_AVI_CONTENTS1	0x0be4
#define	DW_HDMI_QP_PKT_AVI_CONTENTS2	0x0be8
#define	DW_HDMI_QP_PKT_AVI_CONTENTS3	0x0bec
#define	DW_HDMI_QP_PKT_AVI_CONTENTS4	0x0bf0
#define	DW_HDMI_QP_VIDEO_MONITOR_STATUS0 0x0884

#define	DW_HDMI_QP_HDCP2_BYPASS		(1u << 0)
#define	DW_HDMI_QP_OPMODE_DVI		(1u << 4)
#define	DW_HDMI_QP_AVI_FIELDRATE	(1u << 12)
#define	DW_HDMI_QP_AVI_TX_EN		(1u << 13)
#define	DW_HDMI_QP_GCP_TX_EN		(1u << 3)

#define	DW_HDMI_QP_EXPECTED_CORE_ID	0x48515458u
#define	DW_HDMI_QP_EXPECTED_VERSION	0x30313030u
#define	DW_HDMI_QP_EXPECTED_TYPE	0x6c753030u
#define	DW_HDMI_QP_EXPECTED_CONFIG	0x11e0d501u
#define	RK3588_HDMI_RGB8_MASK		0xffu
#define	DW_HDMI_QP_I2CM_ADDR_MASK	0x000ff000u
#define	DW_HDMI_QP_I2CM_SLVADDR_MASK	0x00000fe0u
#define	DW_HDMI_QP_I2CM_WR_MASK	0x0000001eu
#define	DW_HDMI_QP_I2CM_NBYTES_MASK	0x00f00000u
#define	DW_HDMI_QP_I2CM_16BYTES	(0xfu << 20)
#define	DW_HDMI_QP_I2CM_FM_READ	(1u << 2)
#define	DW_HDMI_QP_I2CM_FM_EN		(1u << 0)
#define	DW_HDMI_QP_I2CM_NACK		(1u << 2)
#define	DW_HDMI_QP_I2CM_READ_REQUEST	(1u << 1)
#define	DW_HDMI_QP_I2CM_DONE		(1u << 0)
#define	DW_HDMI_QP_I2CM_STATUS_MASK	0x7u

#define	DW_HDMI_QP_TIMER_BASE_RATE	428571429u
#define	DW_HDMI_QP_I2CM_FM_SCL_RATE	0x085c085cu
#define	DW_HDMI_QP_DDC_ADDR		0x50u
#define	DW_HDMI_QP_EDID_BLOCK_SIZE	128
#define	DW_HDMI_QP_EDID_MAX_SIZE	256
#define	DW_HDMI_QP_EDID_RETRIES		4
#define	DW_HDMI_QP_DDC_POLL_US		1000
#define	DW_HDMI_QP_DDC_TIMEOUT_US	100000
#define	RK3588_HDMI_HPD_DEBOUNCE_MS	150
#define	RK3588_HDMI_HPD_IRQ_RID	4
#define	RK3588_VOP_HDMI0_RATE_MASK	0xc7u

static const char * const rk3588_hdmi_clock_names[] = {
	"pclk", "earc", "ref", "aud", "hdp", "hclk_vo1"
};

static const char * const rk3588_vop_clock_names[] = {
	"aclk", "hclk", "dclk_vp0", "dclk_vp1", "dclk_vp2",
	"dclk_vp3", "pclk_vop", "pll_hdmiphy0", "pll_hdmiphy1"
};

static const char * const rk3588_vop_clock_sysctl_names[] = {
	"vop_aclk", "vop_hclk", "vop_dclk_vp0", "vop_dclk_vp1",
	"vop_dclk_vp2", "vop_dclk_vp3", "vop_pclk",
	"vop_pll_hdmiphy0", "vop_pll_hdmiphy1"
};

static struct videomode rk3588_hdmi_mode[2];
static bool rk3588_hdmi_mode_valid[2];
struct rk3588_hdmi_qp_softc;
static struct rk3588_hdmi_qp_softc *rk3588_hdmi_qp_sc[2];

int
rk3588_hdmi_qp_get_mode(int port, struct videomode *mode)
{

	if (port < 0 || port >= (int)nitems(rk3588_hdmi_mode) ||
	    !rk3588_hdmi_mode_valid[port])
		return (ENXIO);
	*mode = rk3588_hdmi_mode[port];
	mode->name = NULL;
	return (0);
}

struct rk3588_hdmi_qp_softc {
	device_t	dev;
	struct syscon	*grf;
	struct syscon	*vo_grf;
	struct resource	*mem;
	struct resource	*hpd_irq;
	void		*hpd_irq_cookie;
	struct callout	hpd_callout;
	struct task	hpd_task;
	rman_res_t	base;
	clk_t		clocks[nitems(rk3588_hdmi_clock_names)];
	struct intr_config_hook vop_hook;
	uint64_t	clock_rate[nitems(rk3588_hdmi_clock_names)];
	uint64_t	vop_clock_rate[nitems(rk3588_vop_clock_names)];
	int		clock_error[nitems(rk3588_hdmi_clock_names)];
	int		vop_clock_error[nitems(rk3588_vop_clock_names)];
	uint32_t	clock_mask;
	uint32_t	clock_enable_mask;
	uint32_t	vop_clock_mask;
	uint32_t	vop_clock_rate_mask;
	uint32_t	vop_remote_xref;
	phandle_t	vop_node;
	uint32_t	hpd_mask;
	uint32_t	hpd_last_status;
	uint32_t	hpd_irq_count;
	uint32_t	hpd_event_count;
	uint32_t	hpd_connect_count;
	uint32_t	hpd_disconnect_count;
	uint32_t	phy_xref;
	uint32_t	soc_con7_before;
	uint32_t	soc_con7_after;
	uint32_t	status1_before;
	uint32_t	status1_after_hpd_io;
	uint32_t	vo_con_after;
	uint32_t	vo_con9_after;
	uint32_t	soc_con2_after;
	uint32_t	core_id;
	uint32_t	ver_number;
	uint32_t	ver_type;
	uint32_t	config_reg;
	uint32_t	ddc_last_status;
	uint32_t	ddc_done_count;
	uint32_t	ddc_nack_count;
	uint32_t	ddc_timeout_count;
	uint32_t	edid_checksum_mask;
	uint8_t		edid[DW_HDMI_QP_EDID_MAX_SIZE];
	char		edid_sha256[SHA256_DIGEST_STRING_LENGTH];
	int		ddc_attempted;
	int		ddc_fail_offset;
	int		edid_error;
	int		edid_len;
	int		edid_header_valid;
	int		edid_extension_count;
	struct videomode preferred_mode;
	struct videomode current_mode;
	int		preferred_mode_valid;
	int		preferred_mode_error;
	int		current_mode_valid;
	int		vop_probe_done;
	int		vop_route_endpoint;
	int		vop_route_vp;
	int		output_attempted;
	int		output_configured;
	int		output_error;
	int		hpd_irq_rid;
	int		hpd_irq_error;
	int		hpd_state;
	int		late_attempted;
	int		late_configured;
	int		late_error;
	uint32_t	output_color_after;
	uint32_t	output_link_before;
	uint32_t	output_link_after;
	uint32_t	output_hdcp_before;
	uint32_t	output_hdcp_after;
	uint32_t	output_pkt_before;
	uint32_t	output_pkt_after;
	uint32_t	output_avi0_after;
	uint32_t	output_avi1_after;
	uint32_t	output_avi2_after;
	uint32_t	output_video_status[4];
	int		port;
};

int
rk3588_hdmi_qp_get_hpd(int port, int *connected)
{
	struct rk3588_hdmi_qp_softc *sc;

	if (port < 0 || port >= (int)nitems(rk3588_hdmi_qp_sc) ||
	    connected == NULL)
		return (EINVAL);
	sc = rk3588_hdmi_qp_sc[port];
	if (sc == NULL)
		return (ENXIO);
	*connected = sc->hpd_state;
	return (0);
}

int
rk3588_hdmi_qp_get_edid(int port, uint8_t *edid, size_t capacity,
    size_t *length)
{
	struct rk3588_hdmi_qp_softc *sc;

	if (port < 0 || port >= (int)nitems(rk3588_hdmi_qp_sc) ||
	    edid == NULL || length == NULL)
		return (EINVAL);
	sc = rk3588_hdmi_qp_sc[port];
	if (sc == NULL || !sc->hpd_state)
		return (ENXIO);
	if (sc->edid_error != 0)
		return (sc->edid_error);
	if (sc->edid_len < 128 || sc->edid_len > (int)capacity)
		return (EMSGSIZE);
	memcpy(edid, sc->edid, sc->edid_len);
	*length = sc->edid_len;
	return (0);
}

static void rk3588_hdmi_qp_late_connect(void *, int);
static void rk3588_hdmi_qp_refresh_vop_clocks(
    struct rk3588_hdmi_qp_softc *);

static void
rk3588_hdmi_qp_mod(struct rk3588_hdmi_qp_softc *sc, uint32_t reg,
    uint32_t value, uint32_t mask)
{
	uint32_t current;

	current = bus_read_4(sc->mem, reg);
	bus_write_4(sc->mem, reg, (current & ~mask) | (value & mask));
}

static void
rk3588_hdmi_qp_ddc_init(struct rk3588_hdmi_qp_softc *sc)
{

	/* Keep the sequence identical to Linux dw_hdmi_qp_init_hw(). */
	bus_write_4(sc->mem, DW_HDMI_QP_MAINUNIT_0_INT_MASK_N, 0);
	bus_write_4(sc->mem, DW_HDMI_QP_MAINUNIT_1_INT_MASK_N, 0);
	bus_write_4(sc->mem, DW_HDMI_QP_TIMER_BASE_CONFIG0,
	    DW_HDMI_QP_TIMER_BASE_RATE);
	bus_write_4(sc->mem, DW_HDMI_QP_I2CM_CONTROL0, 1);
	bus_write_4(sc->mem, DW_HDMI_QP_I2CM_FM_SCL_CONFIG0,
	    DW_HDMI_QP_I2CM_FM_SCL_RATE);
	rk3588_hdmi_qp_mod(sc, DW_HDMI_QP_I2CM_INTERFACE_CONTROL0, 0,
	    DW_HDMI_QP_I2CM_FM_EN);
	bus_write_4(sc->mem, DW_HDMI_QP_MAINUNIT_1_INT_CLEAR,
	    DW_HDMI_QP_I2CM_DONE | DW_HDMI_QP_I2CM_NACK);
}

static int
rk3588_hdmi_qp_ddc_read_chunk(struct rk3588_hdmi_qp_softc *sc,
    uint8_t offset)
{
	uint32_t control, data, status;
	int elapsed, i;

	bus_write_4(sc->mem, DW_HDMI_QP_MAINUNIT_1_INT_CLEAR,
	    DW_HDMI_QP_I2CM_STATUS_MASK);
	control = bus_read_4(sc->mem, DW_HDMI_QP_I2CM_INTERFACE_CONTROL0);
	control &= ~(DW_HDMI_QP_I2CM_ADDR_MASK |
	    DW_HDMI_QP_I2CM_SLVADDR_MASK | DW_HDMI_QP_I2CM_NBYTES_MASK |
	    DW_HDMI_QP_I2CM_WR_MASK);
	control |= ((uint32_t)offset << 12) |
	    (DW_HDMI_QP_DDC_ADDR << 5) | DW_HDMI_QP_I2CM_16BYTES;
	bus_write_4(sc->mem, DW_HDMI_QP_I2CM_INTERFACE_CONTROL0, control);
	bus_write_4(sc->mem, DW_HDMI_QP_I2CM_INTERFACE_CONTROL0,
	    control | DW_HDMI_QP_I2CM_FM_READ);

	DELAY(DW_HDMI_QP_DDC_POLL_US);
	status = 0;
	for (elapsed = 0; elapsed < DW_HDMI_QP_DDC_TIMEOUT_US;
	    elapsed += DW_HDMI_QP_DDC_POLL_US) {
		status = bus_read_4(sc->mem,
		    DW_HDMI_QP_MAINUNIT_1_INT_STATUS) &
		    DW_HDMI_QP_I2CM_STATUS_MASK;
		if ((status & (DW_HDMI_QP_I2CM_DONE |
		    DW_HDMI_QP_I2CM_NACK)) != 0)
			break;
		if ((status & DW_HDMI_QP_I2CM_READ_REQUEST) != 0)
			bus_write_4(sc->mem, DW_HDMI_QP_MAINUNIT_1_INT_CLEAR,
			    DW_HDMI_QP_I2CM_READ_REQUEST);
		DELAY(DW_HDMI_QP_DDC_POLL_US);
	}

	sc->ddc_last_status = status;
	rk3588_hdmi_qp_mod(sc, DW_HDMI_QP_I2CM_INTERFACE_CONTROL0, 0,
	    DW_HDMI_QP_I2CM_WR_MASK);
	if (status != 0)
		bus_write_4(sc->mem, DW_HDMI_QP_MAINUNIT_1_INT_CLEAR,
		    status);
	if ((status & DW_HDMI_QP_I2CM_NACK) != 0) {
		sc->ddc_nack_count++;
		bus_write_4(sc->mem, DW_HDMI_QP_I2CM_CONTROL0, 1);
		return (EIO);
	}
	if ((status & DW_HDMI_QP_I2CM_DONE) == 0) {
		sc->ddc_timeout_count++;
		bus_write_4(sc->mem, DW_HDMI_QP_I2CM_CONTROL0, 1);
		return (ETIMEDOUT);
	}

	for (i = 0; i < 16; i += 4) {
		data = bus_read_4(sc->mem, DW_HDMI_QP_I2CM_RDDATA_0_3 + i);
		sc->edid[offset + i] = data;
		sc->edid[offset + i + 1] = data >> 8;
		sc->edid[offset + i + 2] = data >> 16;
		sc->edid[offset + i + 3] = data >> 24;
	}
	sc->ddc_done_count += 16;
	return (0);
}

static int
rk3588_hdmi_qp_ddc_read(struct rk3588_hdmi_qp_softc *sc, int start,
    int length)
{
	int error, i;

	for (i = 0; i < length; i += 16) {
		error = rk3588_hdmi_qp_ddc_read_chunk(sc, start + i);
		if (error != 0) {
			sc->ddc_fail_offset = start + i;
			return (error);
		}
	}
	return (0);
}

static bool
rk3588_hdmi_qp_edid_checksum_valid(const uint8_t *block)
{
	uint8_t sum;
	int i;

	sum = 0;
	for (i = 0; i < DW_HDMI_QP_EDID_BLOCK_SIZE; i++)
		sum += block[i];
	return (sum == 0);
}

static int
rk3588_hdmi_qp_read_edid_block(struct rk3588_hdmi_qp_softc *sc, int block)
{
	static const uint8_t header[8] = {
		0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00
	};
	bool valid;
	int error, start, tries;

	start = block * DW_HDMI_QP_EDID_BLOCK_SIZE;
	for (tries = 0; tries < DW_HDMI_QP_EDID_RETRIES; tries++) {
		error = rk3588_hdmi_qp_ddc_read(sc, start,
		    DW_HDMI_QP_EDID_BLOCK_SIZE);
		if (error != 0)
			return (error);
		valid = rk3588_hdmi_qp_edid_checksum_valid(sc->edid + start);
		if (valid)
			sc->edid_checksum_mask |= 1u << block;
		else
			sc->edid_checksum_mask &= ~(1u << block);
		if (block == 0)
			sc->edid_header_valid =
			    memcmp(sc->edid, header, sizeof(header)) == 0;
		if (valid && (block != 0 || sc->edid_header_valid))
			return (0);
	}
	return (EIO);
}

static void
rk3588_hdmi_qp_edid_hash(struct rk3588_hdmi_qp_softc *sc)
{
	static const char hex[] = "0123456789abcdef";
	SHA256_CTX ctx;
	uint8_t digest[SHA256_DIGEST_LENGTH];
	int i;

	SHA256_Init(&ctx);
	SHA256_Update(&ctx, sc->edid, sc->edid_len);
	SHA256_Final(digest, &ctx);
	for (i = 0; i < SHA256_DIGEST_LENGTH; i++) {
		sc->edid_sha256[i * 2] = hex[digest[i] >> 4];
		sc->edid_sha256[i * 2 + 1] = hex[digest[i] & 0xf];
	}
	sc->edid_sha256[SHA256_DIGEST_LENGTH * 2] = '\0';
}

static void
rk3588_hdmi_qp_parse_edid(struct rk3588_hdmi_qp_softc *sc)
{
	struct edid_info info;

	bzero(&info, sizeof(info));
	if (edid_parse(sc->edid, &info) != 0) {
		sc->preferred_mode_error = EINVAL;
		return;
	}
	if (info.edid_preferred_mode == NULL) {
		sc->preferred_mode_error = ENOENT;
		return;
	}
	sc->preferred_mode = *info.edid_preferred_mode;
	sc->preferred_mode.name = NULL;
	sc->preferred_mode_valid = 1;
	sc->preferred_mode_error = 0;
	sc->current_mode = sc->preferred_mode;
	sc->current_mode_valid = 1;
	rk3588_hdmi_mode[sc->port] = sc->preferred_mode;
	rk3588_hdmi_mode[sc->port].name = NULL;
	rk3588_hdmi_mode_valid[sc->port] = true;
}

static void
rk3588_hdmi_qp_read_edid(struct rk3588_hdmi_qp_softc *sc)
{
	int error;

	sc->ddc_attempted = 0;
	sc->ddc_fail_offset = -1;
	sc->edid_error = 0;
	sc->edid_len = 0;
	sc->edid_header_valid = 0;
	sc->edid_extension_count = 0;
	sc->edid_checksum_mask = 0;
	sc->preferred_mode_valid = 0;
	sc->preferred_mode_error = ENXIO;
	sc->current_mode_valid = 0;
	bzero(sc->edid_sha256, sizeof(sc->edid_sha256));
	rk3588_hdmi_mode_valid[sc->port] = false;
	sc->ddc_attempted = 1;
	rk3588_hdmi_qp_ddc_init(sc);
	error = rk3588_hdmi_qp_read_edid_block(sc, 0);
	if (error != 0) {
		sc->edid_error = error;
		return;
	}
	sc->edid_len = DW_HDMI_QP_EDID_BLOCK_SIZE;
	sc->edid_extension_count = sc->edid[126];

	if (sc->edid_extension_count > 0) {
		error = rk3588_hdmi_qp_read_edid_block(sc, 1);
		if (error != 0) {
			sc->edid_error = error;
			return;
		}
		sc->edid_len = DW_HDMI_QP_EDID_MAX_SIZE;
	}

	if (!sc->edid_header_valid || (sc->edid_checksum_mask & 1u) == 0 ||
	    (sc->edid_extension_count > 0 &&
	    (sc->edid_checksum_mask & 2u) == 0)) {
		sc->edid_error = EIO;
		return;
	}
	if (sc->edid_extension_count > 1) {
		sc->edid_error = EOVERFLOW;
		return;
	}
	rk3588_hdmi_qp_edid_hash(sc);
	rk3588_hdmi_qp_parse_edid(sc);
}

static void
rk3588_hdmi_qp_io_init(struct rk3588_hdmi_qp_softc *sc)
{
	uint32_t grant_mask, hpd_int_mask, mask, vo_con;

	vo_con = sc->port == 0 ? RK3588_GRF_VO1_CON3 :
	    RK3588_GRF_VO1_CON6;
	grant_mask = sc->port == 0 ? RK3588_HDMI0_GRANT_SEL :
	    RK3588_HDMI1_GRANT_SEL;
	hpd_int_mask = sc->port == 0 ? RK3588_HDMI0_HPD_INT_MSK :
	    RK3588_HDMI1_HPD_INT_MSK;

	sc->soc_con7_before = SYSCON_READ_4(sc->grf, RK3588_GRF_SOC_CON7);
	sc->status1_before = SYSCON_READ_4(sc->grf,
	    RK3588_GRF_SOC_STATUS1);
	mask = RK3588_SCLIN_MASK | RK3588_SDAIN_MASK | RK3588_MODE_MASK |
	    RK3588_I2S_SEL_MASK;
	SYSCON_WRITE_4(sc->vo_grf, vo_con, (mask << 16) | mask);
	sc->vo_con_after = SYSCON_READ_4(sc->vo_grf, vo_con);

	mask = RK3588_HPD_HDMI0_IO_EN | RK3588_HPD_HDMI1_IO_EN;
	SYSCON_WRITE_4(sc->grf, RK3588_GRF_SOC_CON7,
	    (mask << 16) | mask);
	sc->soc_con7_after = SYSCON_READ_4(sc->grf, RK3588_GRF_SOC_CON7);

	SYSCON_WRITE_4(sc->vo_grf, RK3588_GRF_VO1_CON9,
	    (grant_mask << 16) | grant_mask);
	sc->vo_con9_after = SYSCON_READ_4(sc->vo_grf,
	    RK3588_GRF_VO1_CON9);

	SYSCON_WRITE_4(sc->grf, RK3588_GRF_SOC_CON2,
	    (hpd_int_mask << 16) | hpd_int_mask);
	sc->soc_con2_after = SYSCON_READ_4(sc->grf, RK3588_GRF_SOC_CON2);
	sc->status1_after_hpd_io = SYSCON_READ_4(sc->grf,
	    RK3588_GRF_SOC_STATUS1);
}

static void
rk3588_hdmi_qp_hpd_irq_update(struct rk3588_hdmi_qp_softc *sc,
    bool masked, bool clear)
{
	uint32_t clear_mask, mask, value;

	if (sc->port == 0) {
		clear_mask = RK3588_HDMI0_HPD_INT_CLR;
		mask = RK3588_HDMI0_HPD_INT_MSK;
	} else {
		clear_mask = RK3588_HDMI1_HPD_INT_CLR;
		mask = RK3588_HDMI1_HPD_INT_MSK;
	}
	value = mask << 16;
	if (masked)
		value |= mask;
	if (clear)
		value |= (clear_mask << 16) | clear_mask;
	SYSCON_WRITE_4(sc->grf, RK3588_GRF_SOC_CON2, value);
}

static void
rk3588_hdmi_qp_hpd_debounce(void *arg)
{
	struct rk3588_hdmi_qp_softc *sc;
	uint32_t status;
	int connected;

	sc = arg;
	status = SYSCON_READ_4(sc->grf, RK3588_GRF_SOC_STATUS1);
	connected = (status & sc->hpd_mask) != 0;
	sc->hpd_last_status = status;
	if (connected == sc->hpd_state)
		return;
	sc->hpd_state = connected;
	sc->hpd_event_count++;
	if (connected)
		sc->hpd_connect_count++;
	else
		sc->hpd_disconnect_count++;
	device_printf(sc->dev, "HDMI%d %s\n", sc->port,
	    connected ? "connected" : "disconnected");
	if (sc->port == 0) {
		if (!connected)
			sc->output_configured = 0;
		taskqueue_enqueue(taskqueue_bus, &sc->hpd_task);
	}
}

static void
rk3588_hdmi_qp_hpd_intr(void *arg)
{
	struct rk3588_hdmi_qp_softc *sc;

	sc = arg;
	sc->hpd_irq_count++;
	rk3588_hdmi_qp_hpd_irq_update(sc, true, false);
	callout_reset(&sc->hpd_callout,
	    MAX(1, hz * RK3588_HDMI_HPD_DEBOUNCE_MS / 1000),
	    rk3588_hdmi_qp_hpd_debounce, sc);
	rk3588_hdmi_qp_hpd_irq_update(sc, false, true);
}

static int
rk3588_hdmi_qp_hpd_irq_setup(struct rk3588_hdmi_qp_softc *sc)
{
	uint32_t status;
	int error;

	sc->hpd_irq_rid = RK3588_HDMI_HPD_IRQ_RID;
	sc->hpd_irq = bus_alloc_resource_any(sc->dev, SYS_RES_IRQ,
	    &sc->hpd_irq_rid, RF_ACTIVE);
	if (sc->hpd_irq == NULL)
		return (ENXIO);
	callout_init(&sc->hpd_callout, 1);
	status = SYSCON_READ_4(sc->grf, RK3588_GRF_SOC_STATUS1);
	sc->hpd_last_status = status;
	sc->hpd_state = (status & sc->hpd_mask) != 0;
	error = bus_setup_intr(sc->dev, sc->hpd_irq,
	    INTR_TYPE_MISC | INTR_MPSAFE, NULL, rk3588_hdmi_qp_hpd_intr, sc,
	    &sc->hpd_irq_cookie);
	if (error != 0) {
		bus_release_resource(sc->dev, SYS_RES_IRQ, sc->hpd_irq_rid,
		    sc->hpd_irq);
		sc->hpd_irq = NULL;
		return (error);
	}
	/* Linux clears a stale edge before unmasking the dedicated HPD IRQ. */
	rk3588_hdmi_qp_hpd_irq_update(sc, false, true);
	return (0);
}

static int
rk3588_hdmi_qp_hpd_sysctl(SYSCTL_HANDLER_ARGS)
{
	struct rk3588_hdmi_qp_softc *sc;
	uint32_t status;
	int connected;

	sc = arg1;
	status = SYSCON_READ_4(sc->grf, RK3588_GRF_SOC_STATUS1);
	connected = (status & sc->hpd_mask) != 0;
	return (sysctl_handle_int(oidp, &connected, 0, req));
}

static int
rk3588_hdmi_qp_status_sysctl(SYSCTL_HANDLER_ARGS)
{
	struct rk3588_hdmi_qp_softc *sc;
	uint32_t status;

	sc = arg1;
	status = SYSCON_READ_4(sc->grf, RK3588_GRF_SOC_STATUS1);
	return (sysctl_handle_32(oidp, &status, 0, req));
}

static int
rk3588_hdmi_qp_edid_sysctl(SYSCTL_HANDLER_ARGS)
{
	struct rk3588_hdmi_qp_softc *sc;

	sc = arg1;
	return (sysctl_handle_opaque(oidp, sc->edid, sc->edid_len, req));
}

static void
rk3588_hdmi_qp_get_clocks(struct rk3588_hdmi_qp_softc *sc)
{
	clk_t clk;
	uint64_t rate;
	int error, i;

	for (i = 0; i < nitems(rk3588_hdmi_clock_names); i++) {
		error = clk_get_by_ofw_name(sc->dev, 0,
		    rk3588_hdmi_clock_names[i], &clk);
		sc->clock_error[i] = error;
		if (error != 0)
			continue;
		sc->clocks[i] = clk;
		sc->clock_mask |= 1u << i;
		error = clk_enable(clk);
		sc->clock_error[i] = error;
		if (error != 0)
			continue;
		sc->clock_enable_mask |= 1u << i;
		error = clk_get_freq(clk, &rate);
		sc->clock_error[i] = error;
		if (error == 0)
			sc->clock_rate[i] = rate;
	}
}

static void
rk3588_hdmi_qp_get_vop_route(struct rk3588_hdmi_qp_softc *sc,
    phandle_t node)
{
	phandle_t endpoint, port, remote_node, remote_port;
	uint32_t reg;

	sc->vop_route_vp = -1;
	sc->vop_route_endpoint = -1;
	port = ofw_graph_get_port_by_idx(node, 0);
	if (port == 0)
		return;
	endpoint = ofw_graph_get_endpoint_by_idx(port, 0);
	if (endpoint == 0)
		return;
	sc->vop_remote_xref = ofw_graph_get_remote_endpoint(endpoint);
	if (sc->vop_remote_xref == 0)
		return;
	sc->vop_node = ofw_graph_get_remote_parent(sc->vop_remote_xref);
	remote_node = OF_node_from_xref(sc->vop_remote_xref);
	if (remote_node == 0)
		return;
	if (OF_getencprop(remote_node, "reg", &reg, sizeof(reg)) > 0)
		sc->vop_route_endpoint = reg;
	remote_port = OF_parent(remote_node);
	if (OF_getencprop(remote_port, "reg", &reg, sizeof(reg)) > 0)
		sc->vop_route_vp = reg;
}

static const char *
rk3588_hdmi_qp_clock_parent(device_t dev, const char *name)
{
	clk_t clk, parent;
	const char *parent_name;

	if (clk_get_by_name(dev, name, &clk) != 0)
		return ("unresolved");
	if (clk_get_parent(clk, &parent) != 0) {
		clk_release(clk);
		return ("none");
	}
	parent_name = clk_get_name(parent);
	clk_release(parent);
	clk_release(clk);
	return (parent_name);
}

static void
rk3588_hdmi_qp_refresh_vop_clocks(struct rk3588_hdmi_qp_softc *sc)
{
	clk_t clk;
	uint64_t rate;
	int error, i;

	sc->vop_clock_mask = 0;
	sc->vop_clock_rate_mask = 0;
	bzero(sc->vop_clock_rate, sizeof(sc->vop_clock_rate));
	for (i = 0; i < nitems(rk3588_vop_clock_names); i++) {
		error = clk_get_by_ofw_name(sc->dev, sc->vop_node,
		    rk3588_vop_clock_names[i], &clk);
		sc->vop_clock_error[i] = error;
		if (error != 0)
			continue;
		sc->vop_clock_mask |= 1u << i;
		error = clk_get_freq(clk, &rate);
		sc->vop_clock_error[i] = error;
		if (error == 0) {
			sc->vop_clock_rate[i] = rate;
			sc->vop_clock_rate_mask |= 1u << i;
		}
		clk_release(clk);
	}
	sc->vop_probe_done = 1;
}

static int
rk3588_hdmi_qp_selected_output(struct rk3588_hdmi_qp_softc *sc)
{
	uint64_t rate;
	uint32_t avi1, avi2, mask, status;
	int i;

	sc->output_attempted = 1;
	if (!sc->current_mode_valid)
		return (ENXIO);
	rate = (uint64_t)sc->current_mode.dot_clock * 1000;
	if (sc->current_mode.hdisplay == 1920 &&
	    sc->current_mode.vdisplay == 1080 && rate == 148500000 &&
	    (sc->current_mode.flags & VID_INTERLACE) == 0) {
		/* Linux's RGB8 AVI frame with CEA VIC 16. */
		avi1 = 0x08000255;
		avi2 = 0x00000010;
	} else if (sc->current_mode.hdisplay == 3840 &&
	    sc->current_mode.vdisplay == 2160 && rate == 297000000 &&
	    (sc->current_mode.flags & VID_INTERLACE) == 0) {
		/* Linux's packed AVI frame for the EDID-selected 4K DTD. */
		avi1 = 0x04281231;
		avi2 = 0;
	} else if (sc->current_mode.hdisplay == 1920 &&
	    sc->current_mode.vdisplay == 1200 && rate == 154000000 &&
	    (sc->current_mode.flags & VID_INTERLACE) == 0) {
		avi1 = 0x08000265;
		avi2 = 0;
	} else {
		return (ERANGE);
	}
	status = SYSCON_READ_4(sc->grf, RK3588_GRF_SOC_STATUS1);
	if (sc->port != 0 || (status & sc->hpd_mask) == 0 ||
	    sc->vop_route_vp != 0 || sc->vop_route_endpoint != 2 ||
	    sc->clock_mask != 0x37 || sc->clock_enable_mask != 0x37 ||
	    sc->vop_clock_mask != 0x1ff ||
	    (sc->vop_clock_rate_mask & RK3588_VOP_HDMI0_RATE_MASK) !=
	    RK3588_VOP_HDMI0_RATE_MASK ||
	    sc->vop_clock_rate[0] != 500000000 ||
	    sc->vop_clock_rate[1] != 198000000 ||
	    (sc->vop_clock_rate[2] != 594000000 &&
	    sc->vop_clock_rate[2] != rate) ||
	    sc->vop_clock_rate[6] != 100000000 ||
	    sc->vop_clock_rate[7] != rate ||
	    sc->core_id != DW_HDMI_QP_EXPECTED_CORE_ID ||
	    sc->ver_number != DW_HDMI_QP_EXPECTED_VERSION ||
	    sc->ver_type != DW_HDMI_QP_EXPECTED_TYPE ||
	    sc->config_reg != DW_HDMI_QP_EXPECTED_CONFIG ||
	    sc->edid_error != 0 || sc->edid_len != 256 ||
	    sc->edid_checksum_mask != 3)
		return (ENXIO);

	/* RGB 8-bpc is the zero encoding in VO1_GRF_CON3. */
	SYSCON_WRITE_4(sc->vo_grf, RK3588_GRF_VO1_CON3,
	    RK3588_HDMI_RGB8_MASK << 16);
	sc->output_color_after = SYSCON_READ_4(sc->vo_grf,
	    RK3588_GRF_VO1_CON3);
	if ((sc->output_color_after & RK3588_HDMI_RGB8_MASK) != 0)
		return (EIO);

	sc->output_link_before = bus_read_4(sc->mem,
	    DW_HDMI_QP_LINK_CONFIG0);
	sc->output_hdcp_before = bus_read_4(sc->mem,
	    DW_HDMI_QP_HDCP2LOGIC_CONFIG0);
	sc->output_pkt_before = bus_read_4(sc->mem,
	    DW_HDMI_QP_PKTSCHED_PKT_EN);

	/* Linux 6.18 atomic_enable plus the AVI frame for the selected mode. */
	rk3588_hdmi_qp_mod(sc, DW_HDMI_QP_HDCP2LOGIC_CONFIG0,
	    DW_HDMI_QP_HDCP2_BYPASS, DW_HDMI_QP_HDCP2_BYPASS);
	rk3588_hdmi_qp_mod(sc, DW_HDMI_QP_LINK_CONFIG0, 0,
	    DW_HDMI_QP_OPMODE_DVI);
	bus_write_4(sc->mem, DW_HDMI_QP_SCRAMB_CONFIG0, 0);
	bus_write_4(sc->mem, DW_HDMI_QP_PKT_AVI_CONTENTS0, 0x000d0200);
	bus_write_4(sc->mem, DW_HDMI_QP_PKT_AVI_CONTENTS1, avi1);
	bus_write_4(sc->mem, DW_HDMI_QP_PKT_AVI_CONTENTS2, avi2);
	bus_write_4(sc->mem, DW_HDMI_QP_PKT_AVI_CONTENTS3, 0);
	bus_write_4(sc->mem, DW_HDMI_QP_PKT_AVI_CONTENTS4, 0);
	rk3588_hdmi_qp_mod(sc, DW_HDMI_QP_PKTSCHED_PKT_CONFIG1, 0,
	    DW_HDMI_QP_AVI_FIELDRATE);
	mask = DW_HDMI_QP_AVI_TX_EN | DW_HDMI_QP_GCP_TX_EN;
	rk3588_hdmi_qp_mod(sc, DW_HDMI_QP_PKTSCHED_PKT_EN, mask, mask);
	bus_barrier(sc->mem, 0, rman_get_size(sc->mem),
	    BUS_SPACE_BARRIER_READ | BUS_SPACE_BARRIER_WRITE);

	sc->output_link_after = bus_read_4(sc->mem,
	    DW_HDMI_QP_LINK_CONFIG0);
	sc->output_hdcp_after = bus_read_4(sc->mem,
	    DW_HDMI_QP_HDCP2LOGIC_CONFIG0);
	sc->output_pkt_after = bus_read_4(sc->mem,
	    DW_HDMI_QP_PKTSCHED_PKT_EN);
	sc->output_avi0_after = bus_read_4(sc->mem,
	    DW_HDMI_QP_PKT_AVI_CONTENTS0);
	sc->output_avi1_after = bus_read_4(sc->mem,
	    DW_HDMI_QP_PKT_AVI_CONTENTS1);
	sc->output_avi2_after = bus_read_4(sc->mem,
	    DW_HDMI_QP_PKT_AVI_CONTENTS2);
	for (i = 0; i < nitems(sc->output_video_status); i++)
		sc->output_video_status[i] = bus_read_4(sc->mem,
		    DW_HDMI_QP_VIDEO_MONITOR_STATUS0 + i * 4);

	if ((sc->output_link_after & DW_HDMI_QP_OPMODE_DVI) != 0 ||
	    (sc->output_hdcp_after & DW_HDMI_QP_HDCP2_BYPASS) == 0 ||
	    (sc->output_pkt_after & mask) != mask ||
	    sc->output_avi0_after != 0x000d0200 ||
	    sc->output_avi1_after != avi1 || sc->output_avi2_after != avi2)
		return (EIO);
	return (0);
}

static bool
rk3588_hdmi_qp_mode_equal(const struct videomode *a,
    const struct videomode *b)
{

	return (a->dot_clock == b->dot_clock &&
	    a->hdisplay == b->hdisplay &&
	    a->hsync_start == b->hsync_start &&
	    a->hsync_end == b->hsync_end &&
	    a->htotal == b->htotal &&
	    a->vdisplay == b->vdisplay &&
	    a->vsync_start == b->vsync_start &&
	    a->vsync_end == b->vsync_end &&
	    a->vtotal == b->vtotal &&
	    a->flags == b->flags);
}

static int
rk3588_hdmi_qp_apply_mode(struct rk3588_hdmi_qp_softc *sc,
    const struct videomode *mode, const char **failed_stage)
{
	int error;

	if (failed_stage != NULL)
		*failed_stage = "prepare";
	sc->current_mode = *mode;
	sc->current_mode.name = NULL;
	sc->current_mode_valid = 1;
	rk3588_hdmi_mode[sc->port] = sc->current_mode;
	rk3588_hdmi_mode_valid[sc->port] = true;

	error = rk3588_vop2_prepare_mode(sc->port);
	if (error == 0) {
		if (failed_stage != NULL)
			*failed_stage = "hdptx";
		error = rk3588_hdptx_configure_mode(sc->port,
		    &sc->current_mode);
	}
	if (error == 0) {
		if (failed_stage != NULL)
			*failed_stage = "vop2";
		error = rk3588_vop2_configure_mode(sc->port);
	}
	if (error == 0) {
		if (failed_stage != NULL)
			*failed_stage = "hdmi-qp";
		rk3588_hdmi_qp_refresh_vop_clocks(sc);
		error = rk3588_hdmi_qp_selected_output(sc);
	}
	sc->output_error = error;
	sc->output_configured = error == 0;
	return (error);
}

int
rk3588_hdmi_qp_set_mode(int port, const struct videomode *mode)
{
	struct rk3588_hdmi_qp_softc *sc;
	struct videomode old_mode;
	const char *failed_stage, *rollback_stage;
	uint32_t status;
	bool old_valid;
	int error, rollback_error;

	if (port < 0 || port >= (int)nitems(rk3588_hdmi_qp_sc) ||
	    mode == NULL)
		return (EINVAL);
	sc = rk3588_hdmi_qp_sc[port];
	if (sc == NULL)
		return (ENXIO);
	status = SYSCON_READ_4(sc->grf, RK3588_GRF_SOC_STATUS1);
	if ((status & sc->hpd_mask) == 0)
		return (ENXIO);
	if (sc->output_configured && sc->current_mode_valid &&
	    rk3588_hdmi_qp_mode_equal(&sc->current_mode, mode))
		return (0);

	old_mode = sc->current_mode;
	old_valid = sc->current_mode_valid;
	failed_stage = rollback_stage = "none";
	rollback_error = 0;
	error = rk3588_hdmi_qp_apply_mode(sc, mode, &failed_stage);
	if (error != 0 && old_valid)
		rollback_error = rk3588_hdmi_qp_apply_mode(sc, &old_mode,
		    &rollback_stage);
	if (error != 0 && !old_valid) {
		sc->current_mode_valid = 0;
		rk3588_hdmi_mode_valid[port] = false;
	}
	if (error != 0)
		device_printf(sc->dev,
		    "DRM modeset %dx%d@%d failed at %s: %d; "
		    "rollback=%s/%d\n", mode->hdisplay, mode->vdisplay,
		    mode->dot_clock, failed_stage, error,
		    old_valid ? rollback_stage : "not-available",
		    rollback_error);
	return (error);
}

int
rk3588_hdmi_qp_prepare_output(int port)
{
	struct rk3588_hdmi_qp_softc *sc;
	int error;

	if (port < 0 || port >= (int)nitems(rk3588_hdmi_qp_sc))
		return (EINVAL);
	sc = rk3588_hdmi_qp_sc[port];
	if (sc == NULL)
		return (ENXIO);
	if (sc->output_configured)
		return (0);
	if (!sc->current_mode_valid)
		return (ENXIO);
	error = rk3588_hdptx_configure_mode(port, &sc->current_mode);
	sc->output_error = error;
	return (error);
}

int
rk3588_hdmi_qp_set_output(int port, bool enable)
{
	struct rk3588_hdmi_qp_softc *sc;
	int error;

	if (port < 0 || port >= (int)nitems(rk3588_hdmi_qp_sc))
		return (EINVAL);
	sc = rk3588_hdmi_qp_sc[port];
	if (sc == NULL)
		return (ENXIO);
	if (enable) {
		if (sc->output_configured)
			return (0);
		rk3588_hdmi_qp_refresh_vop_clocks(sc);
		error = rk3588_hdmi_qp_selected_output(sc);
		sc->output_error = error;
		sc->output_configured = error == 0;
		return (error);
	}
	error = rk3588_hdptx_disable(port);
	sc->output_error = error;
	if (error == 0)
		sc->output_configured = 0;
	return (error);
}

static void
rk3588_hdmi_qp_late_connect(void *arg, int pending)
{
	struct rk3588_hdmi_qp_softc *sc;
	uint32_t status;
	int error;

	(void)pending;
	sc = arg;
	status = SYSCON_READ_4(sc->grf, RK3588_GRF_SOC_STATUS1);
	if ((status & sc->hpd_mask) == 0 || sc->output_configured) {
		rk3588_vop2_hpd_changed();
		return;
	}

	sc->late_attempted = 1;
	rk3588_hdmi_qp_read_edid(sc);
	error = sc->edid_error;
	if (error == 0 && !sc->preferred_mode_valid)
		error = sc->preferred_mode_error;
	if (error == 0)
		error = rk3588_hdmi_qp_apply_mode(sc, &sc->preferred_mode,
		    NULL);
	sc->late_error = error;
	sc->late_configured = error == 0;
	if (error != 0)
		device_printf(sc->dev, "late connect failed: %d\n", error);
	rk3588_vop2_hpd_changed();
}

static void
rk3588_hdmi_qp_vop_probe(void *arg)
{
	struct rk3588_hdmi_qp_softc *sc;

	sc = arg;
	config_intrhook_disestablish(&sc->vop_hook);
	if (sc->vop_node == 0) {
		device_printf(sc->dev, "VOP route unresolved\n");
		return;
	}

	rk3588_hdmi_qp_refresh_vop_clocks(sc);

	if (bootverbose)
		device_printf(sc->dev,
	    "RK3588-DIAG V2238 port=%d route=vp%d/ep%d remote=%#x "
	    "vop_node=%#x clocks/rates=%#x/%#x "
	    "errors=%d/%d/%d/%d/%d/%d/%d/%d/%d\n",
	    sc->port, sc->vop_route_vp, sc->vop_route_endpoint,
	    sc->vop_remote_xref, sc->vop_node, sc->vop_clock_mask,
	    sc->vop_clock_rate_mask, sc->vop_clock_error[0],
	    sc->vop_clock_error[1], sc->vop_clock_error[2],
	    sc->vop_clock_error[3], sc->vop_clock_error[4],
	    sc->vop_clock_error[5], sc->vop_clock_error[6],
	    sc->vop_clock_error[7], sc->vop_clock_error[8]);
	if (bootverbose)
		device_printf(sc->dev,
	    "RK3588-DIAG V2238 port=%d rates=%ju/%ju/%ju/%ju/%ju/%ju/"
	    "%ju/%ju/%ju parents=aclk:%s dclk0:%s write=%s\n",
	    sc->port, (uintmax_t)sc->vop_clock_rate[0],
	    (uintmax_t)sc->vop_clock_rate[1],
	    (uintmax_t)sc->vop_clock_rate[2],
	    (uintmax_t)sc->vop_clock_rate[3],
	    (uintmax_t)sc->vop_clock_rate[4],
	    (uintmax_t)sc->vop_clock_rate[5],
	    (uintmax_t)sc->vop_clock_rate[6],
	    (uintmax_t)sc->vop_clock_rate[7],
	    (uintmax_t)sc->vop_clock_rate[8],
	    rk3588_hdmi_qp_clock_parent(sc->dev, "aclk_vop_sub_src"),
	    rk3588_hdmi_qp_clock_parent(sc->dev, "dclk_vop0_sel"),
	    sc->port == 0 ? "selected-output" : "none");

	if (sc->port == 0) {
		sc->output_error = rk3588_hdmi_qp_selected_output(sc);
		sc->output_configured = sc->output_error == 0;
		if (bootverbose || sc->output_error != 0)
			device_printf(sc->dev,
		    "RK3588-DIAG V2272 QP selected=%dx%d clock=%d RGB8 "
		    "attempted=%d configured=%d error=%d color=%#x "
		    "link=%#x->%#x hdcp=%#x->%#x pkt=%#x->%#x "
		    "avi=%#x/%#x/%#x video=%#x/%#x/%#x/%#x\n",
		    sc->preferred_mode.hdisplay, sc->preferred_mode.vdisplay,
		    sc->preferred_mode.dot_clock,
		    sc->output_attempted, sc->output_configured,
		    sc->output_error, sc->output_color_after,
		    sc->output_link_before, sc->output_link_after,
		    sc->output_hdcp_before, sc->output_hdcp_after,
		    sc->output_pkt_before, sc->output_pkt_after,
		    sc->output_avi0_after, sc->output_avi1_after,
		    sc->output_avi2_after,
		    sc->output_video_status[0], sc->output_video_status[1],
		    sc->output_video_status[2], sc->output_video_status[3]);
	}
}

static void
rk3588_hdmi_qp_add_sysctls(struct rk3588_hdmi_qp_softc *sc)
{
	struct sysctl_ctx_list *ctx;
	struct sysctl_oid_list *children;
	int i;

	ctx = device_get_sysctl_ctx(sc->dev);
	children = SYSCTL_CHILDREN(device_get_sysctl_tree(sc->dev));

	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "port", CTLFLAG_RD,
	    &sc->port, 0, "RK3588 HDMI port index");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "phy_xref", CTLFLAG_RD,
	    &sc->phy_xref, 0, "HDPTX PHY OFW xref");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "clock_mask", CTLFLAG_RD,
	    &sc->clock_mask, 0, "Resolved DT clocks in clock-names order");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "clock_enable_mask",
	    CTLFLAG_RD, &sc->clock_enable_mask, 0,
	    "Enabled DT clocks in clock-names order");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "vop_probe_done", CTLFLAG_RD,
	    &sc->vop_probe_done, 0, "Deferred read-only VOP probe completed");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "vop_route_vp", CTLFLAG_RD,
	    &sc->vop_route_vp, 0, "VOP video port routed to this HDMI input");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "vop_route_endpoint",
	    CTLFLAG_RD, &sc->vop_route_endpoint, 0,
	    "VOP endpoint routed to this HDMI input");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "vop_remote_xref", CTLFLAG_RD,
	    &sc->vop_remote_xref, 0, "Remote VOP endpoint OFW xref");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "vop_clock_mask", CTLFLAG_RD,
	    &sc->vop_clock_mask, 0, "Resolved VOP clocks in clock-names order");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "vop_clock_rate_mask",
	    CTLFLAG_RD, &sc->vop_clock_rate_mask, 0,
	    "VOP clocks with readable rates in clock-names order");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "output_attempted", CTLFLAG_RD,
	    &sc->output_attempted, 0, "Selected HDMI output programming attempted");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "output_configured", CTLFLAG_RD,
	    &sc->output_configured, 0,
	    "Selected HDMI output register verification passed");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "output_error", CTLFLAG_RD,
	    &sc->output_error, 0, "Selected HDMI output programming error");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "output_link", CTLFLAG_RD,
	    &sc->output_link_after, 0, "HDMI-QP link configuration after setup");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "output_packet_enable",
	    CTLFLAG_RD, &sc->output_pkt_after, 0,
	    "HDMI-QP packet enables after setup");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "output_video_status0",
	    CTLFLAG_RD, &sc->output_video_status[0], 0,
	    "HDMI-QP video monitor status 0 after setup");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "soc_con7_before",
	    CTLFLAG_RD, &sc->soc_con7_before, 0,
	    "GRF SOC_CON7 before enabling HDMI HPD inputs");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "soc_con7_after",
	    CTLFLAG_RD, &sc->soc_con7_after, 0,
	    "GRF SOC_CON7 after enabling HDMI HPD inputs");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "status1_before",
	    CTLFLAG_RD, &sc->status1_before, 0,
	    "GRF SOC_STATUS1 before enabling HDMI HPD inputs");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "status1_after_hpd_io",
	    CTLFLAG_RD, &sc->status1_after_hpd_io, 0,
	    "GRF SOC_STATUS1 after enabling HDMI HPD inputs");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "vo_con_after",
	    CTLFLAG_RD, &sc->vo_con_after, 0,
	    "VO1 GRF per-port control after HDMI I/O setup");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "vo_con9_after",
	    CTLFLAG_RD, &sc->vo_con9_after, 0,
	    "VO1 GRF CON9 after HDMI grant setup");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "soc_con2_after",
	    CTLFLAG_RD, &sc->soc_con2_after, 0,
	    "GRF SOC_CON2 after masking HDMI HPD interrupt");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "core_id", CTLFLAG_RD,
	    &sc->core_id, 0, "Synopsys HDMI-QP core ID");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "version", CTLFLAG_RD,
	    &sc->ver_number, 0, "Synopsys HDMI-QP version number");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "version_type", CTLFLAG_RD,
	    &sc->ver_type, 0, "Synopsys HDMI-QP version type");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "config", CTLFLAG_RD,
	    &sc->config_reg, 0, "Synopsys HDMI-QP configuration");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "ddc_attempted", CTLFLAG_RD,
	    &sc->ddc_attempted, 0, "Whether EDID DDC was attempted");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "ddc_fail_offset", CTLFLAG_RD,
	    &sc->ddc_fail_offset, 0, "EDID offset of the first DDC failure");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "edid_error", CTLFLAG_RD,
	    &sc->edid_error, 0, "EDID read or validation error");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "edid_length", CTLFLAG_RD,
	    &sc->edid_len, 0, "Number of captured EDID bytes");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "edid_header_valid", CTLFLAG_RD,
	    &sc->edid_header_valid, 0, "EDID header validation result");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "edid_extensions", CTLFLAG_RD,
	    &sc->edid_extension_count, 0, "EDID extension block count");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "edid_checksum_mask",
	    CTLFLAG_RD, &sc->edid_checksum_mask, 0,
	    "Valid checksum bits for captured EDID blocks");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "ddc_last_status", CTLFLAG_RD,
	    &sc->ddc_last_status, 0, "Last HDMI-QP I2CM interrupt status");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "ddc_done_count", CTLFLAG_RD,
	    &sc->ddc_done_count, 0, "Completed DDC byte operations");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "ddc_nack_count", CTLFLAG_RD,
	    &sc->ddc_nack_count, 0, "DDC NACK count");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "ddc_timeout_count", CTLFLAG_RD,
	    &sc->ddc_timeout_count, 0, "DDC timeout count");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "preferred_mode_valid",
	    CTLFLAG_RD, &sc->preferred_mode_valid, 0,
	    "EDID preferred timing was parsed");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "preferred_mode_error",
	    CTLFLAG_RD, &sc->preferred_mode_error, 0,
	    "EDID preferred timing parse error");
	SYSCTL_ADD_STRING(ctx, children, OID_AUTO, "edid_sha256", CTLFLAG_RD,
	    sc->edid_sha256, sizeof(sc->edid_sha256),
	    "SHA256 of the captured EDID bytes");
	SYSCTL_ADD_PROC(ctx, children, OID_AUTO, "edid",
	    CTLTYPE_OPAQUE | CTLFLAG_RD, sc, 0, rk3588_hdmi_qp_edid_sysctl,
	    "S,edid", "Raw captured EDID bytes; use sysctl -b");
	SYSCTL_ADD_PROC(ctx, children, OID_AUTO, "hpd",
	    CTLTYPE_INT | CTLFLAG_RD, sc, 0, rk3588_hdmi_qp_hpd_sysctl,
	    "I", "Live HDMI hot-plug detect state");
	SYSCTL_ADD_PROC(ctx, children, OID_AUTO, "grf_status1",
	    CTLTYPE_UINT | CTLFLAG_RD, sc, 0, rk3588_hdmi_qp_status_sysctl,
	    "IU", "Live RK3588 GRF SOC_STATUS1 value");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "hpd_state", CTLFLAG_RD,
	    &sc->hpd_state, 0, "Debounced HDMI hot-plug state");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "hpd_irq_error", CTLFLAG_RD,
	    &sc->hpd_irq_error, 0, "HPD interrupt setup error");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "hpd_irq_count", CTLFLAG_RD,
	    &sc->hpd_irq_count, 0, "HPD interrupt count");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "hpd_event_count", CTLFLAG_RD,
	    &sc->hpd_event_count, 0, "Debounced HPD transition count");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "hpd_connect_count", CTLFLAG_RD,
	    &sc->hpd_connect_count, 0, "Debounced HPD connect count");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "hpd_disconnect_count",
	    CTLFLAG_RD, &sc->hpd_disconnect_count, 0,
	    "Debounced HPD disconnect count");
	SYSCTL_ADD_U32(ctx, children, OID_AUTO, "hpd_last_status", CTLFLAG_RD,
	    &sc->hpd_last_status, 0, "GRF status at the last HPD observation");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "late_attempted", CTLFLAG_RD,
	    &sc->late_attempted, 0, "Late-connect output setup attempted");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "late_configured", CTLFLAG_RD,
	    &sc->late_configured, 0, "Late-connect output setup completed");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "late_error", CTLFLAG_RD,
	    &sc->late_error, 0, "Late-connect output setup error");

	for (i = 0; i < nitems(rk3588_hdmi_clock_names); i++)
		SYSCTL_ADD_U64(ctx, children, OID_AUTO,
		    rk3588_hdmi_clock_names[i], CTLFLAG_RD,
		    &sc->clock_rate[i], 0, "Resolved clock frequency in Hz");
	for (i = 0; i < nitems(rk3588_vop_clock_names); i++)
		SYSCTL_ADD_U64(ctx, children, OID_AUTO,
		    rk3588_vop_clock_sysctl_names[i], CTLFLAG_RD,
		    &sc->vop_clock_rate[i], 0,
		    "Read-only VOP clock frequency in Hz");
}

static int
rk3588_hdmi_qp_probe(device_t dev)
{

	if (!ofw_bus_status_okay(dev))
		return (ENXIO);
	if (!ofw_bus_is_compatible(dev, "rockchip,rk3588-dw-hdmi-qp"))
		return (ENXIO);

	device_set_desc(dev, "Rockchip RK3588 DesignWare HDMI QP prerequisites");
	return (BUS_PROBE_DEFAULT);
}

static int
rk3588_hdmi_qp_attach(device_t dev)
{
	struct rk3588_hdmi_qp_softc *sc;
	phandle_t node;
	rman_res_t size;
	pcell_t phy;
	uint32_t status;
	int error, rid;

	sc = device_get_softc(dev);
	sc->dev = dev;
	node = ofw_bus_get_node(dev);

	error = bus_get_resource(dev, SYS_RES_MEMORY, 0, &sc->base, &size);
	if (error != 0)
		return (error);
	if (sc->base == RK3588_HDMI0_BASE) {
		sc->port = 0;
		sc->hpd_mask = RK3588_HDMI0_LEVEL_INT;
	} else if (sc->base == RK3588_HDMI1_BASE) {
		sc->port = 1;
		sc->hpd_mask = RK3588_HDMI1_LEVEL_INT;
	} else {
		device_printf(dev, "Unsupported register base %#jx\n",
		    (uintmax_t)sc->base);
		return (ENXIO);
	}

	error = syscon_get_by_ofw_property(dev, node, "rockchip,grf",
	    &sc->grf);
	if (error != 0) {
		device_printf(dev, "Cannot get GRF syscon: %d\n", error);
		return (error);
	}
	error = syscon_get_by_ofw_property(dev, node, "rockchip,vo-grf",
	    &sc->vo_grf);
	if (error != 0) {
		device_printf(dev, "Cannot get VO GRF syscon: %d\n", error);
		return (error);
	}

	if (OF_getencprop(node, "phys", &phy, sizeof(phy)) == sizeof(phy))
		sc->phy_xref = phy;
	rk3588_hdmi_qp_get_vop_route(sc, node);
	rk3588_hdmi_qp_get_clocks(sc);
	rk3588_hdmi_qp_io_init(sc);
	status = sc->status1_after_hpd_io;

	/* Allocate MMIO only after all prerequisite providers have resolved. */
	rid = 0;
	sc->mem = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid, RF_ACTIVE);
	if (sc->mem == NULL) {
		device_printf(dev, "Cannot allocate HDMI-QP MMIO resource\n");
		return (ENXIO);
	}
	sc->core_id = bus_read_4(sc->mem, DW_HDMI_QP_CORE_ID);
	sc->ver_number = bus_read_4(sc->mem, DW_HDMI_QP_VER_NUMBER);
	sc->ver_type = bus_read_4(sc->mem, DW_HDMI_QP_VER_TYPE);
	sc->config_reg = bus_read_4(sc->mem, DW_HDMI_QP_CONFIG_REG);
	sc->ddc_fail_offset = -1;
	sc->preferred_mode_error = ENXIO;
	rk3588_hdmi_mode_valid[sc->port] = false;
	if ((status & sc->hpd_mask) != 0)
		rk3588_hdmi_qp_read_edid(sc);
	TASK_INIT(&sc->hpd_task, 0, rk3588_hdmi_qp_late_connect, sc);
	sc->hpd_irq_error = rk3588_hdmi_qp_hpd_irq_setup(sc);

	if (bootverbose)
		device_printf(dev,
	    "RK3588-DIAG V2228 port=%d base=%#jx hpd=%u->%u "
	    "status1=%#x->%#x io=%#x/%#x/%#x/%#x phy_xref=%#x "
	    "clocks=%#x enabled=%#x errors=%d/%d/%d/%d/%d/%d "
	    "rates=%ju/%ju/%ju/%ju/%ju/%ju\n",
	    sc->port, (uintmax_t)sc->base,
	    (sc->status1_before & sc->hpd_mask) != 0,
	    (status & sc->hpd_mask) != 0, sc->status1_before, status,
	    sc->vo_con_after, sc->soc_con7_after, sc->vo_con9_after,
	    sc->soc_con2_after, sc->phy_xref, sc->clock_mask,
	    sc->clock_enable_mask, sc->clock_error[0], sc->clock_error[1],
	    sc->clock_error[2], sc->clock_error[3], sc->clock_error[4],
	    sc->clock_error[5], (uintmax_t)sc->clock_rate[0],
	    (uintmax_t)sc->clock_rate[1], (uintmax_t)sc->clock_rate[2],
	    (uintmax_t)sc->clock_rate[3], (uintmax_t)sc->clock_rate[4],
	    (uintmax_t)sc->clock_rate[5]);
	if (sc->ddc_attempted != 0 &&
	    (bootverbose || sc->edid_error != 0)) {
		device_printf(dev,
		    "RK3588-DIAG V2231 port=%d core=%#x/%#x/%#x/%#x "
		    "ddc_error=%d fail_offset=%d status=%#x done/nack/timeout="
		    "%u/%u/%u edid_len=%d header=%d ext=%d checksums=%#x "
		    "sha256=%s\n",
		    sc->port, sc->core_id, sc->ver_number, sc->ver_type,
		    sc->config_reg, sc->edid_error, sc->ddc_fail_offset,
		    sc->ddc_last_status, sc->ddc_done_count,
		    sc->ddc_nack_count, sc->ddc_timeout_count, sc->edid_len,
		    sc->edid_header_valid, sc->edid_extension_count,
		    sc->edid_checksum_mask, sc->edid_sha256);
	} else if (bootverbose) {
		device_printf(dev,
		    "RK3588-DIAG V2231 port=%d core=%#x/%#x/%#x/%#x "
		    "ddc=skipped hpd=0\n",
		    sc->port, sc->core_id, sc->ver_number, sc->ver_type,
		    sc->config_reg);
	}
	if (bootverbose ||
	    ((status & sc->hpd_mask) != 0 &&
	    sc->preferred_mode_error != 0))
		device_printf(dev,
	    "RK3588-DIAG V2272 port=%d preferred=%d error=%d "
	    "mode=%dx%d clock=%d h=%d/%d/%d/%d v=%d/%d/%d/%d flags=%#x\n",
	    sc->port, sc->preferred_mode_valid, sc->preferred_mode_error,
	    sc->preferred_mode.hdisplay, sc->preferred_mode.vdisplay,
	    sc->preferred_mode.dot_clock, sc->preferred_mode.hdisplay,
	    sc->preferred_mode.hsync_start, sc->preferred_mode.hsync_end,
	    sc->preferred_mode.htotal, sc->preferred_mode.vdisplay,
	    sc->preferred_mode.vsync_start, sc->preferred_mode.vsync_end,
	    sc->preferred_mode.vtotal, sc->preferred_mode.flags);
	if (bootverbose || sc->hpd_irq_error != 0)
		device_printf(dev,
	    "RK3588-DIAG V2276 HPD port=%d rid=%d irq=%ju setup_error=%d "
	    "connected=%d debounce_ms=%d\n", sc->port, sc->hpd_irq_rid,
	    sc->hpd_irq != NULL ? (uintmax_t)rman_get_start(sc->hpd_irq) : 0,
	    sc->hpd_irq_error, sc->hpd_state,
	    RK3588_HDMI_HPD_DEBOUNCE_MS);

	rk3588_hdmi_qp_add_sysctls(sc);
	sc->vop_hook.ich_func = rk3588_hdmi_qp_vop_probe;
	sc->vop_hook.ich_arg = sc;
	error = config_intrhook_establish(&sc->vop_hook);
	if (error != 0)
		device_printf(dev,
		    "Cannot establish deferred VOP observer: %d\n", error);
	rk3588_hdmi_qp_sc[sc->port] = sc;
	return (0);
}

static device_method_t rk3588_hdmi_qp_methods[] = {
	DEVMETHOD(device_probe,		rk3588_hdmi_qp_probe),
	DEVMETHOD(device_attach,	rk3588_hdmi_qp_attach),

	DEVMETHOD_END
};

static driver_t rk3588_hdmi_qp_driver = {
	"rk3588_hdmi_qp",
	rk3588_hdmi_qp_methods,
	sizeof(struct rk3588_hdmi_qp_softc),
};

EARLY_DRIVER_MODULE(rk3588_hdmi_qp, simplebus, rk3588_hdmi_qp_driver, 0, 0,
    BUS_PASS_DEFAULT);
MODULE_VERSION(rk3588_hdmi_qp, 1);
