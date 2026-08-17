/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 */

#ifndef _DEV_MMC_HOST_DWMMC_SOC_H_
#define	_DEV_MMC_HOST_DWMMC_SOC_H_

#include <sys/bus.h>

#include <dev/ofw/openfirm.h>

#include <machine/stdarg.h>

struct dwmmc_softc;

struct dwmmc_soc_ops {
	bool	(*match)(struct dwmmc_softc *);
	bool	(*probe)(device_t);
	void	(*apply_fdt_defaults)(struct dwmmc_softc *);
	bool	(*defer_missing_vmmc)(struct dwmmc_softc *, phandle_t, int);
	bool	(*defer_missing_vqmmc)(struct dwmmc_softc *, phandle_t, int);
	void	(*fallback_bus_hz)(struct dwmmc_softc *);
	bool	prepare_reboot;
	void	(*vdiag)(struct dwmmc_softc *, const char *, va_list);
};

void	dwmmc_soc_register(const struct dwmmc_soc_ops *);
bool	dwmmc_soc_probe(device_t);
void	dwmmc_soc_apply_fdt_defaults(struct dwmmc_softc *);
bool	dwmmc_soc_defer_missing_vmmc(struct dwmmc_softc *, phandle_t, int);
bool	dwmmc_soc_defer_missing_vqmmc(struct dwmmc_softc *, phandle_t, int);
void	dwmmc_soc_fallback_bus_hz(struct dwmmc_softc *);
bool	dwmmc_soc_needs_reboot_prepare(struct dwmmc_softc *);
void	dwmmc_soc_diag(struct dwmmc_softc *, const char *, ...);

#endif /* _DEV_MMC_HOST_DWMMC_SOC_H_ */
