/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 */

#ifndef _DEV_FIRMWARE_ARM_SCMI_SOC_H_
#define	_DEV_FIRMWARE_ARM_SCMI_SOC_H_

#include <sys/param.h>
#include <sys/bus.h>

#include <dev/ofw/openfirm.h>

struct scmi_soc_shmem {
	void		*cookie;
	uint8_t		(*read_1)(void *, bus_size_t);
	void		(*write_1)(void *, bus_size_t, uint8_t);
	void		(*fini)(void *);
};

struct scmi_soc_ops {
	bool	(*probe)(device_t);
	int	(*map_shmem)(device_t, phandle_t, struct scmi_soc_shmem *);
	bool	(*has_transport)(device_t);
	int	(*transport_init)(device_t, void **);
	int	(*transport_xfer)(device_t, void *);
	void	(*transport_fini)(device_t, void *);
};

void	scmi_soc_register(const struct scmi_soc_ops *);
bool	scmi_soc_probe(device_t);
int	scmi_soc_map_shmem(device_t, phandle_t, struct scmi_soc_shmem *);
bool	scmi_soc_shmem_valid(const struct scmi_soc_shmem *);
uint8_t	scmi_soc_shmem_read_1(const struct scmi_soc_shmem *, bus_size_t);
void	scmi_soc_shmem_write_1(const struct scmi_soc_shmem *, bus_size_t,
	    uint8_t);
bool	scmi_soc_has_transport(device_t);
int	scmi_soc_transport_init(device_t, void **);
int	scmi_soc_transport_xfer(device_t, void *);
void	scmi_soc_transport_fini(device_t, void *);

#endif /* _DEV_FIRMWARE_ARM_SCMI_SOC_H_ */
