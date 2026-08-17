/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 */

#ifndef _DEV_SDHCI_SDHCI_FDT_SOC_H_
#define	_DEV_SDHCI_SDHCI_FDT_SOC_H_

#include <sys/types.h>
#include <sys/bus.h>

#include <machine/stdarg.h>

struct sdhci_slot;

struct sdhci_fdt_soc_ops {
	bool	(*match)(device_t);
	bool	(*probe)(device_t);
	void	(*probe_setup)(device_t, u_int *);
	void	(*post_fdt_parse)(device_t, u_int *, bool *);
	bool	(*uses_core_clock)(device_t);
	bool	(*uses_controller_resets)(device_t);
	void	(*clock_bringup)(device_t);
	void	(*init_slot)(device_t, struct sdhci_slot *);
	bool	(*enable_high_speed_caps)(device_t);
	uint32_t (*filter_caps2)(device_t, uint32_t);
	bool	(*set_clock)(device_t, struct sdhci_slot *, int *);
	void	(*post_reset)(device_t, struct sdhci_slot *, uint8_t);
	void	(*vdiag)(device_t, const char *, va_list);
};

void	sdhci_fdt_soc_register(const struct sdhci_fdt_soc_ops *);
bool	sdhci_fdt_soc_probe(device_t);
void	sdhci_fdt_soc_probe_setup(device_t, u_int *);
void	sdhci_fdt_soc_post_fdt_parse(device_t, u_int *, bool *);
bool	sdhci_fdt_soc_uses_core_clock(device_t);
bool	sdhci_fdt_soc_uses_controller_resets(device_t);
void	sdhci_fdt_soc_clock_bringup(device_t);
void	sdhci_fdt_soc_init_slot(device_t, struct sdhci_slot *);
bool	sdhci_fdt_soc_enable_high_speed_caps(device_t);
uint32_t sdhci_fdt_soc_filter_caps2(device_t, uint32_t);
bool	sdhci_fdt_soc_set_clock(device_t, struct sdhci_slot *, int *);
void	sdhci_fdt_soc_post_reset(device_t, struct sdhci_slot *, uint8_t);
void	sdhci_fdt_soc_diag(device_t, const char *, ...);

uint16_t sdhci_fdt_slot_read_2(device_t, struct sdhci_slot *, bus_size_t);
void	sdhci_fdt_slot_write_2(device_t, struct sdhci_slot *, bus_size_t,
	    uint16_t);
uint32_t sdhci_fdt_slot_read_4(device_t, struct sdhci_slot *, bus_size_t);
void	sdhci_fdt_slot_write_4(device_t, struct sdhci_slot *, bus_size_t,
	    uint32_t);
int	sdhci_fdt_set_core_clock(device_t, int, uint64_t *);
int	sdhci_fdt_set_core_clock_flags(device_t, int, int, uint64_t *);

#endif /* _DEV_SDHCI_SDHCI_FDT_SOC_H_ */
