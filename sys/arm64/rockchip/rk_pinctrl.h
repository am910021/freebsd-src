/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2018 Emmanuel Vadot <manu@FreeBSD.org>
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 */

#ifndef _ARM64_ROCKCHIP_RK_PINCTRL_H_
#define	_ARM64_ROCKCHIP_RK_PINCTRL_H_

struct rk_pinctrl_pin_drive {
	uint32_t	bank;
	uint32_t	subbank;
	uint32_t	offset;
	uint32_t	value;
	uint32_t	ma;
};

struct rk_pinctrl_bank {
	uint32_t	bank;
	uint32_t	subbank;
	uint32_t	offset;
	uint32_t	nbits;
};

struct rk_pinctrl_pin_fixup {
	uint32_t	bank;
	uint32_t	subbank;
	uint32_t	pin;
	uint32_t	reg;
	uint32_t	bit;
	uint32_t	mask;
};

struct rk_pinctrl_gpio {
	uint32_t	bank;
	char		*gpio_name;
	device_t	gpio_dev;
};

struct rk_pinctrl_softc;

struct rk_pinctrl_conf {
	struct rk_pinctrl_bank		*iomux_conf;
	uint32_t			iomux_nbanks;
	struct rk_pinctrl_pin_fixup	*pin_fixup;
	uint32_t			npin_fixup;
	struct rk_pinctrl_pin_drive	*pin_drive;
	uint32_t			npin_drive;
	struct rk_pinctrl_gpio		*gpio_bank;
	uint32_t			ngpio_bank;
	uint32_t	(*get_pd_offset)(struct rk_pinctrl_softc *, uint32_t);
	struct syscon	*(*get_syscon)(struct rk_pinctrl_softc *, uint32_t);
	int		(*parse_bias)(phandle_t, int);
	int		(*resolv_bias_value)(int, int);
	int		(*get_bias_value)(int, int);
	void		(*configure_pin)(struct rk_pinctrl_softc *, uint32_t *);
	int		(*is_gpio)(struct rk_pinctrl_softc *, struct syscon *,
			    int, uint32_t, bool *);
	int		(*get_flags)(struct rk_pinctrl_softc *, struct syscon *,
			    int, uint32_t, uint32_t *);
	int		(*set_flags)(struct rk_pinctrl_softc *, struct syscon *,
			    int, uint32_t, uint32_t);
	bool		is_rk3568;
};

struct rk_pinctrl_softc {
	struct simplebus_softc	simplebus_sc;
	device_t		dev;
	struct syscon		*grf;
	struct syscon		*pmu;
	struct rk_pinctrl_conf	*conf;
	struct mtx		mtx;
};

#define	RK_IOMUX(_bank, _subbank, _offset, _nbits)			\
{									\
	.bank = _bank,							\
	.subbank = _subbank,						\
	.offset = _offset,						\
	.nbits = _nbits,						\
}

#define	RK_PINFIX(_bank, _pin, _reg, _bit, _mask)			\
{									\
	.bank = _bank,							\
	.pin = _pin,							\
	.reg = _reg,							\
	.bit = _bit,							\
	.mask = _mask,							\
}

#define	RK_PINDRIVE(_bank, _subbank, _offset, _value, _ma)		\
{									\
	.bank = _bank,							\
	.subbank = _subbank,						\
	.offset = _offset,						\
	.value = _value,						\
	.ma = _ma,							\
}

#define	RK_GPIO(_bank, _name)						\
{									\
	.bank = _bank,							\
	.gpio_name = _name,						\
}

extern struct rk_pinctrl_conf rk3588_pinctrl_conf;

#endif
