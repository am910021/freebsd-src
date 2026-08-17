/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 *
 * This software was developed for RK3588 platform support.
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
#include <sys/param.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/rman.h>
#include <sys/stdatomic.h>
#include <sys/systm.h>

#include <machine/bus.h>

#include <dev/extres/clk/clk.h>
#include <dev/extres/hwreset/hwreset.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#include <dev/random/randomdev.h>

#define	RK3588_RNG_MAX_BYTE		32
#define	RK3588_RNG_POLL_DELAY_US	100
#define	RK3588_RNG_POLL_TIMEOUT_US	10000
#define	RK3588_TRNG_AUTO_RESEED_CNT	16000

#define	TRNG_V1_CTRL			0x0000
#define	 TRNG_V1_CTRL_NOP		0x00
#define	 TRNG_V1_CTRL_RAND		0x01

#define	TRNG_V1_STAT			0x0004
#define	 TRNG_V1_STAT_SEEDED		(1u << 9)
#define	 TRNG_V1_STAT_GENERATING	(1u << 30)
#define	 TRNG_V1_STAT_RESEEDING		(1u << 31)

#define	TRNG_V1_MODE			0x0008
#define	 TRNG_V1_MODE_256_BIT		(0x01 << 3)

#define	TRNG_V1_ISTAT			0x0014
#define	 TRNG_V1_ISTAT_RAND_RDY		(1u << 0)

#define	TRNG_V1_RAND0			0x0020
#define	TRNG_V1_AUTO_RQSTS		0x0060
#define	TRNG_V1_VERSION			0x00f0
#define	TRNG_V1_VERSION_CODE		0x46bc

struct rk3588_rng_softc {
	device_t		dev;
	struct resource		*res;
	clk_t			clk;
	hwreset_array_t		resets;
	struct mtx		mtx;
};

static unsigned rk3588_rng_read(void *, unsigned);

static struct random_source random_rk3588_rng = {
	.rs_ident = "Rockchip RK3588 TRNG",
	.rs_source = RANDOM_PURE_ROCKCHIP,
	.rs_read = rk3588_rng_read,
};

static _Atomic(struct rk3588_rng_softc *) g_rk3588_rng_softc;

static inline uint32_t
rk3588_rng_read_4(struct rk3588_rng_softc *sc, bus_size_t off)
{

	return (bus_read_4(sc->res, off));
}

static inline void
rk3588_rng_write_4(struct rk3588_rng_softc *sc, bus_size_t off, uint32_t val)
{

	bus_write_4(sc->res, off, val);
}

static int
rk3588_rng_poll(struct rk3588_rng_softc *sc, bus_size_t off, uint32_t mask,
    uint32_t value)
{
	uint32_t reg;
	int i;

	for (i = 0; i < RK3588_RNG_POLL_TIMEOUT_US /
	    RK3588_RNG_POLL_DELAY_US; i++) {
		reg = rk3588_rng_read_4(sc, off);
		if ((reg & mask) == value)
			return (0);
		DELAY(RK3588_RNG_POLL_DELAY_US);
	}

	return (ETIMEDOUT);
}

static int
rk3588_rng_hw_init(struct rk3588_rng_softc *sc)
{
	uint32_t istat;
	uint32_t mask;
	uint32_t version;
	int error;

	error = clk_enable(sc->clk);
	if (error != 0) {
		device_printf(sc->dev, "Cannot enable clock: %d\n", error);
		return (error);
	}

	version = rk3588_rng_read_4(sc, TRNG_V1_VERSION);
	if (version != TRNG_V1_VERSION_CODE) {
		device_printf(sc->dev,
		    "Unexpected TRNG version %#x, expected %#x\n",
		    version, TRNG_V1_VERSION_CODE);
		clk_disable(sc->clk);
		return (ENXIO);
	}

	mask = TRNG_V1_STAT_SEEDED | TRNG_V1_STAT_GENERATING |
	    TRNG_V1_STAT_RESEEDING;
	error = rk3588_rng_poll(sc, TRNG_V1_STAT, mask, TRNG_V1_STAT_SEEDED);
	if (error != 0) {
		device_printf(sc->dev, "Timed out waiting for seeded TRNG\n");
		clk_disable(sc->clk);
		return (error);
	}

	istat = rk3588_rng_read_4(sc, TRNG_V1_ISTAT);
	rk3588_rng_write_4(sc, TRNG_V1_ISTAT, istat);
	rk3588_rng_write_4(sc, TRNG_V1_AUTO_RQSTS,
	    RK3588_TRNG_AUTO_RESEED_CNT / 16);

	if (bootverbose)
		device_printf(sc->dev, "TRNG ready version=%#x istat=%#x\n",
		    version, istat);

	return (0);
}

static unsigned
rk3588_rng_read(void *buf, unsigned count)
{
	struct rk3588_rng_softc *sc;
	uint8_t tmp[RK3588_RNG_MAX_BYTE];
	unsigned done;
	unsigned n;
	uint32_t istat;
	int error;
	int i;

	sc = atomic_load_explicit(&g_rk3588_rng_softc, memory_order_acquire);
	if (sc == NULL)
		return (0);

	done = 0;
	mtx_lock(&sc->mtx);
	while (done < count) {
		istat = rk3588_rng_read_4(sc, TRNG_V1_ISTAT);
		rk3588_rng_write_4(sc, TRNG_V1_ISTAT, istat);
		rk3588_rng_write_4(sc, TRNG_V1_MODE, TRNG_V1_MODE_256_BIT);
		rk3588_rng_write_4(sc, TRNG_V1_CTRL, TRNG_V1_CTRL_RAND);

		error = rk3588_rng_poll(sc, TRNG_V1_ISTAT,
		    TRNG_V1_ISTAT_RAND_RDY, TRNG_V1_ISTAT_RAND_RDY);
		if (error != 0)
			break;

		for (i = 0; i < RK3588_RNG_MAX_BYTE / sizeof(uint32_t); i++) {
			uint32_t val;

			val = rk3588_rng_read_4(sc, TRNG_V1_RAND0 +
			    i * sizeof(uint32_t));
			memcpy(tmp + i * sizeof(uint32_t), &val, sizeof(val));
		}

		n = MIN(count - done, RK3588_RNG_MAX_BYTE);
		memcpy((uint8_t *)buf + done, tmp, n);
		done += n;

		istat = rk3588_rng_read_4(sc, TRNG_V1_ISTAT);
		rk3588_rng_write_4(sc, TRNG_V1_ISTAT, istat);
		rk3588_rng_write_4(sc, TRNG_V1_CTRL, TRNG_V1_CTRL_NOP);
	}
	mtx_unlock(&sc->mtx);

	return (done);
}

static int
rk3588_rng_probe(device_t dev)
{

	if (!ofw_bus_status_okay(dev))
		return (ENXIO);

	if (!ofw_bus_is_compatible(dev, "rockchip,rk3588-rng"))
		return (ENXIO);

	device_set_desc(dev, "Rockchip RK3588 True Random Number Generator");

	return (BUS_PROBE_DEFAULT);
}

static int
rk3588_rng_attach(device_t dev)
{
	struct rk3588_rng_softc *sc;
	struct rk3588_rng_softc *exp;
	phandle_t node;
	int error;
	int rid;

	sc = device_get_softc(dev);
	sc->dev = dev;
	node = ofw_bus_get_node(dev);

	exp = NULL;
	if (!atomic_compare_exchange_strong_explicit(&g_rk3588_rng_softc,
	    &exp, sc, memory_order_release, memory_order_acquire))
		return (ENXIO);

	rid = 0;
	sc->res = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid, RF_ACTIVE);
	if (sc->res == NULL) {
		device_printf(dev, "Cannot allocate memory resource\n");
		error = ENXIO;
		goto fail;
	}

	error = clk_get_by_ofw_index(dev, node, 0, &sc->clk);
	if (error != 0) {
		device_printf(dev, "Cannot get SCMI clock: %d\n", error);
		goto fail;
	}

	error = hwreset_array_get_ofw(dev, node, &sc->resets);
	if (error == 0) {
		hwreset_array_assert(sc->resets);
		DELAY(2);
		hwreset_array_deassert(sc->resets);
	} else {
		if (bootverbose)
			device_printf(dev, "optional reset unavailable: %d\n",
			    error);
		sc->resets = NULL;
	}

	mtx_init(&sc->mtx, device_get_nameunit(dev), "rk3588_rng", MTX_DEF);

	error = rk3588_rng_hw_init(sc);
	if (error != 0)
		goto fail_mtx;

	random_source_register(&random_rk3588_rng);

	return (0);

fail_mtx:
	mtx_destroy(&sc->mtx);
fail:
	if (sc->resets != NULL)
		hwreset_array_release(sc->resets);
	if (sc->clk != NULL)
		clk_release(sc->clk);
	if (sc->res != NULL)
		bus_release_resource(dev, SYS_RES_MEMORY, 0, sc->res);
	atomic_store_explicit(&g_rk3588_rng_softc, NULL, memory_order_release);
	return (error);
}

static int
rk3588_rng_detach(device_t dev)
{
	struct rk3588_rng_softc *sc;

	sc = device_get_softc(dev);
	random_source_deregister(&random_rk3588_rng);
	atomic_store_explicit(&g_rk3588_rng_softc, NULL, memory_order_release);
	mtx_destroy(&sc->mtx);
	if (sc->resets != NULL)
		hwreset_array_release(sc->resets);
	if (sc->clk != NULL)
		clk_release(sc->clk);
	if (sc->res != NULL)
		bus_release_resource(dev, SYS_RES_MEMORY, 0, sc->res);

	return (0);
}

static device_method_t rk3588_rng_methods[] = {
	DEVMETHOD(device_probe,		rk3588_rng_probe),
	DEVMETHOD(device_attach,	rk3588_rng_attach),
	DEVMETHOD(device_detach,	rk3588_rng_detach),

	DEVMETHOD_END
};

static driver_t rk3588_rng_driver = {
	"rk3588_rng",
	rk3588_rng_methods,
	sizeof(struct rk3588_rng_softc),
};

EARLY_DRIVER_MODULE(rk3588_rng, simplebus, rk3588_rng_driver, 0, 0,
    BUS_PASS_DEFAULT);
MODULE_VERSION(rk3588_rng, 1);
MODULE_DEPEND(rk3588_rng, random_harvestq, 1, 1, 1);
