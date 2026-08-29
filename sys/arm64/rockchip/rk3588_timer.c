/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/rman.h>
#include <sys/systm.h>
#include <sys/timeet.h>

#include <machine/bus.h>

#include <dev/extres/clk/clk.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#define RK_TIMER_LOAD_COUNT0		0x00
#define RK_TIMER_LOAD_COUNT1		0x04
#define RK_TIMER_CONTROL		0x10
#define  RK_TIMER_ENABLE		(1u << 0)
#define  RK_TIMER_USER_DEFINED		(1u << 1)
#define  RK_TIMER_INTERRUPT_ENABLE	(1u << 2)
#define RK_TIMER_INTERRUPT_STATUS	0x18

struct rk3588_timer_softc {
	struct resource		*res[2];
	void			*ih;
	clk_t			pclk;
	clk_t			timer_clk;
	struct eventtimer	et;
	uint32_t		period;
	bool			periodic;
	bool			reload_periodic;
};

enum {
	RK_TIMER_MEM,
	RK_TIMER_IRQ,
};

static struct resource_spec rk3588_timer_spec[] = {
	{ SYS_RES_MEMORY, 0, RF_ACTIVE },
	{ SYS_RES_IRQ, 0, RF_ACTIVE },
	{ -1, 0, 0 },
};

static uint32_t
rk3588_timer_count(struct eventtimer *et, sbintime_t interval)
{
	uint64_t count;

	count = (et->et_frequency * interval) >> 32;
	return ((uint32_t)MAX(count, 1));
}

static void
rk3588_timer_program(struct rk3588_timer_softc *sc, uint32_t count,
    uint32_t mode)
{

	bus_write_4(sc->res[RK_TIMER_MEM], RK_TIMER_CONTROL, 0);
	bus_write_4(sc->res[RK_TIMER_MEM], RK_TIMER_LOAD_COUNT0, count);
	bus_write_4(sc->res[RK_TIMER_MEM], RK_TIMER_LOAD_COUNT1, 0);
	bus_write_4(sc->res[RK_TIMER_MEM], RK_TIMER_INTERRUPT_STATUS, 1);
	bus_write_4(sc->res[RK_TIMER_MEM], RK_TIMER_CONTROL,
	    RK_TIMER_ENABLE | RK_TIMER_INTERRUPT_ENABLE | mode);
}

static int
rk3588_timer_start(struct eventtimer *et, sbintime_t first,
    sbintime_t period)
{
	struct rk3588_timer_softc *sc;
	uint32_t count;

	sc = et->et_priv;
	if (first == 0 && period == 0)
		return (EINVAL);

	sc->periodic = period != 0;
	if (sc->periodic)
		sc->period = rk3588_timer_count(et, period);
	count = rk3588_timer_count(et, first != 0 ? first : period);
	sc->reload_periodic = sc->periodic && count != sc->period;
	rk3588_timer_program(sc, count,
	    sc->periodic && !sc->reload_periodic ? 0 : RK_TIMER_USER_DEFINED);

	return (0);
}

static int
rk3588_timer_stop(struct eventtimer *et)
{
	struct rk3588_timer_softc *sc;

	sc = et->et_priv;
	sc->periodic = false;
	sc->reload_periodic = false;
	bus_write_4(sc->res[RK_TIMER_MEM], RK_TIMER_CONTROL, 0);
	bus_write_4(sc->res[RK_TIMER_MEM], RK_TIMER_INTERRUPT_STATUS, 1);

	return (0);
}

static int
rk3588_timer_intr(void *arg)
{
	struct rk3588_timer_softc *sc;

	sc = arg;
	if ((bus_read_4(sc->res[RK_TIMER_MEM], RK_TIMER_INTERRUPT_STATUS) & 1) ==
	    0)
		return (FILTER_STRAY);

	bus_write_4(sc->res[RK_TIMER_MEM], RK_TIMER_INTERRUPT_STATUS, 1);
	if (!sc->periodic)
		bus_write_4(sc->res[RK_TIMER_MEM], RK_TIMER_CONTROL, 0);
	else if (sc->reload_periodic) {
		sc->reload_periodic = false;
		rk3588_timer_program(sc, sc->period, 0);
	}

	if (sc->et.et_active)
		sc->et.et_event_cb(&sc->et, sc->et.et_arg);

	return (FILTER_HANDLED);
}

static int
rk3588_timer_probe(device_t dev)
{

	if (!ofw_bus_status_okay(dev) ||
	    !ofw_bus_is_compatible(dev, "rockchip,rk3588-timer"))
		return (ENXIO);

	device_set_desc(dev, "RK3588 Bus Timer");
	return (BUS_PROBE_DEFAULT);
}

static int
rk3588_timer_attach(device_t dev)
{
	struct rk3588_timer_softc *sc;
	uint64_t frequency;
	int error;

	sc = device_get_softc(dev);

	error = bus_alloc_resources(dev, rk3588_timer_spec, sc->res);
	if (error != 0)
		return (error);

	error = clk_get_by_ofw_name(dev, 0, "pclk", &sc->pclk);
	if (error != 0)
		goto fail_resources;
	error = clk_enable(sc->pclk);
	if (error != 0)
		goto fail_release_pclk;

	error = clk_get_by_ofw_name(dev, 0, "timer", &sc->timer_clk);
	if (error != 0)
		goto fail_disable_pclk;
	error = clk_enable(sc->timer_clk);
	if (error != 0)
		goto fail_release_timer_clk;
	error = clk_get_freq(sc->timer_clk, &frequency);
	if (error != 0 || frequency == 0) {
		if (error == 0)
			error = EINVAL;
		goto fail_disable_timer_clk;
	}

	bus_write_4(sc->res[RK_TIMER_MEM], RK_TIMER_CONTROL, 0);
	bus_write_4(sc->res[RK_TIMER_MEM], RK_TIMER_INTERRUPT_STATUS, 1);
	error = bus_setup_intr(dev, sc->res[RK_TIMER_IRQ], INTR_TYPE_CLK,
	    rk3588_timer_intr, NULL, sc, &sc->ih);
	if (error != 0)
		goto fail_disable_timer_clk;

	sc->et.et_name = "RK3588 Bus Timer";
	sc->et.et_flags = ET_FLAGS_PERIODIC | ET_FLAGS_ONESHOT;
	sc->et.et_quality = 500;
	sc->et.et_frequency = frequency;
	sc->et.et_min_period = (2LLU << 32) / frequency;
	sc->et.et_max_period = (0xfffffffeLLU << 32) / frequency;
	sc->et.et_start = rk3588_timer_start;
	sc->et.et_stop = rk3588_timer_stop;
	sc->et.et_priv = sc;
	et_register(&sc->et);

	return (0);

fail_disable_timer_clk:
	clk_disable(sc->timer_clk);
fail_release_timer_clk:
	clk_release(sc->timer_clk);
fail_disable_pclk:
	clk_disable(sc->pclk);
fail_release_pclk:
	clk_release(sc->pclk);
fail_resources:
	device_printf(dev, "attach failed: %d\n", error);
	bus_release_resources(dev, rk3588_timer_spec, sc->res);
	return (error);
}

static device_method_t rk3588_timer_methods[] = {
	DEVMETHOD(device_probe,		rk3588_timer_probe),
	DEVMETHOD(device_attach,	rk3588_timer_attach),

	DEVMETHOD_END
};

static driver_t rk3588_timer_driver = {
	"rk3588_timer",
	rk3588_timer_methods,
	sizeof(struct rk3588_timer_softc),
};

EARLY_DRIVER_MODULE(rk3588_timer, simplebus, rk3588_timer_driver, 0, 0,
    BUS_PASS_TIMER + BUS_PASS_ORDER_LAST);
