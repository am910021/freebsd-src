/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 */

#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/gpio.h>
#include <sys/lock.h>
#include <sys/mutex.h>

#include <dev/fdt/simplebus.h>
#include <dev/ofw/openfirm.h>
#include <dev/extres/syscon/syscon.h>

#include "rk_pinctrl.h"
#include "syscon_if.h"

#define	RK3588_PMU1_IOC_REG	0x0000
#define	RK3588_PMU2_IOC_REG	0x4000
#define	RK3588_BUS_IOC_REG	0x8000
#define	RK3588_VCCIO1_4_IOC_REG	0x9000
#define	RK3588_VCCIO3_5_IOC_REG	0xa000
#define	RK3588_VCCIO2_IOC_REG	0xb000
#define	RK3588_VCCIO6_IOC_REG	0xc000
#define	RK3588_EMMC_IOC_REG	0xd000

struct rk3588_pin_reg {
	uint32_t	pin;
	uint32_t	reg;
};

static struct rk_pinctrl_gpio rk3588_gpio_bank[] = {
	RK_GPIO(0, "gpio0"),
	RK_GPIO(1, "gpio1"),
	RK_GPIO(2, "gpio2"),
	RK_GPIO(3, "gpio3"),
	RK_GPIO(4, "gpio4"),
};

static struct rk_pinctrl_bank rk3588_iomux_bank[] = {
	RK_IOMUX(0, 0, 0x0000, 4),
	RK_IOMUX(0, 1, 0x0008, 4),
	RK_IOMUX(0, 2, 0x0010, 4),
	RK_IOMUX(0, 3, 0x0018, 4),
	RK_IOMUX(1, 0, 0x0020, 4),
	RK_IOMUX(1, 1, 0x0028, 4),
	RK_IOMUX(1, 2, 0x0030, 4),
	RK_IOMUX(1, 3, 0x0038, 4),
	RK_IOMUX(2, 0, 0x0040, 4),
	RK_IOMUX(2, 1, 0x0048, 4),
	RK_IOMUX(2, 2, 0x0050, 4),
	RK_IOMUX(2, 3, 0x0058, 4),
	RK_IOMUX(3, 0, 0x0060, 4),
	RK_IOMUX(3, 1, 0x0068, 4),
	RK_IOMUX(3, 2, 0x0070, 4),
	RK_IOMUX(3, 3, 0x0078, 4),
	RK_IOMUX(4, 0, 0x0080, 4),
	RK_IOMUX(4, 1, 0x0088, 4),
	RK_IOMUX(4, 2, 0x0090, 4),
	RK_IOMUX(4, 3, 0x0098, 4),
};

static const struct rk3588_pin_reg rk3588_drive_regs[] = {
	{ 0 * 32 +  0, RK3588_PMU1_IOC_REG + 0x0010 },
	{ 0 * 32 +  4, RK3588_PMU1_IOC_REG + 0x0014 },
	{ 0 * 32 +  8, RK3588_PMU1_IOC_REG + 0x0018 },
	{ 0 * 32 + 12, RK3588_PMU2_IOC_REG + 0x0014 },
	{ 0 * 32 + 16, RK3588_PMU2_IOC_REG + 0x0018 },
	{ 0 * 32 + 20, RK3588_PMU2_IOC_REG + 0x001c },
	{ 0 * 32 + 24, RK3588_PMU2_IOC_REG + 0x0020 },
	{ 0 * 32 + 28, RK3588_PMU2_IOC_REG + 0x0024 },
	{ 1 * 32 +  0, RK3588_VCCIO1_4_IOC_REG + 0x0020 },
	{ 1 * 32 +  4, RK3588_VCCIO1_4_IOC_REG + 0x0024 },
	{ 1 * 32 +  8, RK3588_VCCIO1_4_IOC_REG + 0x0028 },
	{ 1 * 32 + 12, RK3588_VCCIO1_4_IOC_REG + 0x002c },
	{ 1 * 32 + 16, RK3588_VCCIO1_4_IOC_REG + 0x0030 },
	{ 1 * 32 + 20, RK3588_VCCIO1_4_IOC_REG + 0x0034 },
	{ 1 * 32 + 24, RK3588_VCCIO1_4_IOC_REG + 0x0038 },
	{ 1 * 32 + 28, RK3588_VCCIO1_4_IOC_REG + 0x003c },
	{ 2 * 32 +  0, RK3588_EMMC_IOC_REG + 0x0040 },
	{ 2 * 32 +  4, RK3588_VCCIO3_5_IOC_REG + 0x0044 },
	{ 2 * 32 +  8, RK3588_VCCIO3_5_IOC_REG + 0x0048 },
	{ 2 * 32 + 12, RK3588_VCCIO3_5_IOC_REG + 0x004c },
	{ 2 * 32 + 16, RK3588_VCCIO3_5_IOC_REG + 0x0050 },
	{ 2 * 32 + 20, RK3588_VCCIO3_5_IOC_REG + 0x0054 },
	{ 2 * 32 + 24, RK3588_EMMC_IOC_REG + 0x0058 },
	{ 2 * 32 + 28, RK3588_EMMC_IOC_REG + 0x005c },
	{ 3 * 32 +  0, RK3588_VCCIO3_5_IOC_REG + 0x0060 },
	{ 3 * 32 +  4, RK3588_VCCIO3_5_IOC_REG + 0x0064 },
	{ 3 * 32 +  8, RK3588_VCCIO3_5_IOC_REG + 0x0068 },
	{ 3 * 32 + 12, RK3588_VCCIO3_5_IOC_REG + 0x006c },
	{ 3 * 32 + 16, RK3588_VCCIO3_5_IOC_REG + 0x0070 },
	{ 3 * 32 + 20, RK3588_VCCIO3_5_IOC_REG + 0x0074 },
	{ 3 * 32 + 24, RK3588_VCCIO3_5_IOC_REG + 0x0078 },
	{ 3 * 32 + 28, RK3588_VCCIO3_5_IOC_REG + 0x007c },
	{ 4 * 32 +  0, RK3588_VCCIO6_IOC_REG + 0x0080 },
	{ 4 * 32 +  4, RK3588_VCCIO6_IOC_REG + 0x0084 },
	{ 4 * 32 +  8, RK3588_VCCIO6_IOC_REG + 0x0088 },
	{ 4 * 32 + 12, RK3588_VCCIO6_IOC_REG + 0x008c },
	{ 4 * 32 + 16, RK3588_VCCIO6_IOC_REG + 0x0090 },
	{ 4 * 32 + 18, RK3588_VCCIO3_5_IOC_REG + 0x0090 },
	{ 4 * 32 + 20, RK3588_VCCIO3_5_IOC_REG + 0x0094 },
	{ 4 * 32 + 24, RK3588_VCCIO2_IOC_REG + 0x0098 },
	{ 4 * 32 + 28, RK3588_VCCIO2_IOC_REG + 0x009c },
};

static const struct rk3588_pin_reg rk3588_pull_regs[] = {
	{ 0 * 32 +  0, RK3588_PMU1_IOC_REG + 0x0020 },
	{ 0 * 32 +  8, RK3588_PMU1_IOC_REG + 0x0024 },
	{ 0 * 32 + 13, RK3588_PMU2_IOC_REG + 0x0028 },
	{ 0 * 32 + 16, RK3588_PMU2_IOC_REG + 0x002c },
	{ 0 * 32 + 24, RK3588_PMU2_IOC_REG + 0x0030 },
	{ 1 * 32 +  0, RK3588_VCCIO1_4_IOC_REG + 0x0110 },
	{ 1 * 32 +  8, RK3588_VCCIO1_4_IOC_REG + 0x0114 },
	{ 1 * 32 + 16, RK3588_VCCIO1_4_IOC_REG + 0x0118 },
	{ 1 * 32 + 24, RK3588_VCCIO1_4_IOC_REG + 0x011c },
	{ 2 * 32 +  0, RK3588_EMMC_IOC_REG + 0x0120 },
	{ 2 * 32 +  6, RK3588_VCCIO3_5_IOC_REG + 0x0120 },
	{ 2 * 32 +  8, RK3588_VCCIO3_5_IOC_REG + 0x0124 },
	{ 2 * 32 + 16, RK3588_VCCIO3_5_IOC_REG + 0x0128 },
	{ 2 * 32 + 24, RK3588_EMMC_IOC_REG + 0x012c },
	{ 3 * 32 +  0, RK3588_VCCIO3_5_IOC_REG + 0x0130 },
	{ 3 * 32 +  8, RK3588_VCCIO3_5_IOC_REG + 0x0134 },
	{ 3 * 32 + 16, RK3588_VCCIO3_5_IOC_REG + 0x0138 },
	{ 3 * 32 + 24, RK3588_VCCIO3_5_IOC_REG + 0x013c },
	{ 4 * 32 +  0, RK3588_VCCIO6_IOC_REG + 0x0140 },
	{ 4 * 32 +  8, RK3588_VCCIO6_IOC_REG + 0x0144 },
	{ 4 * 32 + 16, RK3588_VCCIO6_IOC_REG + 0x0148 },
	{ 4 * 32 + 18, RK3588_VCCIO3_5_IOC_REG + 0x0148 },
	{ 4 * 32 + 24, RK3588_VCCIO2_IOC_REG + 0x014c },
};

static int
rk3588_calc_reg_and_bit(const struct rk3588_pin_reg *regs, int nregs,
    uint32_t bank, uint32_t pin, uint32_t pins_per_reg,
    uint32_t bits_per_pin, uint32_t *reg, uint32_t *bit, uint32_t *mask)
{
	uint32_t global_pin;
	int i;

	global_pin = bank * 32 + pin;
	for (i = nregs - 1; i >= 0; i--) {
		if (global_pin >= regs[i].pin) {
			*reg = regs[i].reg;
			*bit = (pin % pins_per_reg) * bits_per_pin;
			*mask = ((1U << bits_per_pin) - 1) << *bit;
			return (0);
		}
	}

	return (EINVAL);
}

static int
rk3588_calc_pull(uint32_t bank, uint32_t pin, uint32_t *reg,
    uint32_t *bit, uint32_t *mask)
{

	return (rk3588_calc_reg_and_bit(rk3588_pull_regs,
	    nitems(rk3588_pull_regs), bank, pin, 8, 2, reg, bit, mask));
}

static int
rk3588_calc_drive(uint32_t bank, uint32_t pin, uint32_t *reg,
    uint32_t *bit, uint32_t *mask)
{

	return (rk3588_calc_reg_and_bit(rk3588_drive_regs,
	    nitems(rk3588_drive_regs), bank, pin, 4, 4, reg, bit, mask));
}

static struct rk_pinctrl_bank *
rk3588_find_iomux(struct rk_pinctrl_softc *sc, uint32_t bank, uint32_t pin)
{
	uint32_t subbank;
	int i;

	subbank = pin / 8;
	for (i = 0; i < sc->conf->iomux_nbanks; i++)
		if (sc->conf->iomux_conf[i].bank == bank &&
		    sc->conf->iomux_conf[i].subbank == subbank)
			return (&sc->conf->iomux_conf[i]);
	return (NULL);
}

static int
rk3588_calc_mux(struct rk_pinctrl_bank *iomux, uint32_t pin, uint32_t *reg,
    uint32_t *mask, uint32_t *bit)
{

	if (iomux == NULL || iomux->nbits != 4)
		return (EINVAL);
	*reg = iomux->offset;
	if ((pin % 8) >= 4)
		*reg += 4;
	*bit = (pin % 4) * 4;
	*mask = 0xf;
	return (0);
}

static void
rk3588_write_mux(struct syscon *syscon, uint32_t bank, uint32_t pin,
    uint32_t function, uint32_t reg, uint32_t mask, uint32_t bit)
{
	uint32_t data;

	data = (mask << (bit + 16)) | ((function & mask) << bit);
	if (bank == 0 && pin >= 12) {
		if (function < 8) {
			SYSCON_WRITE_4(syscon,
			    reg + RK3588_PMU2_IOC_REG - 0xc, data);
			return;
		}
		SYSCON_WRITE_4(syscon, reg + RK3588_PMU2_IOC_REG - 0xc,
		    (mask << (bit + 16)) | (8 << bit));
		SYSCON_WRITE_4(syscon, reg + RK3588_BUS_IOC_REG, data);
		return;
	}
	if (bank > 0)
		reg += RK3588_BUS_IOC_REG;
	SYSCON_WRITE_4(syscon, reg, data);
}

static int
rk3588_read_mux(struct rk_pinctrl_softc *sc, struct syscon *syscon,
    uint32_t bank, uint32_t pin, uint32_t *pinfunc)
{
	struct rk_pinctrl_bank *iomux;
	uint32_t bit, mask, reg;

	iomux = rk3588_find_iomux(sc, bank, pin);
	if (rk3588_calc_mux(iomux, pin, &reg, &mask, &bit) != 0)
		return (EINVAL);
	if (bank == 0 && pin >= 12) {
		reg += RK3588_PMU2_IOC_REG - 0xc;
		*pinfunc = (SYSCON_READ_4(syscon, reg) >> bit) & mask;
		if (*pinfunc != 8)
			return (0);
		reg += RK3588_BUS_IOC_REG - RK3588_PMU2_IOC_REG + 0xc;
	} else if (bank > 0) {
		reg += RK3588_BUS_IOC_REG;
	}
	*pinfunc = (SYSCON_READ_4(syscon, reg) >> bit) & mask;
	return (0);
}

static int
rk3588_parse_bias(phandle_t node, int bank)
{

	if (node <= 0)
		return (-1);
	if (OF_hasprop(node, "bias-disable"))
		return (0);
	if (OF_hasprop(node, "bias-pull-down"))
		return (1);
	if (OF_hasprop(node, "bias-pull-up"))
		return (3);
	return (-1);
}

static int
rk3588_resolv_bias_value(int bank, int bias)
{

	if (bias == 3)
		return (GPIO_PIN_PULLUP);
	if (bias == 1)
		return (GPIO_PIN_PULLDOWN);
	return (0);
}

static int
rk3588_get_bias_value(int bank, int bias)
{

	if (bias & GPIO_PIN_PULLUP)
		return (3);
	if (bias & GPIO_PIN_PULLDOWN)
		return (1);
	return (0);
}

static struct syscon *
rk3588_get_syscon(struct rk_pinctrl_softc *sc, uint32_t bank)
{

	return (sc->grf);
}

static void
rk3588_configure_pin(struct rk_pinctrl_softc *sc, uint32_t *pindata)
{
	struct rk_pinctrl_bank *iomux;
	struct syscon *syscon;
	phandle_t pin_conf;
	uint32_t bank, bit, drive, function, mask, pin, reg, value;
	int bias;

	bank = pindata[0];
	pin = pindata[1];
	function = pindata[2];
	pin_conf = OF_node_from_xref(pindata[3]);
	iomux = rk3588_find_iomux(sc, bank, pin);
	if (iomux == NULL) {
		device_printf(sc->dev, "Unknown pin %u in bank %u\n", pin, bank);
		return;
	}
	syscon = sc->grf;

	bias = rk3588_parse_bias(pin_conf, bank);
	if (bias >= 0 &&
	    rk3588_calc_pull(bank, pin, &reg, &bit, &mask) == 0)
		SYSCON_WRITE_4(syscon, reg, (mask << 16) | (bias << bit));

	if (pin_conf > 0 &&
	    OF_getencprop(pin_conf, "drive-strength", &value,
	    sizeof(value)) > 0 &&
	    rk3588_calc_drive(bank, pin, &reg, &bit, &mask) == 0) {
		drive = value & 0xf;
		SYSCON_WRITE_4(syscon, reg, (mask << 16) | (drive << bit));
	}

	if (rk3588_calc_mux(iomux, pin, &reg, &mask, &bit) == 0)
		rk3588_write_mux(syscon, bank, pin, function, reg, mask, bit);
}

static int
rk3588_is_gpio(struct rk_pinctrl_softc *sc, struct syscon *syscon, int bank,
    uint32_t pin, bool *is_gpio)
{
	uint32_t pinfunc;
	int error;

	error = rk3588_read_mux(sc, syscon, bank, pin, &pinfunc);
	if (error != 0)
		return (error);
	*is_gpio = (pinfunc == 0);
	return (0);
}

static int
rk3588_get_flags(struct rk_pinctrl_softc *sc, struct syscon *syscon, int bank,
    uint32_t pin, uint32_t *flags)
{
	uint32_t bit, mask, reg, value;
	int error;

	error = rk3588_calc_pull(bank, pin, &reg, &bit, &mask);
	if (error != 0)
		return (error);
	value = (SYSCON_READ_4(syscon, reg) & mask) >> bit;
	*flags = rk3588_resolv_bias_value(bank, value);
	return (0);
}

static int
rk3588_set_flags(struct rk_pinctrl_softc *sc, struct syscon *syscon, int bank,
    uint32_t pin, uint32_t flags)
{
	uint32_t bias, bit, mask, reg;
	int error;

	error = rk3588_calc_pull(bank, pin, &reg, &bit, &mask);
	if (error != 0)
		return (error);
	bias = rk3588_get_bias_value(bank, flags);
	SYSCON_WRITE_4(syscon, reg, (mask << 16) | (bias << bit));
	return (0);
}

struct rk_pinctrl_conf rk3588_pinctrl_conf = {
	.iomux_conf = rk3588_iomux_bank,
	.iomux_nbanks = nitems(rk3588_iomux_bank),
	.gpio_bank = rk3588_gpio_bank,
	.ngpio_bank = nitems(rk3588_gpio_bank),
	.get_syscon = rk3588_get_syscon,
	.parse_bias = rk3588_parse_bias,
	.resolv_bias_value = rk3588_resolv_bias_value,
	.get_bias_value = rk3588_get_bias_value,
	.configure_pin = rk3588_configure_pin,
	.is_gpio = rk3588_is_gpio,
	.get_flags = rk3588_get_flags,
	.set_flags = rk3588_set_flags,
};
