/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 */

#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>

#include <dev/firmware/arm/scmi_soc.h>

static const struct scmi_soc_ops *scmi_soc_ops;

void
scmi_soc_register(const struct scmi_soc_ops *ops)
{

	scmi_soc_ops = ops;
}

bool
scmi_soc_probe(device_t dev)
{

	return (scmi_soc_ops != NULL && scmi_soc_ops->probe != NULL &&
	    scmi_soc_ops->probe(dev));
}

int
scmi_soc_map_shmem(device_t dev, phandle_t node,
    struct scmi_soc_shmem *shmem)
{

	if (scmi_soc_ops == NULL || scmi_soc_ops->map_shmem == NULL ||
	    !scmi_soc_probe(dev))
		return (ENXIO);
	return (scmi_soc_ops->map_shmem(dev, node, shmem));
}

bool
scmi_soc_shmem_valid(const struct scmi_soc_shmem *shmem)
{

	return (shmem != NULL && shmem->cookie != NULL &&
	    shmem->read_1 != NULL && shmem->write_1 != NULL);
}

uint8_t
scmi_soc_shmem_read_1(const struct scmi_soc_shmem *shmem,
    bus_size_t offset)
{

	return (shmem->read_1(shmem->cookie, offset));
}

void
scmi_soc_shmem_write_1(const struct scmi_soc_shmem *shmem,
    bus_size_t offset, uint8_t val)
{

	shmem->write_1(shmem->cookie, offset, val);
}

bool
scmi_soc_has_transport(device_t dev)
{

	return (scmi_soc_ops != NULL && scmi_soc_ops->has_transport != NULL &&
	    scmi_soc_probe(dev) && scmi_soc_ops->has_transport(dev));
}

int
scmi_soc_transport_init(device_t dev, void **cookie)
{

	if (!scmi_soc_has_transport(dev) ||
	    scmi_soc_ops->transport_init == NULL)
		return (ENXIO);
	return (scmi_soc_ops->transport_init(dev, cookie));
}

int
scmi_soc_transport_xfer(device_t dev, void *cookie)
{

	if (scmi_soc_ops == NULL || scmi_soc_ops->transport_xfer == NULL ||
	    !scmi_soc_probe(dev))
		return (ENXIO);
	return (scmi_soc_ops->transport_xfer(dev, cookie));
}

void
scmi_soc_transport_fini(device_t dev, void *cookie)
{

	if (scmi_soc_ops != NULL && scmi_soc_ops->transport_fini != NULL &&
	    scmi_soc_probe(dev))
		scmi_soc_ops->transport_fini(dev, cookie);
}
