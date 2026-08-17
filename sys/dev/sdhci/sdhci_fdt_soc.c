/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/systm.h>

#include <dev/sdhci/sdhci_fdt_soc.h>

static const struct sdhci_fdt_soc_ops *sdhci_fdt_soc_ops;

void
sdhci_fdt_soc_register(const struct sdhci_fdt_soc_ops *ops)
{

	sdhci_fdt_soc_ops = ops;
}

static const struct sdhci_fdt_soc_ops *
sdhci_fdt_soc_get_ops(device_t dev)
{

	if (sdhci_fdt_soc_ops != NULL && sdhci_fdt_soc_ops->match != NULL &&
	    sdhci_fdt_soc_ops->match(dev))
		return (sdhci_fdt_soc_ops);

	return (NULL);
}

bool
sdhci_fdt_soc_probe(device_t dev)
{
	const struct sdhci_fdt_soc_ops *ops;

	ops = sdhci_fdt_soc_get_ops(dev);
	if (ops != NULL && ops->probe != NULL)
		return (ops->probe(dev));

	return (false);
}

void
sdhci_fdt_soc_probe_setup(device_t dev, u_int *quirks)
{
	const struct sdhci_fdt_soc_ops *ops;

	ops = sdhci_fdt_soc_get_ops(dev);
	if (ops != NULL && ops->probe_setup != NULL)
		ops->probe_setup(dev, quirks);
}

void
sdhci_fdt_soc_post_fdt_parse(device_t dev, u_int *quirks, bool *wp_disabled)
{
	const struct sdhci_fdt_soc_ops *ops;

	ops = sdhci_fdt_soc_get_ops(dev);
	if (ops != NULL && ops->post_fdt_parse != NULL)
		ops->post_fdt_parse(dev, quirks, wp_disabled);
}

bool
sdhci_fdt_soc_uses_core_clock(device_t dev)
{
	const struct sdhci_fdt_soc_ops *ops;

	ops = sdhci_fdt_soc_get_ops(dev);
	if (ops != NULL && ops->uses_core_clock != NULL)
		return (ops->uses_core_clock(dev));

	return (false);
}

bool
sdhci_fdt_soc_uses_controller_resets(device_t dev)
{
	const struct sdhci_fdt_soc_ops *ops;

	ops = sdhci_fdt_soc_get_ops(dev);
	if (ops != NULL && ops->uses_controller_resets != NULL)
		return (ops->uses_controller_resets(dev));

	return (false);
}

void
sdhci_fdt_soc_clock_bringup(device_t dev)
{
	const struct sdhci_fdt_soc_ops *ops;

	ops = sdhci_fdt_soc_get_ops(dev);
	if (ops != NULL && ops->clock_bringup != NULL)
		ops->clock_bringup(dev);
}

void
sdhci_fdt_soc_init_slot(device_t dev, struct sdhci_slot *slot)
{
	const struct sdhci_fdt_soc_ops *ops;

	ops = sdhci_fdt_soc_get_ops(dev);
	if (ops != NULL && ops->init_slot != NULL)
		ops->init_slot(dev, slot);
}

bool
sdhci_fdt_soc_enable_high_speed_caps(device_t dev)
{
	const struct sdhci_fdt_soc_ops *ops;

	ops = sdhci_fdt_soc_get_ops(dev);
	if (ops != NULL && ops->enable_high_speed_caps != NULL)
		return (ops->enable_high_speed_caps(dev));

	return (false);
}

uint32_t
sdhci_fdt_soc_filter_caps2(device_t dev, uint32_t caps2)
{
	const struct sdhci_fdt_soc_ops *ops;

	ops = sdhci_fdt_soc_get_ops(dev);
	if (ops != NULL && ops->filter_caps2 != NULL)
		return (ops->filter_caps2(dev, caps2));

	return (caps2);
}

bool
sdhci_fdt_soc_set_clock(device_t dev, struct sdhci_slot *slot, int *clock)
{
	const struct sdhci_fdt_soc_ops *ops;

	ops = sdhci_fdt_soc_get_ops(dev);
	if (ops != NULL && ops->set_clock != NULL)
		return (ops->set_clock(dev, slot, clock));

	return (false);
}

void
sdhci_fdt_soc_post_reset(device_t dev, struct sdhci_slot *slot, uint8_t mask)
{
	const struct sdhci_fdt_soc_ops *ops;

	ops = sdhci_fdt_soc_get_ops(dev);
	if (ops != NULL && ops->post_reset != NULL)
		ops->post_reset(dev, slot, mask);
}

void
sdhci_fdt_soc_diag(device_t dev, const char *fmt, ...)
{
	const struct sdhci_fdt_soc_ops *ops;
	va_list ap;

	ops = sdhci_fdt_soc_get_ops(dev);
	if (ops == NULL || ops->vdiag == NULL)
		return;

	va_start(ap, fmt);
	ops->vdiag(dev, fmt, ap);
	va_end(ap);
}
