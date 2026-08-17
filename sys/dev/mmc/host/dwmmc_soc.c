/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>

#include <dev/ofw/openfirm.h>
#include <dev/mmc/host/dwmmc_soc.h>

static const struct dwmmc_soc_ops *dwmmc_soc_ops;

void
dwmmc_soc_register(const struct dwmmc_soc_ops *ops)
{

	dwmmc_soc_ops = ops;
}

bool
dwmmc_soc_probe(device_t dev)
{

	return (dwmmc_soc_ops != NULL && dwmmc_soc_ops->probe != NULL &&
	    dwmmc_soc_ops->probe(dev));
}

static const struct dwmmc_soc_ops *
dwmmc_soc_get_ops(struct dwmmc_softc *sc)
{

	if (dwmmc_soc_ops != NULL && dwmmc_soc_ops->match != NULL &&
	    dwmmc_soc_ops->match(sc))
		return (dwmmc_soc_ops);

	return (NULL);
}

void
dwmmc_soc_apply_fdt_defaults(struct dwmmc_softc *sc)
{
	const struct dwmmc_soc_ops *ops;

	ops = dwmmc_soc_get_ops(sc);
	if (ops != NULL && ops->apply_fdt_defaults != NULL)
		ops->apply_fdt_defaults(sc);
}

bool
dwmmc_soc_defer_missing_vmmc(struct dwmmc_softc *sc, phandle_t node, int error)
{
	const struct dwmmc_soc_ops *ops;

	ops = dwmmc_soc_get_ops(sc);
	if (ops != NULL && ops->defer_missing_vmmc != NULL)
		return (ops->defer_missing_vmmc(sc, node, error));

	return (false);
}

bool
dwmmc_soc_defer_missing_vqmmc(struct dwmmc_softc *sc, phandle_t node,
    int error)
{
	const struct dwmmc_soc_ops *ops;

	ops = dwmmc_soc_get_ops(sc);
	if (ops != NULL && ops->defer_missing_vqmmc != NULL)
		return (ops->defer_missing_vqmmc(sc, node, error));

	return (false);
}

void
dwmmc_soc_fallback_bus_hz(struct dwmmc_softc *sc)
{
	const struct dwmmc_soc_ops *ops;

	ops = dwmmc_soc_get_ops(sc);
	if (ops != NULL && ops->fallback_bus_hz != NULL)
		ops->fallback_bus_hz(sc);
}

bool
dwmmc_soc_needs_reboot_prepare(struct dwmmc_softc *sc)
{
	const struct dwmmc_soc_ops *ops;

	ops = dwmmc_soc_get_ops(sc);
	return (ops != NULL && ops->prepare_reboot);
}

void
dwmmc_soc_diag(struct dwmmc_softc *sc, const char *fmt, ...)
{
	const struct dwmmc_soc_ops *ops;
	va_list ap;

	ops = dwmmc_soc_get_ops(sc);
	if (ops == NULL || ops->vdiag == NULL)
		return;

	va_start(ap, fmt);
	ops->vdiag(sc, fmt, ap);
	va_end(ap);
}
