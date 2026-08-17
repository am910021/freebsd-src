/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2022 Ruslan Bukin <br@bsdpad.com>
 *
 * This work was supported by Innovate UK project 105694, "Digital Security
 * by Design (DSbD) Technology Platform Prototype".
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
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/cpu.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/module.h>
#include <sys/mutex.h>

#include <dev/extres/clk/clk.h>
#include <dev/fdt/simplebus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include "dev/mailbox/arm/arm_doorbell.h"

#include "scmi.h"
#include "scmi_protocols.h"
#include "scmi_soc.h"

struct scmi_softc {
	struct simplebus_softc	simplebus_sc;
	device_t		dev;
	device_t		tx_shmem;
	struct scmi_soc_shmem	soc_shmem;
	struct arm_doorbell	*db;
	struct intr_config_hook	intr_hook;
	struct mtx		mtx;
	void			*soc_transport;
	bool			use_soc_transport;
	int			initialized;
	int			mtx_initialized;
	int			req_done;
};

static device_t
scmi_get_shmem(struct scmi_softc *sc, int index)
{
	phandle_t *shmems;
	phandle_t shmem_node;
	phandle_t node;
	device_t dev;
	size_t len;

	node = ofw_bus_get_node(sc->dev);
	if (node <= 0)
		return (NULL);

	len = OF_getencprop_alloc_multi(node, "shmem", sizeof(*shmems),
	    (void **)&shmems);
	if (len <= 0) {
		device_printf(sc->dev, "%s: Can't get shmem node.\n", __func__);
		return (NULL);
	}

	if (index >= len) {
		OF_prop_free(shmems);
		return (NULL);
	}

	dev = OF_device_from_xref(shmems[index]);
	shmem_node = OF_node_from_xref(shmems[index]);
	if (dev == NULL && shmem_node > 0 &&
	    scmi_soc_map_shmem(sc->dev, shmem_node, &sc->soc_shmem) != 0)
		device_printf(sc->dev, "%s: Can't get shmem device.\n",
		    __func__);

	OF_prop_free(shmems);

	return (dev);
}

static void
scmi_shmem_read_req(struct scmi_softc *sc, bus_size_t offset, void *buf,
    bus_size_t len)
{
	uint8_t *addr;
	int i;

	if (!scmi_soc_shmem_valid(&sc->soc_shmem)) {
		scmi_shmem_read(sc->tx_shmem, offset, buf, len);
		return;
	}

	addr = (uint8_t *)buf;
	for (i = 0; i < len; i++)
		addr[i] = scmi_soc_shmem_read_1(&sc->soc_shmem, offset + i);
}

static void
scmi_shmem_write_req(struct scmi_softc *sc, bus_size_t offset,
    const void *buf, bus_size_t len)
{
	const uint8_t *addr;
	int i;

	if (!scmi_soc_shmem_valid(&sc->soc_shmem)) {
		scmi_shmem_write(sc->tx_shmem, offset, buf, len);
		return;
	}

	addr = (const uint8_t *)buf;
	for (i = 0; i < len; i++)
		scmi_soc_shmem_write_1(&sc->soc_shmem, offset + i, addr[i]);
}

static void
scmi_callback(void *arg)
{
	struct scmi_softc *sc;

	sc = arg;

	dprintf("%s sc %p\n", __func__, sc);

	SCMI_LOCK(sc);
	sc->req_done = 1;
	wakeup(sc);
	SCMI_UNLOCK(sc);
}

static int
scmi_request_locked(struct scmi_softc *sc, struct scmi_req *req)
{
	struct scmi_smt_header hdr;
	int timeout;

	bzero(&hdr, sizeof(struct scmi_smt_header));

	SCMI_ASSERT_LOCKED(sc);

	/* Read header */
	scmi_shmem_read_req(sc, 0, &hdr, SMT_HEADER_SIZE);

	if ((hdr.channel_status & SCMI_SHMEM_CHAN_STAT_CHANNEL_FREE) == 0)
		return (1);

	/* Update header */
	hdr.channel_status &= ~SCMI_SHMEM_CHAN_STAT_CHANNEL_FREE;
	hdr.msg_header = req->protocol_id << SMT_HEADER_PROTOCOL_ID_S;
	hdr.msg_header |= req->message_id << SMT_HEADER_MESSAGE_ID_S;
	hdr.length = sizeof(hdr.msg_header) + req->in_size;
	hdr.flags |= SCMI_SHMEM_FLAG_INTR_ENABLED;

	/* Write header */
	scmi_shmem_write_req(sc, 0, &hdr, SMT_HEADER_SIZE);

	/* Write request */
	scmi_shmem_write_req(sc, SMT_HEADER_SIZE, req->in_buf,
	    req->in_size);

	sc->req_done = 0;

	if (sc->use_soc_transport) {
		if (scmi_soc_transport_xfer(sc->dev, sc->soc_transport) != 0)
			return (-1);
		goto read_response;
	}

	/* Interrupt SCP firmware. */
	arm_doorbell_set(sc->db);

	timeout = 200;

	dprintf("%s: request\n", __func__);

	do {
		if (cold) {
			if (arm_doorbell_get(sc->db))
				break;
			DELAY(10000);
		} else {
			msleep(sc, &sc->mtx, 0, "scmi", hz / 10);
			if (sc->req_done)
				break;
		}
	} while (timeout--);

	if (timeout <= 0)
		return (-1);

	dprintf("%s: got reply, timeout %d\n", __func__, timeout);

read_response:
	/* Read header. */
	scmi_shmem_read_req(sc, 0, &hdr, SMT_HEADER_SIZE);

	/* Read response */
	scmi_shmem_read_req(sc, SMT_HEADER_SIZE, req->out_buf,
	    req->out_size);

	return (0);
}

int
scmi_request(device_t dev, struct scmi_req *req)
{
	struct scmi_softc *sc;
	int error;

	sc = device_get_softc(dev);

	SCMI_LOCK(sc);
	error = scmi_request_locked(sc, req);
	SCMI_UNLOCK(sc);

	return (error);
}

static int
scmi_probe(device_t dev)
{

	if (!ofw_bus_is_compatible(dev, "arm,scmi") &&
	    !scmi_soc_probe(dev))
		return (ENXIO);

	if (!ofw_bus_status_okay(dev))
		return (ENXIO);

	device_set_desc(dev, "ARM SCMI interface driver");

	return (BUS_PROBE_DEFAULT);
}

static int
scmi_attach_init(device_t dev)
{
	struct scmi_softc *sc;
	phandle_t node;
	int error;

	sc = device_get_softc(dev);
	sc->dev = dev;
	if (sc->initialized)
		return (0);

	node = ofw_bus_get_node(dev);
	if (node == -1)
		return (ENXIO);

	sc->tx_shmem = scmi_get_shmem(sc, 0);
	if (sc->tx_shmem == NULL && !scmi_soc_shmem_valid(&sc->soc_shmem)) {
		if (bootverbose)
			device_printf(dev, "SCMI TX shmem not ready, deferring\n");
		return (EAGAIN);
	}

	if (scmi_soc_has_transport(dev)) {
		error = scmi_soc_transport_init(dev, &sc->soc_transport);
		if (error != 0)
			return (error);
		sc->use_soc_transport = true;
	} else {
		sc->db = arm_doorbell_ofw_get(sc->dev, "tx");
		if (sc->db == NULL) {
			device_printf(dev, "Doorbell device not found.\n");
			return (ENXIO);
		}
	}

	if (!sc->mtx_initialized) {
		mtx_init(&sc->mtx, device_get_nameunit(dev), "SCMI", MTX_DEF);
		sc->mtx_initialized = 1;
	}

	if (!sc->use_soc_transport)
		arm_doorbell_set_handler(sc->db, scmi_callback, sc);

	simplebus_init(dev, node);

	/*
	 * Allow devices to identify.
	 */
	bus_generic_probe(dev);

	/*
	 * Now walk the OFW tree and attach top-level devices.
	 */
	for (node = OF_child(node); node > 0; node = OF_peer(node))
		simplebus_add_device(dev, node, 0, NULL, -1, NULL);

	error = bus_generic_attach(dev);
	if (error == 0)
		sc->initialized = 1;

	return (error);
}

static void
scmi_intrhook(void *arg)
{
	struct scmi_softc *sc;
	int error;

	sc = arg;
	error = scmi_attach_init(sc->dev);
	if (error != 0)
		device_printf(sc->dev,
		    "deferred SCMI attach failed: %d\n", error);
	config_intrhook_disestablish(&sc->intr_hook);
}

static int
scmi_attach(device_t dev)
{
	struct scmi_softc *sc;
	int error;

	sc = device_get_softc(dev);
	sc->dev = dev;

	error = scmi_attach_init(dev);
	if (error != EAGAIN)
		return (error);

	sc->intr_hook.ich_func = scmi_intrhook;
	sc->intr_hook.ich_arg = sc;
	error = config_intrhook_establish(&sc->intr_hook);
	if (error != 0)
		device_printf(dev,
		    "cannot establish SCMI intrhook: %d\n", error);

	return (error);
}

static int
scmi_detach(device_t dev)
{
	struct scmi_softc *sc;

	sc = device_get_softc(dev);
	if (sc->use_soc_transport)
		scmi_soc_transport_fini(dev, sc->soc_transport);
	if (scmi_soc_shmem_valid(&sc->soc_shmem) &&
	    sc->soc_shmem.fini != NULL)
		sc->soc_shmem.fini(sc->soc_shmem.cookie);

	return (0);
}

static device_method_t scmi_methods[] = {
	DEVMETHOD(device_probe,		scmi_probe),
	DEVMETHOD(device_attach,	scmi_attach),
	DEVMETHOD(device_detach,	scmi_detach),
	DEVMETHOD_END
};

DEFINE_CLASS_1(scmi, scmi_driver, scmi_methods, sizeof(struct scmi_softc),
    simplebus_driver);

DRIVER_MODULE(scmi, simplebus, scmi_driver, 0, 0);
EARLY_DRIVER_MODULE(scmi, ofw_firmware, scmi_driver, 0, 0,
    BUS_PASS_BUS + BUS_PASS_ORDER_FIRST);
MODULE_VERSION(scmi, 1);
