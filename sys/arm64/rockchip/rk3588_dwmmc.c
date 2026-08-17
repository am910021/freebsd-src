/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/taskqueue.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#include <dev/mmc/bridge.h>
#include <dev/mmc/mmc_fdt_helpers.h>
#include <dev/mmc/mmc_subr.h>
#include <dev/mmc/mmcbrvar.h>
#include <dev/mmc/host/dwmmc_var.h>
#include <dev/mmc/host/dwmmc_soc.h>

#include "clkdev_if.h"

#define	RK3588_DWMMC_SAFE_FMAX	200000000
#define	RK3588_DWMMC_POWER_OFF_DELAY_MS	10
#define	RK3588_DWMMC_UNTUNED_CAPS	MMC_CAP_UHS_DDR50
#define	RK3588_DWMMC_RETRY_SCHEDULED	(1U << 30)

#define	RK3588_CRU_SCLK_SDMMC_DRV	685
#define	RK3588_CRU_SCLK_SDMMC_SAMPLE	686
#define	RK3588_SDMMC_CON0		0x0c30
#define	RK3588_SDMMC_CON1		0x0c34
#define	RK3588_MMC_PHASE_SHIFT		1
#define	RK3588_MMC_PHASE_MASK		0x07ff
#define	RK3588_MMC_DELAY_SEL		(1U << 10)
#define	RK3588_MMC_DELAYNUM_SHIFT	2
#define	RK3588_MMC_DELAY_ELEMENT_PS	60
#define	RK3588_MMC_TUNING_PHASES	360
#define	RK3588_MMC_TUNING_SKIP		20

static const uint8_t rk3588_sd_tuning_pattern[MMC_TUNING_LEN] = {
	0xff, 0x0f, 0xff, 0x00, 0xff, 0xcc, 0xc3, 0xcc,
	0xc3, 0x3c, 0xcc, 0xff, 0xfe, 0xff, 0xfe, 0xef,
	0xff, 0xdf, 0xff, 0xdd, 0xff, 0xfb, 0xff, 0xfb,
	0xbf, 0xff, 0x7f, 0xff, 0x77, 0xf7, 0xbd, 0xef,
	0xff, 0xf0, 0xff, 0xf0, 0x0f, 0xfc, 0xcc, 0x3c,
	0xcc, 0x33, 0xcc, 0xcf, 0xff, 0xef, 0xff, 0xee,
	0xff, 0xfd, 0xff, 0xfd, 0xdf, 0xff, 0xbf, 0xff,
	0xbb, 0xff, 0xf7, 0xff, 0xf7, 0x7f, 0x7b, 0xde,
};

static int
rk3588_dwmmc_get_cru(struct dwmmc_softc *sc, const char *name,
    pcell_t clock_id, device_t *cru)
{
	pcell_t *cells;
	phandle_t node, xref;
	int error, idx, ncells;

	node = ofw_bus_get_node(sc->dev);
	error = ofw_bus_find_string_index(node, "clock-names", name, &idx);
	if (error != 0)
		return (error);
	cells = NULL;
	error = ofw_bus_parse_xref_list_alloc(node, "clocks", "#clock-cells",
	    idx, &xref, &ncells, &cells);
	if (error == 0 && (ncells != 1 || cells[0] != clock_id))
		error = ENXIO;
	if (cells != NULL)
		OF_prop_free(cells);
	if (error != 0)
		return (error);

	*cru = OF_device_from_xref(xref);
	if (*cru == NULL ||
	    !ofw_bus_is_compatible(*cru, "rockchip,rk3588-cru"))
		return (ENXIO);
	return (0);
}

static int
rk3588_dwmmc_read_phase(device_t cru, bus_addr_t reg, uint32_t *raw)
{
	uint32_t value;
	int error;

	CLKDEV_DEVICE_LOCK(cru);
	error = CLKDEV_READ_4(cru, reg, &value);
	CLKDEV_DEVICE_UNLOCK(cru);
	if (error == 0)
		*raw = (value >> RK3588_MMC_PHASE_SHIFT) &
		    RK3588_MMC_PHASE_MASK;
	return (error);
}

static int
rk3588_dwmmc_write_phase(device_t cru, bus_addr_t reg, uint32_t raw)
{
	uint32_t value;
	int error;

	value = (RK3588_MMC_PHASE_MASK << RK3588_MMC_PHASE_SHIFT) << 16;
	value |= (raw & RK3588_MMC_PHASE_MASK) << RK3588_MMC_PHASE_SHIFT;
	CLKDEV_DEVICE_LOCK(cru);
	error = CLKDEV_WRITE_4(cru, reg, value);
	CLKDEV_DEVICE_UNLOCK(cru);
	return (error);
}

static int
rk3588_dwmmc_set_phase(device_t cru, bus_addr_t reg, uint64_t rate,
    u_int degrees)
{
	uint64_t denominator, delay;
	uint32_t raw;
	u_int nineties, remainder;

	if (rate < 1000 || degrees >= 360)
		return (EINVAL);

	nineties = degrees / 90;
	remainder = degrees % 90;
	denominator = (rate / 1000) * 36 *
	    (RK3588_MMC_DELAY_ELEMENT_PS / 10);
	delay = (10000000ULL * remainder + denominator / 2) / denominator;
	delay = MIN(delay, 255);
	raw = delay != 0 ? RK3588_MMC_DELAY_SEL : 0;
	raw |= delay << RK3588_MMC_DELAYNUM_SHIFT;
	raw |= nineties;
	return (rk3588_dwmmc_write_phase(cru, reg, raw));
}

static int
rk3588_dwmmc_send_tuning(device_t reqdev)
{
	uint8_t buf[MMC_TUNING_LEN];
	struct mmc_command cmd;
	struct mmc_data data;
	int error;

	memset(&cmd, 0, sizeof(cmd));
	memset(&data, 0, sizeof(data));
	memset(buf, 0, sizeof(buf));
	cmd.opcode = MMC_SEND_TUNING_BLOCK;
	cmd.flags = MMC_RSP_R1 | MMC_CMD_ADTC;
	cmd.data = &data;
	data.data = buf;
	data.len = sizeof(buf);
	data.flags = MMC_DATA_READ;
	error = mmc_wait_for_cmd(reqdev, reqdev, &cmd, 0);
	if (error == MMC_ERR_NONE &&
	    memcmp(buf, rk3588_sd_tuning_pattern, sizeof(buf)) != 0)
		error = MMC_ERR_FAILED;
	return (error);
}

static int
rk3588_dwmmc_tune(struct dwmmc_softc *sc, device_t reqdev, bool hs400)
{
	bool valid[RK3588_MMC_TUNING_PHASES] = { false };
	device_t drive_cru, sample_cru;
	uint64_t ciu_rate, rate;
	uint32_t old_drive, old_sample;
	u_int best_end, best_len, drive_phase, i, phase, run;
	int error;

	if (hs400 || sc->host.ios.bus_width != bus_width_4 ||
	    (sc->host.ios.timing != bus_timing_uhs_sdr50 &&
	    sc->host.ios.timing != bus_timing_uhs_sdr104))
		return (EINVAL);

	error = rk3588_dwmmc_get_cru(sc, "ciu-drive",
	    RK3588_CRU_SCLK_SDMMC_DRV, &drive_cru);
	if (error != 0)
		return (error);
	error = rk3588_dwmmc_get_cru(sc, "ciu-sample",
	    RK3588_CRU_SCLK_SDMMC_SAMPLE, &sample_cru);
	if (error != 0 || drive_cru != sample_cru)
		return (error != 0 ? error : ENXIO);

	rate = sc->bus_hz;
	if (sc->ciu != NULL && clk_get_freq(sc->ciu, &ciu_rate) == 0 &&
	    ciu_rate >= 2)
		rate = ciu_rate / 2;
	if (rate == 0)
		return (EINVAL);

	error = rk3588_dwmmc_read_phase(sample_cru, RK3588_SDMMC_CON0,
	    &old_drive);
	if (error == 0)
		error = rk3588_dwmmc_read_phase(sample_cru, RK3588_SDMMC_CON1,
		    &old_sample);
	if (error != 0)
		return (error);

	drive_phase = sc->host.ios.timing == bus_timing_uhs_sdr104 ? 180 : 90;
	error = rk3588_dwmmc_set_phase(drive_cru, RK3588_SDMMC_CON0, rate,
	    drive_phase);
	if (error != 0)
		return (error);
	device_printf(sc->dev,
	    "RK3588-DIAG V2280 tuning start timing=%u rate=%ju "
	    "drive=%u old_drive=%#x old_sample=%#x\n",
	    sc->host.ios.timing, (uintmax_t)rate, drive_phase, old_drive,
	    old_sample);

	for (i = 0; i < RK3588_MMC_TUNING_PHASES;) {
		error = rk3588_dwmmc_set_phase(sample_cru, RK3588_SDMMC_CON1,
		    rate, i);
		if (error != 0)
			goto fail;
		valid[i] = rk3588_dwmmc_send_tuning(reqdev) == MMC_ERR_NONE;
		if (valid[i] || i == RK3588_MMC_TUNING_PHASES - 1)
			i++;
		else {
			i += RK3588_MMC_TUNING_SKIP;
			if (i >= RK3588_MMC_TUNING_PHASES)
				i = RK3588_MMC_TUNING_PHASES - 1;
		}
	}

	best_end = best_len = run = 0;
	for (i = 0; i < RK3588_MMC_TUNING_PHASES * 2; i++) {
		if (!valid[i % RK3588_MMC_TUNING_PHASES])
			run = 0;
		else if (run < RK3588_MMC_TUNING_PHASES)
			run++;
		if (run > best_len) {
			best_len = run;
			best_end = i;
		}
	}
	if (best_len == 0) {
		error = EIO;
		goto fail;
	}

	if (best_len == RK3588_MMC_TUNING_PHASES)
		phase = 0;
	else
		phase = (best_end - best_len + 1 + best_len / 2) %
		    RK3588_MMC_TUNING_PHASES;
	error = rk3588_dwmmc_set_phase(sample_cru, RK3588_SDMMC_CON1,
	    rate, phase);
	if (error != 0)
		goto fail;
	device_printf(sc->dev,
	    "tuned sample phase to %u degrees (window %u-%u, %u phases, "
	    "drive %u, %ju Hz)\n", phase,
	    (best_end - best_len + 1) % RK3588_MMC_TUNING_PHASES,
	    best_end % RK3588_MMC_TUNING_PHASES, best_len, drive_phase,
	    (uintmax_t)rate);
	return (0);

fail:
	(void)rk3588_dwmmc_write_phase(sample_cru, RK3588_SDMMC_CON1,
	    old_sample);
	(void)rk3588_dwmmc_write_phase(drive_cru, RK3588_SDMMC_CON0,
	    old_drive);
	device_printf(sc->dev, "tuning failed: %d; restored clock phases\n",
	    error);
	return (error);
}

static bool
rk3588_dwmmc_match(struct dwmmc_softc *sc)
{

	return (ofw_bus_is_compatible(sc->dev, "rockchip,rk3588-dw-mshc"));
}

static bool
rk3588_dwmmc_probe(device_t dev)
{

	if (!ofw_bus_is_compatible(dev, "rockchip,rk3588-dw-mshc"))
		return (false);

	device_set_desc(dev, "Synopsys DesignWare Mobile Storage Host "
	    "Controller (RockChip RK3588)");
	return (true);
}

static void
rk3588_dwmmc_retry_attach(void *arg)
{
	device_t dev;
	int error;

	dev = arg;
	error = device_probe_and_attach(dev);
	if (error != 0)
		device_printf(dev, "retry attach failed: %d\n", error);
}

static void
rk3588_dwmmc_apply_fdt_defaults(struct dwmmc_softc *sc)
{
	if (sc->host.f_max > RK3588_DWMMC_SAFE_FMAX)
		sc->host.f_max = RK3588_DWMMC_SAFE_FMAX;
	sc->host.caps &= ~RK3588_DWMMC_UNTUNED_CAPS;
	sc->host.caps |= MMC_CAP_UHS_SDR12 | MMC_CAP_UHS_SDR25 |
	    MMC_CAP_UHS_SDR50 | MMC_CAP_UHS_SDR104;
	sc->safe_power_cycle = true;
	sc->power_off_delay_ms = RK3588_DWMMC_POWER_OFF_DELAY_MS;
	sc->tune = rk3588_dwmmc_tune;
}

static bool
rk3588_dwmmc_defer_missing_supply(struct dwmmc_softc *sc, phandle_t node,
    int error, const char *name, bool present)
{

	if (!OF_hasprop(node, name) || present ||
	    (error != ENOENT && error != ENODEV))
		return (false);

	if ((device_get_flags(sc->dev) & RK3588_DWMMC_RETRY_SCHEDULED) == 0) {
		device_set_flags(sc->dev, device_get_flags(sc->dev) |
		    RK3588_DWMMC_RETRY_SCHEDULED);
		config_intrhook_oneshot(rk3588_dwmmc_retry_attach, sc->dev);
	} else {
		device_printf(sc->dev,
		    "%s regulator unavailable after retry\n", name);
	}

	return (true);
}

static bool
rk3588_dwmmc_defer_missing_vmmc(struct dwmmc_softc *sc, phandle_t node,
    int error)
{

	return (rk3588_dwmmc_defer_missing_supply(sc, node, error,
	    "vmmc-supply", sc->vmmc != NULL));
}

static bool
rk3588_dwmmc_defer_missing_vqmmc(struct dwmmc_softc *sc, phandle_t node,
    int error)
{

	return (rk3588_dwmmc_defer_missing_supply(sc, node, error,
	    "vqmmc-supply", sc->vqmmc != NULL));
}

static void
rk3588_dwmmc_fallback_bus_hz(struct dwmmc_softc *sc)
{

	if (sc->bus_hz == 0 && sc->host.f_max != 0) {
		sc->bus_hz = sc->host.f_max;
	}
}

static const struct dwmmc_soc_ops rk3588_dwmmc_soc_ops = {
	.match = rk3588_dwmmc_match,
	.probe = rk3588_dwmmc_probe,
	.apply_fdt_defaults = rk3588_dwmmc_apply_fdt_defaults,
	.defer_missing_vmmc = rk3588_dwmmc_defer_missing_vmmc,
	.defer_missing_vqmmc = rk3588_dwmmc_defer_missing_vqmmc,
	.fallback_bus_hz = rk3588_dwmmc_fallback_bus_hz,
	.prepare_reboot = true,
};

static void
rk3588_dwmmc_soc_register(void *arg __unused)
{

	dwmmc_soc_register(&rk3588_dwmmc_soc_ops);
}
SYSINIT(rk3588_dwmmc_soc, SI_SUB_DRIVERS, SI_ORDER_FIRST,
    rk3588_dwmmc_soc_register, NULL);
