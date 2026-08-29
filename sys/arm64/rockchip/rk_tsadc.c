/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2019 Michal Meloun <mmel@FreeBSD.org>
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

#include <sys/cdefs.h>
/*
 * Thermometer and thermal zones driver for RockChip SoCs.
 * Calibration data are taken from Linux, because this part of SoC
 * is undocumented in TRM.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/callout.h>
#include <sys/cpu.h>
#include <sys/gpio.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/malloc.h>
#include <sys/rman.h>
#include <sys/sysctl.h>
#include <sys/taskqueue.h>

#include <machine/bus.h>

#include <dev/extres/clk/clk.h>
#include <dev/extres/hwreset/hwreset.h>
#include <dev/extres/syscon/syscon.h>
#include <dev/fdt/simplebus.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include "syscon_if.h"
#include "cpufreq_if.h"
#include "rk_tsadc_if.h"

/* Version of HW */
#define	TSADC_V2				1
#define	TSADC_V3				2
#define	TSADC_V7				3
#define	TSADC_V8				4

/* Global registers */
#define	TSADC_USER_CON				0x000
#define	TSADC_AUTO_CON				0x004
#define	 TSADC_AUTO_CON_POL_HI				(1 << 8)
#define	 TSADC_AUTO_SRC_EN(x)				(1 << (4 + (x)))
#define	 TSADC_AUTO_Q_SEL				(1 << 1) /* V3 only */
#define	 TSADC_AUTO_CON_AUTO				(1 << 0)

#define	TSADC_INT_EN				0x008
#define	 TSADC_INT_EN_2CRU_EN_SRC(x)			(1 << (8 + (x)))
#define	 TSADC_INT_EN_2GPIO_EN_SRC(x)			(1 << (4 + (x)))
#define	TSADC_INT_PD				0x00c
#define	TSADC_V3_AUTO_SRC_CON			0x00c
#define	TSADC_V3_HT_INT_EN			0x014
#define	TSADC_V3_HSHUT_GPIO_INT_EN		0x018
#define	TSADC_V3_HSHUT_CRU_INT_EN		0x01c
#define	TSADC_V3_INT_PD			0x024
#define	TSADC_V3_HSHUT_PD			0x028
#define	TSADC_DATA(x)				(0x20 + (x) * 0x04)
#define	TSADC_V3_DATA(x)			(0x2c + (x) * 0x04)
#define	TSADC_COMP_INT(x)			(0x30 + (x) * 0x04)
#define	 TSADC_COMP_INT_SRC_EN(x)			(1 << (0 + (x)))
#define	TSADC_COMP_SHUT(x)			(0x40 + (x) * 0x04)
#define	TSADC_V3_COMP_INT(x)			(0x6c + (x) * 0x04)
#define	TSADC_V3_COMP_SHUT(x)			(0x10c + (x) * 0x04)
#define	TSADC_HIGHT_INT_DEBOUNCE		0x060
#define	TSADC_HIGHT_TSHUT_DEBOUNCE		0x064
#define	TSADC_AUTO_PERIOD			0x068
#define	TSADC_AUTO_PERIOD_HT			0x06c
#define	TSADC_V3_HIGHT_INT_DEBOUNCE		0x14c
#define	TSADC_V3_HIGHT_TSHUT_DEBOUNCE		0x150
#define	TSADC_V3_AUTO_PERIOD			0x154
#define	TSADC_V3_AUTO_PERIOD_HT		0x158
#define	TSADC_COMP0_LOW_INT			0x080	/* V3 only */
#define	TSADC_COMP1_LOW_INT			0x084	/* V3 only */

#define	TSADC_AUTO_CON_AUTO_MASK		(1 << 16)
#define	TSADC_AUTO_CON_POL_HI_MASK		(1 << 24)
#define	TSADC_V3_AUTO_SRC_EN(x)		(1 << (x))
#define	TSADC_V3_AUTO_SRC_EN_MASK(x)		(1 << (16 + (x)))
#define	TSADC_V3_INT_SRC_EN(x)			(1 << (x))
#define	TSADC_V3_INT_SRC_EN_MASK(x)		(1 << (16 + (x)))
#define	TSADC_V4_INT_PD_CLEAR_MASK		0xffffffff
#define	TSADC_V4_DATA_MASK			0x1ff

#define	TSADC_MAX_THERMAL_ZONES		7
#define	TSADC_MAX_COOLING_CPUS		8
#define	TSADC_MAX_CPUFREQ_LEVELS	64
#define	TSADC_IDLE_POLL_MS		1000

/* V3 GFR registers */
#define	GRF_SARADC_TESTBIT			0x0e644
#define	 GRF_SARADC_TESTBIT_ON				(0x10001 << 2)
#define GRF_TSADC_TESTBIT_L			0x0e648
#define	 GRF_TSADC_VCM_EN_L				(0x10001 << 7)
#define	GRF_TSADC_TESTBIT_H			0x0e64c
#define	 GRF_TSADC_VCM_EN_H				(0x10001 << 7)
#define	 GRF_TSADC_TESTBIT_H_ON				(0x10001 << 2)

/* V7 GRF register */
#define	GRF_TSADC_CON				0x0600
#define	 GRF_TSADC_ANA_REG0			(0x10001 << 0)
#define	 GRF_TSADC_ANA_REG1			(0x10001 << 1)
#define	 GRF_TSADC_ANA_REG2			(0x10001 << 2)
#define	 GRF_TSADC_TSEN				(0x10001 << 8)

#define	WR4(_sc, _r, _v)	bus_write_4((_sc)->mem_res, (_r), (_v))
#define	RD4(_sc, _r)		bus_read_4((_sc)->mem_res, (_r))

static struct sysctl_ctx_list tsadc_sysctl_ctx;

struct tsensor {
	char 			*name;
	int			id;
	int			channel;
};

struct rk_calib_entry {
	uint32_t	raw;
	int		temp;
};

struct tsadc_calib_info {
	struct rk_calib_entry	*table;
	int			nentries;
};

struct tsadc_softc;

struct tsadc_cooling_cpu {
	phandle_t	node;
	device_t	dev;
	bool		capped;
};

struct tsadc_thermal_zone {
	char		name[32];
	int		sensor;
	int		trip;
	int		hysteresis;
	int		polling_ms;
	int		limit_freq;
	int		ncpus;
	bool		throttled;
	struct tsadc_cooling_cpu cpus[TSADC_MAX_COOLING_CPUS];
};

struct tsadc_conf {
	int			version;
	int			q_sel_ntc;
	int			shutdown_temp;
	int			shutdown_mode;
	int			shutdown_pol;
	struct tsensor		*tsensors;
	int			ntsensors;
	struct tsadc_calib_info	calib_info;
	void			(*init)(struct tsadc_softc *);
	void			(*init_tsensor)(struct tsadc_softc *,
				    struct tsensor *);
	uint32_t		(*read_data)(struct tsadc_softc *,
				    struct tsensor *);
	int			(*intr)(struct tsadc_softc *);
	uint32_t		auto_mode_enable;
};

struct tsadc_softc {
	device_t		dev;
	struct resource		*mem_res;
	struct resource		*irq_res;
	void			*irq_ih;

	clk_t			tsadc_clk;
	clk_t			apb_pclk_clk;
	hwreset_array_t		hwreset;
	struct syscon		*grf;

	struct tsadc_conf	*conf;

	int			shutdown_temp;
	int			shutdown_mode;
	int			shutdown_pol;

	int			alarm_temp;

	struct callout		thermal_callout;
	struct task		thermal_task;
	struct tsadc_thermal_zone thermal_zones[TSADC_MAX_THERMAL_ZONES];
	int			nthermal_zones;
	int			thermal_trip_override;
	bool			thermal_started;
	bool			detaching;
};

static void tsadc_init_v8(struct tsadc_softc *);
static void tsadc_init_tsensor_v8(struct tsadc_softc *, struct tsensor *);
static uint32_t tsadc_read_data_v8(struct tsadc_softc *, struct tsensor *);
static int tsadc_intr_v8(struct tsadc_softc *);
static void tsadc_thermal_callout(void *);

static struct rk_calib_entry rk3288_calib_data[] = {
	{3800, -40000},
	{3792, -35000},
	{3783, -30000},
	{3774, -25000},
	{3765, -20000},
	{3756, -15000},
	{3747, -10000},
	{3737, -5000},
	{3728, 0},
	{3718, 5000},
	{3708, 10000},
	{3698, 15000},
	{3688, 20000},
	{3678, 25000},
	{3667, 30000},
	{3656, 35000},
	{3645, 40000},
	{3634, 45000},
	{3623, 50000},
	{3611, 55000},
	{3600, 60000},
	{3588, 65000},
	{3575, 70000},
	{3563, 75000},
	{3550, 80000},
	{3537, 85000},
	{3524, 90000},
	{3510, 95000},
	{3496, 100000},
	{3482, 105000},
	{3467, 110000},
	{3452, 115000},
	{3437, 120000},
	{3421, 125000},
};

struct tsensor rk3288_tsensors[] = {
	{ .channel = 0, .id = 2, .name = "reserved"},
	{ .channel = 1, .id = 0, .name = "CPU"},
	{ .channel = 2, .id = 1, .name = "GPU"},
};

struct tsadc_conf rk3288_tsadc_conf = {
	.version =		TSADC_V2,
	.q_sel_ntc =		0,
	.shutdown_temp =	95000,
	.shutdown_mode =	1, /* GPIO */
	.shutdown_pol =		0, /* Low  */
	.tsensors = 		rk3288_tsensors,
	.ntsensors = 		nitems(rk3288_tsensors),
	.calib_info = 	{
			.table = rk3288_calib_data,
			.nentries = nitems(rk3288_calib_data),
	}
};

static struct rk_calib_entry rk3328_calib_data[] = {
	{296, -40000},
	{304, -35000},
	{313, -30000},
	{331, -20000},
	{340, -15000},
	{349, -10000},
	{359, -5000},
	{368, 0},
	{378, 5000},
	{388, 10000},
	{398, 15000},
	{408, 20000},
	{418, 25000},
	{429, 30000},
	{440, 35000},
	{451, 40000},
	{462, 45000},
	{473, 50000},
	{485, 55000},
	{496, 60000},
	{508, 65000},
	{521, 70000},
	{533, 75000},
	{546, 80000},
	{559, 85000},
	{572, 90000},
	{586, 95000},
	{600, 100000},
	{614, 105000},
	{629, 110000},
	{644, 115000},
	{659, 120000},
	{675, 125000},
};

static struct tsensor rk3328_tsensors[] = {
	{ .channel = 0, .id = 0, .name = "CPU"},
};

static struct tsadc_conf rk3328_tsadc_conf = {
	.version =		TSADC_V2,
	.q_sel_ntc =		1,
	.shutdown_temp =	95000,
	.shutdown_mode =	0, /* CRU */
	.shutdown_pol =		0, /* Low  */
	.tsensors = 		rk3328_tsensors,
	.ntsensors = 		nitems(rk3328_tsensors),
	.calib_info = 	{
			.table = rk3328_calib_data,
			.nentries = nitems(rk3328_calib_data),
	}
};

static struct rk_calib_entry rk3399_calib_data[] = {
	{402, -40000},
	{410, -35000},
	{419, -30000},
	{427, -25000},
	{436, -20000},
	{444, -15000},
	{453, -10000},
	{461, -5000},
	{470, 0},
	{478, 5000},
	{487, 10000},
	{496, 15000},
	{504, 20000},
	{513, 25000},
	{521, 30000},
	{530, 35000},
	{538, 40000},
	{547, 45000},
	{555, 50000},
	{564, 55000},
	{573, 60000},
	{581, 65000},
	{590, 70000},
	{599, 75000},
	{607, 80000},
	{616, 85000},
	{624, 90000},
	{633, 95000},
	{642, 100000},
	{650, 105000},
	{659, 110000},
	{668, 115000},
	{677, 120000},
	{685, 125000},
};

static struct tsensor rk3399_tsensors[] = {
	{ .channel = 0, .id = 0, .name = "CPU"},
	{ .channel = 1, .id = 1, .name = "GPU"},
};

static struct tsadc_conf rk3399_tsadc_conf = {
	.version =		TSADC_V3,
	.q_sel_ntc =		1,
	.shutdown_temp =	95000,
	.shutdown_mode =	1, /* GPIO */
	.shutdown_pol =		0, /* Low  */
	.tsensors = 		rk3399_tsensors,
	.ntsensors = 		nitems(rk3399_tsensors),
	.calib_info = 	{
			.table = rk3399_calib_data,
			.nentries = nitems(rk3399_calib_data),
	}
};

static struct rk_calib_entry rk3568_calib_data[] = {
	{0, -40000},
	{1584, -40000},
	{1620, -35000},
	{1652, -30000},
	{1688, -25000},
	{1720, -20000},
	{1756, -15000},
	{1788, -10000},
	{1824, -5000},
	{1856, 0},
	{1892, 5000},
	{1924, 10000},
	{1956, 15000},
	{1992, 20000},
	{2024, 25000},
	{2060, 30000},
	{2092, 35000},
	{2128, 40000},
	{2160, 45000},
	{2196, 50000},
	{2228, 55000},
	{2264, 60000},
	{2300, 65000},
	{2332, 70000},
	{2368, 75000},
	{2400, 80000},
	{2436, 85000},
	{2468, 90000},
	{2500, 95000},
	{2536, 100000},
	{2572, 105000},
	{2604, 110000},
	{2636, 115000},
	{2672, 120000},
	{2704, 125000},
};

static struct rk_calib_entry rk3588_calib_data[] = {
	{0, -40000},
	{215, -40000},
	{285, 25000},
	{350, 85000},
	{395, 125000},
	{TSADC_V4_DATA_MASK, 125000},
};

static struct tsensor rk3568_tsensors[] = {
	{ .channel = 0, .id = 0, .name = "CPU"},
	{ .channel = 1, .id = 1, .name = "GPU"},
};

static struct tsensor rk3588_tsensors[] = {
	{ .channel = 0, .id = 0, .name = "top"},
	{ .channel = 1, .id = 1, .name = "big_core0"},
	{ .channel = 2, .id = 2, .name = "big_core1"},
	{ .channel = 3, .id = 3, .name = "little_core"},
	{ .channel = 4, .id = 4, .name = "center"},
	{ .channel = 5, .id = 5, .name = "gpu"},
	{ .channel = 6, .id = 6, .name = "npu"},
};

static struct tsadc_conf rk3568_tsadc_conf = {
	.version =		TSADC_V7,
	.q_sel_ntc =		1,
	.shutdown_temp =        95000,
	.shutdown_mode =	1, /* GPIO */
	.shutdown_pol =		0, /* Low  */
	.tsensors =		rk3568_tsensors,
	.ntsensors =		nitems(rk3568_tsensors),
	.calib_info =	{
			.table = rk3568_calib_data,
			.nentries = nitems(rk3568_calib_data),
	}
};

static struct tsadc_conf rk3588_tsadc_conf = {
	.version =		TSADC_V8,
	.q_sel_ntc =		0,
	.shutdown_temp =	95000,
	.shutdown_mode =	1, /* GPIO */
	.shutdown_pol =		0, /* Low  */
	.tsensors =		rk3588_tsensors,
	.ntsensors =		nitems(rk3588_tsensors),
	.calib_info =	{
			.table = rk3588_calib_data,
			.nentries = nitems(rk3588_calib_data),
	},
	.init =			tsadc_init_v8,
	.init_tsensor =		tsadc_init_tsensor_v8,
	.read_data =		tsadc_read_data_v8,
	.intr =			tsadc_intr_v8,
	.auto_mode_enable =	TSADC_AUTO_CON_AUTO | TSADC_AUTO_CON_AUTO_MASK,
};

static struct ofw_compat_data compat_data[] = {
	{"rockchip,rk3288-tsadc",	(uintptr_t)&rk3288_tsadc_conf},
	{"rockchip,rk3328-tsadc",	(uintptr_t)&rk3328_tsadc_conf},
	{"rockchip,rk3399-tsadc",	(uintptr_t)&rk3399_tsadc_conf},
	{"rockchip,rk3568-tsadc",	(uintptr_t)&rk3568_tsadc_conf},
	{"rockchip,rk3588-tsadc",	(uintptr_t)&rk3588_tsadc_conf},
	{NULL,		0}
};

static uint32_t
tsadc_temp_to_raw(struct tsadc_softc *sc, int temp)
{
	struct rk_calib_entry *tbl;
	int denom, ntbl, raw, i;

	tbl = sc->conf->calib_info.table;
	ntbl = sc->conf->calib_info.nentries;

	if (temp <= tbl[0].temp)
		return (tbl[0].raw);

	if (temp >= tbl[ntbl - 1].temp)
		return (tbl[ntbl - 1].raw);

	for (i = 1; i < (ntbl - 1); i++) {
		/* Exact match */
		if (temp == tbl[i].temp)
			return (tbl[i].raw);
		if (temp < tbl[i].temp)
			break;
	}

	/*
	* Translated value is between i and i - 1 table entries.
	* Do linear interpolation for it.
	*/
	raw = (int)tbl[i - 1].raw - (int)tbl[i].raw;
	raw *= temp - tbl[i - 1].temp;
	denom = tbl[i - 1].temp - tbl[i].temp;
	raw = tbl[i - 1].raw + raw / denom;
	return (raw);
}

static int
tsadc_raw_to_temp(struct tsadc_softc *sc, uint32_t raw)
{
	struct rk_calib_entry *tbl;
	int denom, ntbl, temp, i;
	bool descending;

	tbl = sc->conf->calib_info.table;
	ntbl = sc->conf->calib_info.nentries;
	descending = tbl[0].raw > tbl[1].raw;

	if (descending) {
		/* Raw column is in descending order. */
		if (raw >= tbl[0].raw)
			return (tbl[0].temp);
		if (raw <= tbl[ntbl - 1].raw)
			return (tbl[ntbl - 1].temp);

		for (i = ntbl - 2; i > 0; i--) {
			/* Exact match */
			if (raw == tbl[i].raw)
				return (tbl[i].temp);
			if (raw < tbl[i].raw)
				break;
		}
	} else {
		/* Raw column is in ascending order. */
		if (raw <= tbl[0].raw)
			return (tbl[0].temp);
		if (raw >= tbl[ntbl - 1].raw)
			return (tbl[ntbl - 1].temp);
		for (i = 1; i < (ntbl - 1); i++) {
			/* Exact match */
			if (raw == tbl[i].raw)
				return (tbl[i].temp);
			if (raw < tbl[i].raw)
				break;
		}
	}

	/*
	* Translated value is between i and i - 1 table entries.
	* Do linear interpolation for it.
	*/
	temp  = (int)tbl[i - 1].temp - (int)tbl[i].temp;
	temp *= raw - tbl[i - 1].raw;
	denom = tbl[i - 1].raw - tbl[i].raw;
	temp = tbl[i - 1].temp + temp / denom;
	return (temp);
}

static void
tsadc_init_tsensor_default(struct tsadc_softc *sc, struct tsensor *sensor)
{
	uint32_t val;

	/* Shutdown mode */
	val = RD4(sc, TSADC_INT_EN);
	if (sc->shutdown_mode != 0) {
		/* Signal shutdown of GPIO pin */
		val &= ~TSADC_INT_EN_2CRU_EN_SRC(sensor->channel);
		val |= TSADC_INT_EN_2GPIO_EN_SRC(sensor->channel);
	} else {
		val |= TSADC_INT_EN_2CRU_EN_SRC(sensor->channel);
		val &= ~TSADC_INT_EN_2GPIO_EN_SRC(sensor->channel);
	}
	WR4(sc, TSADC_INT_EN, val);

	/* Shutdown temperature */
	val =  tsadc_raw_to_temp(sc, sc->shutdown_temp);
	WR4(sc, TSADC_COMP_SHUT(sensor->channel), val);
	val = RD4(sc, TSADC_AUTO_CON);
	val |= TSADC_AUTO_SRC_EN(sensor->channel);
	WR4(sc, TSADC_AUTO_CON, val);

	/* Alarm temperature */
	val =  tsadc_temp_to_raw(sc, sc->alarm_temp);
	WR4(sc, TSADC_COMP_INT(sensor->channel), val);
	val = RD4(sc, TSADC_INT_EN);
	val |= TSADC_COMP_INT_SRC_EN(sensor->channel);
	WR4(sc, TSADC_INT_EN, val);
}

static void
tsadc_init_tsensor_v8(struct tsadc_softc *sc, struct tsensor *sensor)
{
	uint32_t val;

	if (sc->shutdown_mode != 0) {
		WR4(sc, TSADC_V3_HSHUT_GPIO_INT_EN,
		    TSADC_V3_INT_SRC_EN(sensor->channel) |
		    TSADC_V3_INT_SRC_EN_MASK(sensor->channel));
		WR4(sc, TSADC_V3_HSHUT_CRU_INT_EN,
		    TSADC_V3_INT_SRC_EN_MASK(sensor->channel));
	} else {
		WR4(sc, TSADC_V3_HSHUT_CRU_INT_EN,
		    TSADC_V3_INT_SRC_EN(sensor->channel) |
		    TSADC_V3_INT_SRC_EN_MASK(sensor->channel));
		WR4(sc, TSADC_V3_HSHUT_GPIO_INT_EN,
		    TSADC_V3_INT_SRC_EN_MASK(sensor->channel));
	}

	val = tsadc_temp_to_raw(sc, sc->shutdown_temp);
	WR4(sc, TSADC_V3_COMP_SHUT(sensor->channel), val);
	WR4(sc, TSADC_V3_AUTO_SRC_CON,
	    TSADC_V3_AUTO_SRC_EN(sensor->channel) |
	    TSADC_V3_AUTO_SRC_EN_MASK(sensor->channel));

	val = tsadc_temp_to_raw(sc, sc->alarm_temp);
	WR4(sc, TSADC_V3_COMP_INT(sensor->channel), val);
	WR4(sc, TSADC_V3_HT_INT_EN,
	    TSADC_V3_INT_SRC_EN(sensor->channel) |
	    TSADC_V3_INT_SRC_EN_MASK(sensor->channel));
}

static void
tsadc_init_tsensor(struct tsadc_softc *sc, struct tsensor *sensor)
{
	if (sc->conf->init_tsensor != NULL)
		sc->conf->init_tsensor(sc, sensor);
	else
		tsadc_init_tsensor_default(sc, sensor);
}

static void
tsadc_init_auto_con(struct tsadc_softc *sc)
{
	uint32_t val;

	val = 0;	/* XXX Is this right? */
	if (sc->shutdown_pol != 0)
		val |= TSADC_AUTO_CON_POL_HI;
	else
		val &= ~TSADC_AUTO_CON_POL_HI;
	if (sc->conf->q_sel_ntc)
		val |= TSADC_AUTO_Q_SEL;
	WR4(sc, TSADC_AUTO_CON, val);
}

static void
tsadc_init_default(struct tsadc_softc *sc)
{
	tsadc_init_auto_con(sc);

	switch (sc->conf->version) {
	case TSADC_V2:
		/* V2 init */
		WR4(sc, TSADC_AUTO_PERIOD, 250); 	/* 250 ms */
		WR4(sc, TSADC_AUTO_PERIOD_HT, 50);	/*  50 ms */
		WR4(sc, TSADC_HIGHT_INT_DEBOUNCE, 4);
		WR4(sc, TSADC_HIGHT_TSHUT_DEBOUNCE, 4);
		break;
	case TSADC_V3:
		/* V3 init */
		if (sc->grf == NULL) {
			/* Errata: adjust interleave to working value */
			WR4(sc, TSADC_USER_CON, 13 << 6); 	/* 13 clks */
		} else {
			SYSCON_WRITE_4(sc->grf, GRF_TSADC_TESTBIT_L,
			    GRF_TSADC_VCM_EN_L);
			SYSCON_WRITE_4(sc->grf, GRF_TSADC_TESTBIT_H,
			    GRF_TSADC_VCM_EN_H);
			DELAY(30);  /* 15 usec min */

			SYSCON_WRITE_4(sc->grf, GRF_SARADC_TESTBIT,
			    GRF_SARADC_TESTBIT_ON);
			SYSCON_WRITE_4(sc->grf, GRF_TSADC_TESTBIT_H,
			    GRF_TSADC_TESTBIT_H_ON);
			DELAY(180);  /* 90 usec min */
		}
		WR4(sc, TSADC_AUTO_PERIOD, 1875); 	/* 2.5 ms */
		WR4(sc, TSADC_AUTO_PERIOD_HT, 1875);	/* 2.5 ms */
		WR4(sc, TSADC_HIGHT_INT_DEBOUNCE, 4);
		WR4(sc, TSADC_HIGHT_TSHUT_DEBOUNCE, 4);
		break;
	case TSADC_V7:
		/* V7 init */
		WR4(sc, TSADC_USER_CON, 0xfc0);		/* 97us, at least 90us */
		WR4(sc, TSADC_AUTO_PERIOD, 1622);	/* 2.5ms */
		WR4(sc, TSADC_HIGHT_INT_DEBOUNCE, 4);
		WR4(sc, TSADC_AUTO_PERIOD_HT, 1622);	/* 2.5ms */
		WR4(sc, TSADC_HIGHT_TSHUT_DEBOUNCE, 4);
		if (sc->grf) {
			SYSCON_WRITE_4(sc->grf, GRF_TSADC_CON, GRF_TSADC_TSEN);
			DELAY(15);			/* 10 usec min */
			SYSCON_WRITE_4(sc->grf, GRF_TSADC_CON,
			    GRF_TSADC_ANA_REG0);
			SYSCON_WRITE_4(sc->grf, GRF_TSADC_CON,
			    GRF_TSADC_ANA_REG1);
			SYSCON_WRITE_4(sc->grf, GRF_TSADC_CON,
			    GRF_TSADC_ANA_REG2);
			DELAY(100);			/* 90 usec min */
		}
		break;
	}
}

static void
tsadc_init_v8(struct tsadc_softc *sc)
{
	tsadc_init_auto_con(sc);

	WR4(sc, TSADC_V3_AUTO_PERIOD, 5000);	/* 2.5ms */
	WR4(sc, TSADC_V3_AUTO_PERIOD_HT, 5000);	/* 2.5ms */
	WR4(sc, TSADC_V3_HIGHT_INT_DEBOUNCE, 4);
	WR4(sc, TSADC_V3_HIGHT_TSHUT_DEBOUNCE, 4);
	if (sc->shutdown_pol != 0)
		WR4(sc, TSADC_AUTO_CON, TSADC_AUTO_CON_POL_HI |
		    TSADC_AUTO_CON_POL_HI_MASK);
	else
		WR4(sc, TSADC_AUTO_CON, TSADC_AUTO_CON_POL_HI_MASK);
}

static void
tsadc_init(struct tsadc_softc *sc)
{
	if (sc->conf->init != NULL)
		sc->conf->init(sc);
	else
		tsadc_init_default(sc);
}

static uint32_t
tsadc_read_data_default(struct tsadc_softc *sc, struct tsensor *sensor)
{
	return (RD4(sc, TSADC_DATA(sensor->channel)));
}

static uint32_t
tsadc_read_data_v8(struct tsadc_softc *sc, struct tsensor *sensor)
{
	return (RD4(sc, TSADC_V3_DATA(sensor->channel)) &
	    TSADC_V4_DATA_MASK);
}

static uint32_t
tsadc_read_data(struct tsadc_softc *sc, struct tsensor *sensor)
{
	if (sc->conf->read_data != NULL)
		return (sc->conf->read_data(sc, sensor));
	return (tsadc_read_data_default(sc, sensor));
}

static int
tsadc_read_temp(struct tsadc_softc *sc, struct tsensor *sensor, int *temp)
{
	uint32_t val;

	val = tsadc_read_data(sc, sensor);
	*temp = tsadc_raw_to_temp(sc, val);

#ifdef DEBUG
	device_printf(sc->dev, "%s: Sensor(id: %d, ch: %d), val: %d temp: %d\n",
	    __func__, sensor->id, sensor->channel, val, *temp);
	device_printf(sc->dev, "%s: user_con=0x%08x auto_con=0x%08x "
	    "comp_int=0x%08x comp_shut=0x%08x\n",
	    __func__, RD4(sc, TSADC_USER_CON), RD4(sc, TSADC_AUTO_CON),
	    RD4(sc, TSADC_COMP_INT(sensor->channel)),
	    RD4(sc, TSADC_COMP_SHUT(sensor->channel)));
#endif
	return (0);
}

static int
tsadc_get_temp(device_t dev, device_t cdev, uintptr_t id, int *val)
{
	struct tsadc_softc *sc;
	int i, rv;

	sc = device_get_softc(dev);

	if (id >= sc->conf->ntsensors)
		return (ERANGE);

	for (i = 0; i < sc->conf->ntsensors; i++) {
		if (sc->conf->tsensors->id == id) {
			rv =tsadc_read_temp(sc, sc->conf->tsensors + id, val);
			return (rv);
		}
	}
	return (ERANGE);
}

static device_t
tsadc_cpufreq_device(phandle_t node)
{
	devclass_t dc;
	device_t *devs, cf_dev;
	int count, i;

	dc = devclass_find("cpu");
	if (dc == NULL || devclass_get_devices(dc, &devs, &count) != 0)
		return (NULL);
	cf_dev = NULL;
	for (i = 0; i < count; i++) {
		if (ofw_bus_get_node(devs[i]) != node)
			continue;
		cf_dev = device_find_child(devs[i], "cpufreq", -1);
		break;
	}
	free(devs, M_TEMP);
	return (cf_dev);
}

static int
tsadc_thermal_cap_cpu(struct tsadc_cooling_cpu *cpu, int target,
    int *applied)
{
	struct cf_level current, *levels;
	device_t dev;
	int count, error, i, selected;

	dev = cpu->dev;
	if (dev == NULL) {
		dev = tsadc_cpufreq_device(cpu->node);
		if (dev == NULL)
			return (ENXIO);
		cpu->dev = dev;
	}

	levels = mallocarray(TSADC_MAX_CPUFREQ_LEVELS, sizeof(*levels),
	    M_TEMP, M_WAITOK);
	count = TSADC_MAX_CPUFREQ_LEVELS;
	error = CPUFREQ_LEVELS(dev, levels, &count);
	if (error != 0)
		goto out;
	if (count == 0) {
		error = ENXIO;
		goto out;
	}
	selected = -1;
	if (target == 0) {
		error = CPUFREQ_GET(dev, &current);
		if (error != 0)
			goto out;
		for (i = 0; i < count; i++) {
			if (levels[i].total_set.freq >= current.total_set.freq)
				continue;
			if (selected == -1 || levels[i].total_set.freq >
			    levels[selected].total_set.freq)
				selected = i;
		}
		if (selected == -1) {
			for (i = 0; i < count; i++) {
				if (levels[i].total_set.freq ==
				    current.total_set.freq) {
					selected = i;
					break;
				}
			}
		}
	} else {
		for (i = 0; i < count; i++) {
			if (levels[i].total_set.freq == target) {
				selected = i;
				break;
			}
		}
	}
	if (selected == -1) {
		error = ENXIO;
		goto out;
	}
	error = CPUFREQ_SET(dev, &levels[selected], CPUFREQ_PRIO_KERN);
	if (error == 0) {
		cpu->capped = true;
		*applied = levels[selected].total_set.freq;
	}
out:
	free(levels, M_TEMP);
	return (error);
}

static bool
tsadc_thermal_restore_zone(struct tsadc_thermal_zone *zone)
{
	bool capped;
	int error, i;

	capped = false;
	for (i = 0; i < zone->ncpus; i++) {
		if (!zone->cpus[i].capped)
			continue;
		error = CPUFREQ_SET(zone->cpus[i].dev, NULL,
		    CPUFREQ_PRIO_KERN);
		if (error == 0)
			zone->cpus[i].capped = false;
		else
			capped = true;
	}
	zone->throttled = capped;
	if (!capped)
		zone->limit_freq = 0;
	return (!capped);
}

static void
tsadc_thermal_task(void *arg, int pending)
{
	struct tsadc_softc *sc;
	struct tsadc_thermal_zone *zone;
	bool active, capped;
	int applied, delay, error, i, j, target, temp, trip;

	sc = arg;
	active = false;
	delay = TSADC_IDLE_POLL_MS;
	for (i = 0; i < sc->nthermal_zones; i++) {
		zone = &sc->thermal_zones[i];
		error = tsadc_read_temp(sc, sc->conf->tsensors + zone->sensor,
		    &temp);
		if (error != 0)
			continue;
		trip = sc->thermal_trip_override != 0 ?
		    sc->thermal_trip_override : zone->trip;
		if (temp >= trip) {
			capped = false;
			target = 0;
			for (j = 0; j < zone->ncpus; j++) {
				if (tsadc_thermal_cap_cpu(&zone->cpus[j], target,
				    &applied) == 0) {
					capped = true;
					if (target == 0)
						target = applied;
				}
			}
			if (capped && target != zone->limit_freq) {
				device_printf(sc->dev,
				    "%s: %d.%03dC, CPU frequency capped at %d MHz\n",
				    zone->name, temp / 1000, abs(temp % 1000), target);
				zone->limit_freq = target;
				zone->throttled = true;
			}
		} else if (zone->throttled &&
		    temp <= trip - zone->hysteresis) {
			if (tsadc_thermal_restore_zone(zone))
				device_printf(sc->dev,
				    "%s: %d.%03dC, CPU frequency restored\n",
				    zone->name, temp / 1000, abs(temp % 1000));
		}
		if (zone->throttled) {
			active = true;
			delay = MIN(delay, zone->polling_ms);
		}
	}

	if (!sc->detaching)
		callout_reset(&sc->thermal_callout,
		    MAX(1, (hz * (active ? delay : TSADC_IDLE_POLL_MS) + 999) /
		    1000), tsadc_thermal_callout, sc);
}

static void
tsadc_thermal_callout(void *arg)
{
	struct tsadc_softc *sc;

	sc = arg;
	if (!sc->detaching)
		taskqueue_enqueue(taskqueue_thread, &sc->thermal_task);
}

static void
tsadc_thermal_add_cpu(struct tsadc_thermal_zone *zone, phandle_t node)
{
	int i;

	for (i = 0; i < zone->ncpus; i++) {
		if (zone->cpus[i].node == node)
			return;
	}
	if (zone->ncpus < TSADC_MAX_COOLING_CPUS)
		zone->cpus[zone->ncpus++].node = node;
}

static void
tsadc_thermal_parse_maps(struct tsadc_thermal_zone *zone, phandle_t node,
    phandle_t trip)
{
	pcell_t *cells, ncooling;
	phandle_t cnode, map, maps, xref;
	char type[8];
	int i, len, ncells;

	maps = ofw_bus_find_child(node, "cooling-maps");
	if (maps <= 0)
		return;
	for (map = OF_child(maps); map > 0; map = OF_peer(map)) {
		if (OF_getencprop(map, "trip", &xref, sizeof(xref)) <= 0 ||
		    xref != OF_xref_from_node(trip))
			continue;
		ncells = OF_getencprop_alloc_multi(map, "cooling-device",
		    sizeof(*cells), (void **)&cells);
		if (ncells <= 0)
			continue;
		for (i = 0; i < ncells;) {
			xref = cells[i++];
			cnode = OF_node_from_xref(xref);
			if (cnode <= 0 || OF_getencprop(cnode,
			    "#cooling-cells", &ncooling, sizeof(ncooling)) <= 0 ||
			    ncooling > (pcell_t)(ncells - i))
				break;
			len = OF_getprop(cnode, "device_type", type,
			    sizeof(type) - 1);
			if (len > 0) {
				type[MIN(len, (int)sizeof(type) - 1)] = '\0';
			}
			if (ncooling == 2 && len > 0 && strcmp(type, "cpu") == 0)
				tsadc_thermal_add_cpu(zone, cnode);
			i += ncooling;
		}
		OF_prop_free(cells);
	}
}

static void
tsadc_thermal_parse(struct tsadc_softc *sc, phandle_t sensor_node)
{
	struct tsadc_thermal_zone *zone;
	pcell_t sensor[2], val;
	phandle_t node, root, trip, trips;
	char type[16];
	int len;

	root = OF_finddevice("/thermal-zones");
	if (root <= 0)
		return;
	for (node = OF_child(root); node > 0 &&
	    sc->nthermal_zones < TSADC_MAX_THERMAL_ZONES;
	    node = OF_peer(node)) {
		if (OF_getencprop(node, "thermal-sensors", sensor,
		    sizeof(sensor)) != sizeof(sensor) ||
		    sensor[0] != OF_xref_from_node(sensor_node) ||
		    sensor[1] >= (pcell_t)sc->conf->ntsensors)
			continue;
		trips = ofw_bus_find_child(node, "trips");
		if (trips <= 0)
			continue;
		for (trip = OF_child(trips); trip > 0; trip = OF_peer(trip)) {
			len = OF_getprop(trip, "type", type, sizeof(type) - 1);
			if (len <= 0)
				continue;
			type[MIN(len, (int)sizeof(type) - 1)] = '\0';
			if (strcmp(type, "passive") == 0)
				break;
		}
		if (trip <= 0)
			continue;

		zone = &sc->thermal_zones[sc->nthermal_zones];
		zone->sensor = sensor[1];
		if (OF_getencprop(trip, "temperature", &val, sizeof(val)) <= 0)
			continue;
		zone->trip = val;
		if (OF_getencprop(trip, "hysteresis", &val, sizeof(val)) <= 0)
			val = 0;
		zone->hysteresis = val;
		if (OF_getencprop(node, "polling-delay-passive", &val,
		    sizeof(val)) <= 0 || val == 0)
			val = 100;
		zone->polling_ms = MAX(10, (int)val);
		len = OF_getprop(node, "name", zone->name,
		    sizeof(zone->name) - 1);
		if (len <= 0)
			strlcpy(zone->name, "thermal-zone", sizeof(zone->name));
		else
			zone->name[MIN(len, (int)sizeof(zone->name) - 1)] = '\0';
		tsadc_thermal_parse_maps(zone, node, trip);
		if (zone->ncpus != 0)
			sc->nthermal_zones++;
		else
			memset(zone, 0, sizeof(*zone));
	}
}

static int
tsadc_sysctl_trip_override(SYSCTL_HANDLER_ARGS)
{
	struct tsadc_softc *sc;
	int error, val;

	sc = arg1;
	val = sc->thermal_trip_override;
	error = sysctl_handle_int(oidp, &val, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	if (val != 0 && (val < 30000 || val >= sc->shutdown_temp))
		return (EINVAL);
	sc->thermal_trip_override = val;
	if (sc->thermal_started)
		taskqueue_enqueue(taskqueue_thread, &sc->thermal_task);
	return (0);
}

static int
tsadc_sysctl_temperature(SYSCTL_HANDLER_ARGS)
{
	struct tsadc_softc *sc;
	int val;
	int rv;
	int id;

	/* Write request */
	if (req->newptr != NULL)
		return (EINVAL);

	sc = arg1;
	id = arg2;

	if (id >= sc->conf->ntsensors)
		return (ERANGE);
	rv =  tsadc_read_temp(sc, sc->conf->tsensors + id, &val);
	if (rv != 0)
		return (rv);

	val = val / 100;
	val +=  2731;
	rv = sysctl_handle_int(oidp, &val, 0, req);
	return (rv);
}

static int
tsadc_init_sysctl(struct tsadc_softc *sc)
{
	int i;
	struct sysctl_oid *oid, *tmp;

	sysctl_ctx_init(&tsadc_sysctl_ctx);
	/* create node for hw.temp */
	oid = SYSCTL_ADD_NODE(&tsadc_sysctl_ctx,
	    SYSCTL_STATIC_CHILDREN(_hw), OID_AUTO, "temperature",
	    CTLFLAG_RD | CTLFLAG_MPSAFE, NULL, "");
	if (oid == NULL)
		return (ENXIO);

	/* Add sensors */
	for (i = sc->conf->ntsensors  - 1; i >= 0; i--) {
		tmp = SYSCTL_ADD_PROC(&tsadc_sysctl_ctx,
		    SYSCTL_CHILDREN(oid), OID_AUTO, sc->conf->tsensors[i].name,
		    CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_NEEDGIANT, sc, i,
		    tsadc_sysctl_temperature, "IK", "SoC Temperature");
		if (tmp == NULL)
			return (ENXIO);
	}

	return (0);
}

static int
tsadc_intr_default(struct tsadc_softc *sc)
{
	uint32_t val;

	val = RD4(sc, TSADC_INT_PD);
	WR4(sc, TSADC_INT_PD, val);

	/* XXX Handle shutdown and alarm interrupts. */
	if (val & 0x00F0) {
		device_printf(sc->dev, "Alarm: device temperature "
		    "is above of shutdown level.\n");
	} else if (val & 0x000F) {
		device_printf(sc->dev, "Alarm: device temperature "
		    "is above of alarm level.\n");
	}
	return (FILTER_HANDLED);
}

static int
tsadc_intr_v8(struct tsadc_softc *sc)
{
	uint32_t val;

	val = RD4(sc, TSADC_V3_INT_PD);
	WR4(sc, TSADC_V3_INT_PD, val & TSADC_V4_INT_PD_CLEAR_MASK);
	val = RD4(sc, TSADC_V3_HSHUT_PD);
	WR4(sc, TSADC_V3_HSHUT_PD, val & TSADC_V4_INT_PD_CLEAR_MASK);
	return (FILTER_HANDLED);
}

static int
tsadc_intr(void *arg)
{
	struct tsadc_softc *sc;

	sc = (struct tsadc_softc *)arg;

	if (sc->conf->intr != NULL)
		return (sc->conf->intr(sc));
	return (tsadc_intr_default(sc));
}

static int
tsadc_node_is_compatible(phandle_t node)
{
	int i;

	for (i = 0; compat_data[i].ocd_str != NULL; i++) {
		if (ofw_bus_node_is_compatible(node, compat_data[i].ocd_str))
			return (1);
	}

	return (0);
}

static void
tsadc_identify(driver_t *driver, device_t parent)
{
	phandle_t node;

	if (device_find_child(parent, "rk_tsadc", -1) != NULL)
		return;

	for (node = OF_child(OF_peer(0)); node > 0; node = OF_peer(node)) {
		if (!tsadc_node_is_compatible(node))
			continue;
		if (!ofw_bus_node_status_okay(node))
			continue;
		simplebus_add_device(parent, node, 1000, "rk_tsadc", -1, NULL);
		return;
	}
}

static int
tsadc_probe(device_t dev)
{
	uintptr_t data;

	data = ofw_bus_search_compatible(dev, compat_data)->ocd_data;

	if (!ofw_bus_status_okay(dev))
		return (ENXIO);

	if (data == 0)
		return (ENXIO);

	device_set_desc(dev, "RockChip temperature sensors");
	return (BUS_PROBE_DEFAULT);
}

static int
tsadc_get_clock(struct tsadc_softc *sc, phandle_t node, const char *name,
    int idx, clk_t *clk)
{
	int rv;

	rv = clk_get_by_ofw_name(sc->dev, node, name, clk);
	if (rv == 0)
		return (0);

	rv = clk_get_by_ofw_index(sc->dev, node, idx, clk);

	return (rv);
}

static int
tsadc_attach(device_t dev)
{
	struct tsadc_softc *sc;
	phandle_t node;
	uint32_t val;
	int i, rid, rv;

	sc = device_get_softc(dev);
	sc->dev = dev;
	node = ofw_bus_get_node(sc->dev);
	sc->conf = (struct tsadc_conf *)
	    ofw_bus_search_compatible(dev, compat_data)->ocd_data;
	sc->alarm_temp = 90000;

	rid = 0;
	sc->mem_res = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid,
	    RF_ACTIVE);
	if (sc->mem_res == NULL) {
		device_printf(dev, "Cannot allocate memory resources\n");
		goto fail;
	}

	rid = 0;
	sc->irq_res = bus_alloc_resource_any(dev, SYS_RES_IRQ, &rid, RF_ACTIVE);
	if (sc->irq_res == NULL) {
		device_printf(dev, "Cannot allocate IRQ resources\n");
		goto fail;
	}

	if ((bus_setup_intr(dev, sc->irq_res, INTR_TYPE_MISC | INTR_MPSAFE,
	    tsadc_intr, NULL, sc, &sc->irq_ih))) {
		device_printf(dev,
		    "WARNING: unable to register interrupt handler\n");
		goto fail;
	}

	/* FDT resources */
	rv = hwreset_array_get_ofw(dev, 0, &sc->hwreset);
	if (rv != 0) {
		device_printf(dev, "Cannot get resets\n");
		goto fail;
	}
	rv = tsadc_get_clock(sc, node, "tsadc", 0, &sc->tsadc_clk);
	if (rv != 0) {
		device_printf(dev, "Cannot get 'tsadc' clock: %d\n", rv);
		goto fail;
	}
	rv = tsadc_get_clock(sc, node, "apb_pclk", 1, &sc->apb_pclk_clk);
	if (rv != 0) {
		device_printf(dev, "Cannot get 'apb_pclk' clock: %d\n", rv);
		goto fail;
	}

	/* grf is optional */
	rv = syscon_get_by_ofw_property(dev, node, "rockchip,grf", &sc->grf);
	if (rv != 0 && rv != ENOENT) {
		device_printf(dev, "Cannot get 'grf' syscon: %d\n", rv);
		goto fail;
	}

	rv = OF_getencprop(node, "rockchip,hw-tshut-temp",
	    &sc->shutdown_temp, sizeof(sc->shutdown_temp));
	if (rv <= 0)
		sc->shutdown_temp = sc->conf->shutdown_temp;

	rv = OF_getencprop(node, "rockchip,hw-tshut-mode",
	    &sc->shutdown_mode, sizeof(sc->shutdown_mode));
	if (rv <= 0)
		sc->shutdown_mode = sc->conf->shutdown_mode;

	rv = OF_getencprop(node, "rockchip,hw-tshut-polarity",
	    &sc->shutdown_pol, sizeof(sc->shutdown_pol));
	if (rv <= 0)
		sc->shutdown_pol = sc->conf->shutdown_pol;

	/* Wakeup controller */
	rv = hwreset_array_assert(sc->hwreset);
	if (rv != 0) {
		device_printf(dev, "Cannot assert reset\n");
		goto fail;
	}

	/* Set the assigned clocks parent and freq */
	rv = clk_set_assigned(sc->dev, node);
	if (rv != 0 && rv != ENOENT) {
		device_printf(dev, "clk_set_assigned failed\n");
		goto fail;
	}

	rv = clk_enable(sc->tsadc_clk);
	if (rv != 0) {
		device_printf(dev, "Cannot enable 'tsadc_clk' clock: %d\n", rv);
		goto fail;
	}
	rv = clk_enable(sc->apb_pclk_clk);
	if (rv != 0) {
		device_printf(dev, "Cannot enable 'apb_pclk' clock: %d\n", rv);
		goto fail;
	}
	rv = hwreset_array_deassert(sc->hwreset);
	if (rv != 0) {
		device_printf(dev, "Cannot deassert reset\n");
		goto fail;
	}

	tsadc_init(sc);
	for (i = 0; i < sc->conf->ntsensors; i++)
		tsadc_init_tsensor(sc, sc->conf->tsensors + i);

	/* Enable auto mode */
	val = RD4(sc, TSADC_AUTO_CON);
	if (sc->conf->auto_mode_enable != 0)
		val |= sc->conf->auto_mode_enable;
	else
		val |= TSADC_AUTO_CON_AUTO;
	WR4(sc, TSADC_AUTO_CON, val);

	rv = tsadc_init_sysctl(sc);
	if (rv != 0) {
		device_printf(sc->dev, "Cannot initialize sysctls\n");
		goto fail_sysctl;
	}

	OF_device_register_xref(OF_xref_from_node(node), dev);
	if (sc->conf == &rk3588_tsadc_conf) {
		tsadc_thermal_parse(sc, node);
		if (sc->nthermal_zones != 0) {
			SYSCTL_ADD_PROC(device_get_sysctl_ctx(dev),
			    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
			    "passive_trip_override",
			    CTLTYPE_INT | CTLFLAG_RWTUN | CTLFLAG_MPSAFE, sc, 0,
			    tsadc_sysctl_trip_override, "I",
			    "Passive trip override in millidegrees C; 0 uses DT");
			TASK_INIT(&sc->thermal_task, 0, tsadc_thermal_task, sc);
			callout_init(&sc->thermal_callout, 1);
			sc->thermal_started = true;
			callout_reset(&sc->thermal_callout, hz,
			    tsadc_thermal_callout, sc);
			device_printf(dev, "%d CPU thermal zones enabled\n",
			    sc->nthermal_zones);
		}
	}
	return (bus_generic_attach(dev));

fail_sysctl:
	sysctl_ctx_free(&tsadc_sysctl_ctx);
fail:
	if (sc->irq_ih != NULL)
		bus_teardown_intr(dev, sc->irq_res, sc->irq_ih);
	if (sc->tsadc_clk != NULL)
		clk_release(sc->tsadc_clk);
	if (sc->apb_pclk_clk != NULL)
		clk_release(sc->apb_pclk_clk);
	if (sc->hwreset != NULL)
		hwreset_array_release(sc->hwreset);
	if (sc->irq_res != NULL)
		bus_release_resource(dev, SYS_RES_IRQ, 0, sc->irq_res);
	if (sc->mem_res != NULL)
		bus_release_resource(dev, SYS_RES_MEMORY, 0, sc->mem_res);

	return (ENXIO);
}

static int
tsadc_detach(device_t dev)
{
	struct tsadc_softc *sc;
	int i;

	sc = device_get_softc(dev);

	if (sc->thermal_started) {
		sc->detaching = true;
		callout_drain(&sc->thermal_callout);
		taskqueue_drain(taskqueue_thread, &sc->thermal_task);
		for (i = 0; i < sc->nthermal_zones; i++)
			tsadc_thermal_restore_zone(&sc->thermal_zones[i]);
	}
	if (sc->irq_ih != NULL)
		bus_teardown_intr(dev, sc->irq_res, sc->irq_ih);
	sysctl_ctx_free(&tsadc_sysctl_ctx);
	if (sc->tsadc_clk != NULL)
		clk_release(sc->tsadc_clk);
	if (sc->apb_pclk_clk != NULL)
		clk_release(sc->apb_pclk_clk);
	if (sc->hwreset != NULL)
		hwreset_array_release(sc->hwreset);
	if (sc->irq_res != NULL)
		bus_release_resource(dev, SYS_RES_IRQ, 0, sc->irq_res);
	if (sc->mem_res != NULL)
		bus_release_resource(dev, SYS_RES_MEMORY, 0, sc->mem_res);

	return (ENXIO);
}

static device_method_t rk_tsadc_methods[] = {
	/* Device interface */
	DEVMETHOD(device_identify,		tsadc_identify),
	DEVMETHOD(device_probe,			tsadc_probe),
	DEVMETHOD(device_attach,		tsadc_attach),
	DEVMETHOD(device_detach,		tsadc_detach),

	/* TSADC interface */
	DEVMETHOD(rk_tsadc_get_temperature,	tsadc_get_temp),

	DEVMETHOD_END
};

static DEFINE_CLASS_0(rk_tsadc, rk_tsadc_driver, rk_tsadc_methods,
    sizeof(struct tsadc_softc));
SIMPLEBUS_PNP_INFO(compat_data);
OFWBUS_PNP_INFO(compat_data);
EARLY_DRIVER_MODULE(rk_tsadc, simplebus, rk_tsadc_driver, NULL, NULL,
    BUS_PASS_TIMER + BUS_PASS_ORDER_LAST);
DRIVER_MODULE(rk_tsadc, ofwbus, rk_tsadc_driver, NULL, NULL);
