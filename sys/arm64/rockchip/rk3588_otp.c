/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 */

#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/rman.h>
#include <sys/systm.h>

#include <machine/bus.h>

#include <dev/extres/clk/clk.h>
#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include "nvmem_if.h"

#define	RK3588_OTP_SIZE			0x400
#define	RK3588_OTP_NS_WORD_OFFSET	0x300
#define	RK3588_OTP_WORD_SIZE		4
#define	RK3588_OTP_NCLOCKS		4

#define	RK3588_OTPC_AUTO_CTRL		0x04
#define	 RK3588_OTPC_BURST_NUM		(1u << 8)
#define	 RK3588_OTPC_ADDR_SHIFT	16
#define	RK3588_OTPC_AUTO_EN		0x08
#define	 RK3588_OTPC_AUTO_READ		(1u << 0)
#define	RK3588_OTPC_DOUT0		0x20
#define	RK3588_OTPC_INT_ST		0x84
#define	 RK3588_OTPC_READ_DONE		(1u << 1)

#define	RK3588_OTPC_TIMEOUT_US		10000

struct rk3588_otp_softc {
	device_t		dev;
	struct resource		*mem;
	clk_t			clocks[RK3588_OTP_NCLOCKS];
	int			nclocks;
	struct mtx		mtx;
};

static int
rk3588_otp_clocks_enable(struct rk3588_otp_softc *sc)
{
	int error;
	int i;

	for (i = 0; i < sc->nclocks; i++) {
		error = clk_enable(sc->clocks[i]);
		if (error != 0) {
			while (i-- > 0)
				clk_disable(sc->clocks[i]);
			return (error);
		}
	}
	return (0);
}

static void
rk3588_otp_clocks_disable(struct rk3588_otp_softc *sc)
{
	int i;

	for (i = sc->nclocks - 1; i >= 0; i--)
		clk_disable(sc->clocks[i]);
}

static int
rk3588_otp_read_word(struct rk3588_otp_softc *sc, uint32_t addr,
    uint32_t *value)
{
	int retry;

	/* These writes only start a read and acknowledge its status. */
	bus_write_4(sc->mem, RK3588_OTPC_AUTO_CTRL,
	    (addr << RK3588_OTPC_ADDR_SHIFT) | RK3588_OTPC_BURST_NUM);
	bus_write_4(sc->mem, RK3588_OTPC_AUTO_EN, RK3588_OTPC_AUTO_READ);

	for (retry = 0; retry < RK3588_OTPC_TIMEOUT_US; retry++) {
		if ((bus_read_4(sc->mem, RK3588_OTPC_INT_ST) &
		    RK3588_OTPC_READ_DONE) != 0)
			break;
		DELAY(1);
	}
	if (retry == RK3588_OTPC_TIMEOUT_US)
		return (ETIMEDOUT);

	bus_write_4(sc->mem, RK3588_OTPC_INT_ST, RK3588_OTPC_READ_DONE);
	*value = bus_read_4(sc->mem, RK3588_OTPC_DOUT0);
	return (0);
}

static int
rk3588_otp_read(device_t dev, uint32_t offset, uint32_t size,
    uint8_t *buffer)
{
	struct rk3588_otp_softc *sc;
	uint32_t value;
	uint32_t chunk;
	uint32_t in_word;
	int error;

	if (offset > RK3588_OTP_SIZE || size > RK3588_OTP_SIZE - offset)
		return (EINVAL);
	if (size == 0)
		return (0);

	sc = device_get_softc(dev);
	mtx_lock(&sc->mtx);
	error = rk3588_otp_clocks_enable(sc);
	if (error != 0)
		goto out;

	while (size != 0) {
		in_word = offset % RK3588_OTP_WORD_SIZE;
		error = rk3588_otp_read_word(sc,
		    RK3588_OTP_NS_WORD_OFFSET + offset / RK3588_OTP_WORD_SIZE,
		    &value);
		if (error != 0)
			break;
		chunk = MIN(size, RK3588_OTP_WORD_SIZE - in_word);
		memcpy(buffer, (uint8_t *)&value + in_word, chunk);
		buffer += chunk;
		offset += chunk;
		size -= chunk;
	}

	rk3588_otp_clocks_disable(sc);
out:
	mtx_unlock(&sc->mtx);
	return (error);
}

static int
rk3588_otp_probe(device_t dev)
{

	if (!ofw_bus_status_okay(dev) ||
	    !ofw_bus_is_compatible(dev, "rockchip,rk3588-otp"))
		return (ENXIO);

	device_set_desc(dev, "Rockchip RK3588 read-only OTP");
	return (BUS_PROBE_DEFAULT);
}

static int
rk3588_otp_attach(device_t dev)
{
	struct rk3588_otp_softc *sc;
	phandle_t node;
	uint8_t cpu_code[2];
	int error;
	int i;
	int rid;

	sc = device_get_softc(dev);
	sc->dev = dev;
	node = ofw_bus_get_node(dev);

	rid = 0;
	sc->mem = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid, RF_ACTIVE);
	if (sc->mem == NULL)
		return (ENXIO);

	for (i = 0; i < RK3588_OTP_NCLOCKS; i++) {
		error = clk_get_by_ofw_index(dev, node, i, &sc->clocks[i]);
		if (error != 0)
			break;
		sc->nclocks++;
	}
	if (sc->nclocks < 3) {
		device_printf(dev, "Cannot get required clocks\n");
		error = ENXIO;
		goto fail;
	}

	mtx_init(&sc->mtx, device_get_nameunit(dev), "rk3588_otp", MTX_DEF);
	error = rk3588_otp_read(dev, 2, sizeof(cpu_code), cpu_code);
	if (error != 0) {
		device_printf(dev, "CPU code read failed: %d\n", error);
		mtx_destroy(&sc->mtx);
		goto fail;
	}

	OF_device_register_xref(OF_xref_from_node(node), dev);
	device_printf(dev, "read-only NVMEM ready, cpu-code=%02x%02x\n",
	    cpu_code[0], cpu_code[1]);
	return (0);

fail:
	for (i = 0; i < sc->nclocks; i++)
		clk_release(sc->clocks[i]);
	bus_release_resource(dev, SYS_RES_MEMORY, rid, sc->mem);
	return (error);
}

static device_method_t rk3588_otp_methods[] = {
	DEVMETHOD(device_probe,		rk3588_otp_probe),
	DEVMETHOD(device_attach,	rk3588_otp_attach),
	DEVMETHOD(nvmem_read,		rk3588_otp_read),

	DEVMETHOD_END
};

static driver_t rk3588_otp_driver = {
	"rk3588_otp",
	rk3588_otp_methods,
	sizeof(struct rk3588_otp_softc),
};

EARLY_DRIVER_MODULE(rk3588_otp, simplebus, rk3588_otp_driver, 0, 0,
	BUS_PASS_SUPPORTDEV + BUS_PASS_ORDER_MIDDLE);
MODULE_VERSION(rk3588_otp, 1);
