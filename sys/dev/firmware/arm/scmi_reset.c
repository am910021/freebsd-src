/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 *
 * This software was developed as part of ARM SCMI reset support.
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
#include <sys/endian.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/systm.h>

#include <dev/extres/hwreset/hwreset.h>
#include <dev/ofw/ofw_bus_subr.h>

#include "scmi.h"
#include "scmi_protocols.h"

#include "hwreset_if.h"

#define	SCMI_RESET_DOMAIN_ATTRIBUTES	0x3
#define	SCMI_RESET			0x4

#define	SCMI_RESET_NUM_DOMAINS_MASK	0xffff

#define	SCMI_RESET_FLAG_EXPLICIT_ASSERT	(1u << 1)
#define	SCMI_RESET_ARCH_COLD		0

struct scmi_reset_softc {
	device_t	dev;
	device_t	scmi;
	uint32_t	ndomains;
};

struct scmi_reset_protocol_attrs_out {
	int32_t		status;
	uint32_t	attributes;
};

struct scmi_reset_domain_attrs_in {
	uint32_t	domain_id;
};

struct scmi_reset_domain_attrs_out {
	int32_t		status;
	uint32_t	attributes;
	uint32_t	latency;
	uint8_t		name[16];
};

struct scmi_reset_in {
	uint32_t	domain_id;
	uint32_t	flags;
	uint32_t	reset_state;
};

struct scmi_reset_out {
	int32_t		status;
};

static int
scmi_reset_status_to_errno(int32_t status)
{

	switch (status) {
	case SCMI_SUCCESS:
		return (0);
	case SCMI_NOT_SUPPORTED:
		return (EOPNOTSUPP);
	case SCMI_INVALID_PARAMETERS:
		return (EINVAL);
	case SCMI_DENIED:
		return (EPERM);
	case SCMI_NOT_FOUND:
		return (ENOENT);
	case SCMI_OUT_OF_RANGE:
		return (ERANGE);
	case SCMI_BUSY:
		return (EBUSY);
	case SCMI_COMMS_ERROR:
	case SCMI_PROTOCOL_ERROR:
		return (EIO);
	case SCMI_HARDWARE_ERROR:
		return (EFAULT);
	default:
		return (ENXIO);
	}
}

static int
scmi_reset_discover(struct scmi_reset_softc *sc)
{
	struct scmi_reset_protocol_attrs_out out;
	struct scmi_req req;
	int error;

	req.protocol_id = SCMI_PROTOCOL_ID_RESET_DOMAIN;
	req.message_id = SCMI_PROTOCOL_ATTRIBUTES;
	req.in_buf = NULL;
	req.in_size = 0;
	req.out_buf = &out;
	req.out_size = sizeof(out);

	error = scmi_request(sc->scmi, &req);
	if (error != 0)
		return (error);

	error = scmi_reset_status_to_errno((int32_t)le32toh(out.status));
	if (error != 0)
		return (error);

	sc->ndomains = le32toh(out.attributes) & SCMI_RESET_NUM_DOMAINS_MASK;
	if (bootverbose)
		device_printf(sc->dev, "found %u reset domains\n",
		    sc->ndomains);

	return (0);
}

static int
scmi_reset_get_domain_attrs(struct scmi_reset_softc *sc, uint32_t domain,
    char *name, size_t name_len, uint32_t *latency)
{
	struct scmi_reset_domain_attrs_out out;
	struct scmi_reset_domain_attrs_in in;
	struct scmi_req req;
	int error;

	req.protocol_id = SCMI_PROTOCOL_ID_RESET_DOMAIN;
	req.message_id = SCMI_RESET_DOMAIN_ATTRIBUTES;
	req.in_buf = &in;
	req.in_size = sizeof(in);
	req.out_buf = &out;
	req.out_size = sizeof(out);

	in.domain_id = htole32(domain);

	error = scmi_request(sc->scmi, &req);
	if (error != 0)
		return (error);

	error = scmi_reset_status_to_errno((int32_t)le32toh(out.status));
	if (error != 0)
		return (error);

	if (latency != NULL)
		*latency = le32toh(out.latency);
	if (name != NULL && name_len > 0) {
		memset(name, 0, name_len);
		memcpy(name, out.name, MIN(name_len - 1, sizeof(out.name)));
	}

	return (0);
}

static int
scmi_reset_assert(device_t dev, intptr_t id, bool assert)
{
	struct scmi_reset_softc *sc;
	struct scmi_reset_out out;
	struct scmi_reset_in in;
	struct scmi_req req;
	int32_t status;
	int error;

	sc = device_get_softc(dev);
	if (id < 0 || (uint32_t)id >= sc->ndomains)
		return (ERANGE);

	req.protocol_id = SCMI_PROTOCOL_ID_RESET_DOMAIN;
	req.message_id = SCMI_RESET;
	req.in_buf = &in;
	req.in_size = sizeof(in);
	req.out_buf = &out;
	req.out_size = sizeof(out);

	in.domain_id = htole32((uint32_t)id);
	in.flags = htole32(assert ? SCMI_RESET_FLAG_EXPLICIT_ASSERT : 0);
	in.reset_state = htole32(SCMI_RESET_ARCH_COLD);

	error = scmi_request(sc->scmi, &req);
	if (error != 0)
		return (error);

	status = (int32_t)le32toh(out.status);
	error = scmi_reset_status_to_errno(status);
	if (error != 0) {
		device_printf(dev,
		    "reset domain %jd assert=%d failed status=%d error=%d\n",
		    (intmax_t)id, assert ? 1 : 0, status, error);
	}

	return (error);
}

static int
scmi_reset_is_asserted(device_t dev, intptr_t id, bool *value)
{

	return (EOPNOTSUPP);
}

static int
scmi_reset_probe(device_t dev)
{
	phandle_t node;
	uint32_t reg;
	int error;

	node = ofw_bus_get_node(dev);

	error = OF_getencprop(node, "reg", &reg, sizeof(reg));
	if (error < 0)
		return (ENXIO);

	if (reg != SCMI_PROTOCOL_ID_RESET_DOMAIN)
		return (ENXIO);

	device_set_desc(dev, "SCMI Reset Domain Unit");

	return (BUS_PROBE_DEFAULT);
}

static int
scmi_reset_attach(device_t dev)
{
	struct scmi_reset_softc *sc;
	char name[17];
	uint32_t latency;
	phandle_t node;
	int error;
	int i;

	sc = device_get_softc(dev);
	sc->dev = dev;
	sc->scmi = device_get_parent(dev);

	node = ofw_bus_get_node(dev);
	OF_device_register_xref(OF_xref_from_node(node), dev);

	error = scmi_reset_discover(sc);
	if (error != 0) {
		device_printf(dev, "Could not discover SCMI resets: %d\n",
		    error);
		return (error);
	}

	for (i = 0; i < MIN(sc->ndomains, 8); i++) {
		error = scmi_reset_get_domain_attrs(sc, i, name, sizeof(name),
		    &latency);
		if (error == 0 && bootverbose) {
			device_printf(dev,
			    "reset[%d] name='%s' latency=%u\n",
			    i, name, latency);
		}
	}

	hwreset_register_ofw_provider(dev);

	return (0);
}

static int
scmi_reset_detach(device_t dev)
{

	hwreset_unregister_ofw_provider(dev);

	return (0);
}

static device_method_t scmi_reset_methods[] = {
	DEVMETHOD(device_probe,		scmi_reset_probe),
	DEVMETHOD(device_attach,	scmi_reset_attach),
	DEVMETHOD(device_detach,	scmi_reset_detach),

	DEVMETHOD(hwreset_assert,	scmi_reset_assert),
	DEVMETHOD(hwreset_is_asserted,	scmi_reset_is_asserted),

	DEVMETHOD_END
};

static driver_t scmi_reset_driver = {
	"scmi_reset",
	scmi_reset_methods,
	sizeof(struct scmi_reset_softc),
};

EARLY_DRIVER_MODULE(scmi_reset, scmi, scmi_reset_driver, 0, 0,
    BUS_PASS_INTERRUPT + BUS_PASS_ORDER_MIDDLE);
MODULE_VERSION(scmi_reset, 1);
