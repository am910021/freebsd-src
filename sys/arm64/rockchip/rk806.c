/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/eventhandler.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/reboot.h>

#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#include <dev/ofw/openfirm.h>

#include <dev/extres/regulator/regulator.h>

#include <dev/spibus/spi.h>

#include "regdev_if.h"
#include "spibus_if.h"

#define	RK806_CMD_WRITE		0x80
#define	RK806_POWER_EN0		0x00
#define	RK806_POWER_EN1		0x01
#define	RK806_POWER_EN2		0x02
#define	RK806_POWER_EN3		0x03
#define	RK806_POWER_EN4		0x04
#define	RK806_POWER_EN5		0x05
#define	RK806_BUCK_ON_VSEL(_n)	(0x1a + (_n) - 1)
#define	RK806_NLDO_ON_VSEL(_n)	(0x43 + (_n) - 1)
#define	RK806_PLDO_ON_VSEL(_n)	(0x4e + (_n) - 1)
#define	RK806_CHIP_NAME		0x5a
#define	RK806_CHIP_VER		0x5b
#define	RK806_SYS_CFG3		0x72
#define	RK806_DEV_OFF		(1U << 0)
#define	RK806_RESET_FUNC_MASK	(3U << 6)
#define	RK806_ID		0x8060
#define	RK806_ID_MASK		0xfff0

enum rk806_regulator_id {
	RK806_DCDC1,
	RK806_DCDC2,
	RK806_DCDC3,
	RK806_DCDC4,
	RK806_DCDC5,
	RK806_DCDC6,
	RK806_DCDC7,
	RK806_DCDC8,
	RK806_DCDC9,
	RK806_DCDC10,
	RK806_NLDO1,
	RK806_NLDO2,
	RK806_NLDO3,
	RK806_NLDO4,
	RK806_NLDO5,
	RK806_PLDO1,
	RK806_PLDO2,
	RK806_PLDO3,
	RK806_PLDO4,
	RK806_PLDO5,
	RK806_PLDO6,
	RK806_REGULATOR_COUNT,
};

struct rk806_softc {
	device_t	dev;
	struct regnode	*regulators[RK806_REGULATOR_COUNT];
};

struct rk806_reg_sc {
	device_t	dev;
	uint16_t	enable_reg;
	uint16_t	vsel_reg;
	uint8_t		enable_bit;
	uint8_t		write_mask;
	struct regulator_range *ranges;
	int		nranges;
};

struct rk806_reg_def {
	const char	*node_name;
	const char	*supply_name;
	uint16_t	enable_reg;
	uint16_t	vsel_reg;
	uint8_t		enable_bit;
	uint8_t		write_mask;
	struct regulator_range *ranges;
	int		nranges;
};

static struct regulator_range rk806_buck_ranges[] = {
	REG_RANGE_INIT(0, 159, 500000, 6250),
	REG_RANGE_INIT(160, 236, 1500000, 25000),
	REG_RANGE_INIT(237, 255, 3400000, 0),
};

static struct regulator_range rk806_pldo_ranges[] = {
	REG_RANGE_INIT(0, 231, 500000, 12500),
	REG_RANGE_INIT(232, 255, 3400000, 0),
};

#define	RK806_REG(_node, _supply, _ereg, _bit, _vreg, _ranges)	\
	{								\
		.node_name = (_node),					\
		.supply_name = (_supply),				\
		.enable_reg = (_ereg),					\
		.vsel_reg = (_vreg),					\
		.enable_bit = (1U << (_bit)),				\
		.write_mask = (1U << ((_bit) + 4)),			\
		.ranges = (_ranges),					\
		.nranges = nitems(_ranges),				\
	}

static const struct rk806_reg_def rk806_reg_defs[] = {
	RK806_REG("dcdc-reg1", "vcc1-supply", RK806_POWER_EN0, 0,
	    RK806_BUCK_ON_VSEL(1), rk806_buck_ranges),
	RK806_REG("dcdc-reg2", "vcc2-supply", RK806_POWER_EN0, 1,
	    RK806_BUCK_ON_VSEL(2), rk806_buck_ranges),
	RK806_REG("dcdc-reg3", "vcc3-supply", RK806_POWER_EN0, 2,
	    RK806_BUCK_ON_VSEL(3), rk806_buck_ranges),
	RK806_REG("dcdc-reg4", "vcc4-supply", RK806_POWER_EN0, 3,
	    RK806_BUCK_ON_VSEL(4), rk806_buck_ranges),
	RK806_REG("dcdc-reg5", "vcc5-supply", RK806_POWER_EN1, 0,
	    RK806_BUCK_ON_VSEL(5), rk806_buck_ranges),
	RK806_REG("dcdc-reg6", "vcc6-supply", RK806_POWER_EN1, 1,
	    RK806_BUCK_ON_VSEL(6), rk806_buck_ranges),
	RK806_REG("dcdc-reg7", "vcc7-supply", RK806_POWER_EN1, 2,
	    RK806_BUCK_ON_VSEL(7), rk806_buck_ranges),
	RK806_REG("dcdc-reg8", "vcc8-supply", RK806_POWER_EN1, 3,
	    RK806_BUCK_ON_VSEL(8), rk806_buck_ranges),
	RK806_REG("dcdc-reg9", "vcc9-supply", RK806_POWER_EN2, 0,
	    RK806_BUCK_ON_VSEL(9), rk806_buck_ranges),
	RK806_REG("dcdc-reg10", "vcc10-supply", RK806_POWER_EN2, 1,
	    RK806_BUCK_ON_VSEL(10), rk806_buck_ranges),
	RK806_REG("nldo-reg1", "vcc13-supply", RK806_POWER_EN3, 0,
	    RK806_NLDO_ON_VSEL(1), rk806_pldo_ranges),
	RK806_REG("nldo-reg2", "vcc13-supply", RK806_POWER_EN3, 1,
	    RK806_NLDO_ON_VSEL(2), rk806_pldo_ranges),
	RK806_REG("nldo-reg3", "vcc13-supply", RK806_POWER_EN3, 2,
	    RK806_NLDO_ON_VSEL(3), rk806_pldo_ranges),
	RK806_REG("nldo-reg4", "vcc14-supply", RK806_POWER_EN3, 3,
	    RK806_NLDO_ON_VSEL(4), rk806_pldo_ranges),
	RK806_REG("nldo-reg5", "vcc14-supply", RK806_POWER_EN5, 2,
	    RK806_NLDO_ON_VSEL(5), rk806_pldo_ranges),
	RK806_REG("pldo-reg1", "vcc11-supply", RK806_POWER_EN4, 1,
	    RK806_PLDO_ON_VSEL(1), rk806_pldo_ranges),
	RK806_REG("pldo-reg2", "vcc11-supply", RK806_POWER_EN4, 2,
	    RK806_PLDO_ON_VSEL(2), rk806_pldo_ranges),
	RK806_REG("pldo-reg3", "vcc11-supply", RK806_POWER_EN4, 3,
	    RK806_PLDO_ON_VSEL(3), rk806_pldo_ranges),
	RK806_REG("pldo-reg4", "vcc12-supply", RK806_POWER_EN5, 0,
	    RK806_PLDO_ON_VSEL(4), rk806_pldo_ranges),
	RK806_REG("pldo-reg5", "vcc12-supply", RK806_POWER_EN5, 1,
	    RK806_PLDO_ON_VSEL(5), rk806_pldo_ranges),
	RK806_REG("pldo-reg6", "vcca-supply", RK806_POWER_EN4, 0,
	    RK806_PLDO_ON_VSEL(6), rk806_pldo_ranges),
};

static int
rk806_read(device_t dev, uint16_t reg, uint8_t *val)
{
	struct spi_command cmd = SPI_COMMAND_INITIALIZER;
	uint8_t tx_cmd[3], rx_data;
	int error;

	tx_cmd[0] = 0;		/* Read one byte with CRC disabled. */
	tx_cmd[1] = reg;
	tx_cmd[2] = reg >> 8;
	rx_data = 0;

	cmd.tx_cmd = tx_cmd;
	cmd.tx_cmd_sz = sizeof(tx_cmd);
	cmd.rx_cmd_sz = sizeof(tx_cmd);
	cmd.tx_data_sz = sizeof(rx_data);
	cmd.rx_data = &rx_data;
	cmd.rx_data_sz = sizeof(rx_data);

	error = SPIBUS_TRANSFER(device_get_parent(dev), dev, &cmd);
	if (error == 0)
		*val = rx_data;
	return (error);
}

static int
rk806_write(device_t dev, uint16_t reg, uint8_t val)
{
	struct spi_command cmd = SPI_COMMAND_INITIALIZER;
	uint8_t tx_cmd, tx_data[3];

	tx_cmd = RK806_CMD_WRITE;
	tx_data[0] = reg;
	tx_data[1] = reg >> 8;
	tx_data[2] = val;

	cmd.tx_cmd = &tx_cmd;
	cmd.tx_cmd_sz = sizeof(tx_cmd);
	cmd.rx_cmd_sz = sizeof(tx_cmd);
	cmd.tx_data = tx_data;
	cmd.tx_data_sz = sizeof(tx_data);
	cmd.rx_data_sz = sizeof(tx_data);

	return (SPIBUS_TRANSFER(device_get_parent(dev), dev, &cmd));
}

static int
rk806_reg_init(struct regnode *regnode __unused)
{

	return (0);
}

static int
rk806_reg_enable(struct regnode *regnode, bool enable, int *udelay)
{
	struct rk806_reg_sc *sc;
	struct regnode_std_param *param;
	uint8_t val;
	int error;

	sc = regnode_get_softc(regnode);
	param = regnode_get_stdparam(regnode);
	*udelay = 0;
	error = rk806_read(sc->dev, sc->enable_reg, &val);
	if (error != 0)
		return (error);
	if (((val & sc->enable_bit) != 0) == enable)
		return (0);

	val = sc->write_mask;
	if (enable)
		val |= sc->enable_bit;
	error = rk806_write(sc->dev, sc->enable_reg, val);
	if (error != 0)
		return (error);
	error = rk806_read(sc->dev, sc->enable_reg, &val);
	if (error != 0)
		return (error);
	if (((val & sc->enable_bit) != 0) != enable)
		return (EIO);

	*udelay = param->enable_delay;
	return (0);
}

static int
rk806_reg_status(struct regnode *regnode, int *status)
{
	struct rk806_reg_sc *sc;
	uint8_t val;
	int error;

	sc = regnode_get_softc(regnode);
	*status = 0;
	error = rk806_read(sc->dev, sc->enable_reg, &val);
	if (error == 0 && (val & sc->enable_bit) != 0)
		*status = REGULATOR_STATUS_ENABLED;
	return (error);
}

static int
rk806_reg_get_voltage(struct regnode *regnode, int *uvolt)
{
	struct rk806_reg_sc *sc;
	uint8_t sel;
	int error;

	sc = regnode_get_softc(regnode);
	error = rk806_read(sc->dev, sc->vsel_reg, &sel);
	if (error != 0)
		return (error);
	return (regulator_range_sel8_to_volt(sc->ranges, sc->nranges, sel,
	    uvolt));
}

static int
rk806_reg_set_voltage(struct regnode *regnode, int min_uvolt,
	int max_uvolt, int *udelay)
{
	struct rk806_reg_sc *sc;
	struct regnode_std_param *param;
	uint8_t old_sel, new_sel, check_sel;
	int delta, error, new_uvolt, old_uvolt;

	sc = regnode_get_softc(regnode);
	param = regnode_get_stdparam(regnode);
	*udelay = 0;
	error = regulator_range_volt_to_sel8(sc->ranges, sc->nranges,
	    min_uvolt, max_uvolt, &new_sel);
	if (error != 0)
		return (error);
	error = rk806_read(sc->dev, sc->vsel_reg, &old_sel);
	if (error != 0 || old_sel == new_sel)
		return (error);
	error = regulator_range_sel8_to_volt(sc->ranges, sc->nranges, old_sel,
	    &old_uvolt);
	if (error != 0)
		return (error);
	error = regulator_range_sel8_to_volt(sc->ranges, sc->nranges, new_sel,
	    &new_uvolt);
	if (error != 0)
		return (error);

	error = rk806_write(sc->dev, sc->vsel_reg, new_sel);
	if (error != 0)
		return (error);
	error = rk806_read(sc->dev, sc->vsel_reg, &check_sel);
	if (error != 0)
		return (error);
	if (check_sel != new_sel)
		return (EIO);

	delta = new_uvolt > old_uvolt ? new_uvolt - old_uvolt :
	    old_uvolt - new_uvolt;
	if (param->ramp_delay > 0)
		*udelay = howmany(delta, param->ramp_delay);
	return (0);
}

static regnode_method_t rk806_reg_methods[] = {
	REGNODEMETHOD(regnode_init,		rk806_reg_init),
	REGNODEMETHOD(regnode_enable,		rk806_reg_enable),
	REGNODEMETHOD(regnode_status,		rk806_reg_status),
	REGNODEMETHOD(regnode_set_voltage,	rk806_reg_set_voltage),
	REGNODEMETHOD(regnode_get_voltage,	rk806_reg_get_voltage),
	REGNODEMETHOD(regnode_check_voltage,	regnode_method_check_voltage),
	REGNODEMETHOD_END
};
DEFINE_CLASS_1(rk806_reg, rk806_reg_class, rk806_reg_methods,
    sizeof(struct rk806_reg_sc), regnode_class);

static int
rk806_attach_regulator(struct rk806_softc *sc,
	const struct rk806_reg_def *def, int id,
    struct regnode **result)
{
	struct regnode_init_def initdef;
	struct rk806_reg_sc *reg_sc;
	struct regnode *regnode;
	phandle_t node, parent, regulators;
	pcell_t parent_xref;
	uint8_t selector;
	int error, len, uvolt;

	node = ofw_bus_get_node(sc->dev);
	regulators = ofw_bus_find_child(node, "regulators");
	if (regulators <= 0)
		return (ENOENT);
	node = ofw_bus_find_child(regulators, def->node_name);
	if (node <= 0)
		return (ENOENT);

	memset(&initdef, 0, sizeof(initdef));
	error = regulator_parse_ofw_stdparam(sc->dev, node, &initdef);
	if (error != 0)
		return (error);
	initdef.id = id;
	initdef.ofw_node = node;
	len = OF_getencprop(ofw_bus_get_node(sc->dev), def->supply_name,
	    &parent_xref, sizeof(parent_xref));
	if (len != sizeof(parent_xref))
		return (ENXIO);
	parent = OF_node_from_xref(parent_xref);
	if (parent <= 0 || OF_getprop_alloc(parent, "regulator-name",
	    (void **)&initdef.parent_name) <= 0)
		return (ENXIO);
	if (initdef.std_param.min_uvolt == 0 &&
	    initdef.std_param.max_uvolt == 0) {
		error = rk806_read(sc->dev, def->vsel_reg, &selector);
		if (error != 0)
			return (error);
		error = regulator_range_sel8_to_volt(def->ranges, def->nranges,
		    selector, &uvolt);
		if (error != 0)
			return (error);
		initdef.std_param.min_uvolt = uvolt;
		initdef.std_param.max_uvolt = uvolt;
	}

	regnode = regnode_create(sc->dev, &rk806_reg_class, &initdef);
	if (regnode == NULL)
		return (ENXIO);
	reg_sc = regnode_get_softc(regnode);
	reg_sc->dev = sc->dev;
	reg_sc->enable_reg = def->enable_reg;
	reg_sc->enable_bit = def->enable_bit;
	reg_sc->write_mask = def->write_mask;
	reg_sc->vsel_reg = def->vsel_reg;
	reg_sc->ranges = def->ranges;
	reg_sc->nranges = def->nranges;
	regnode = regnode_register(regnode);
	if (regnode == NULL)
		return (ENXIO);
	*result = regnode;

	return (0);
}

static int
rk806_probe(device_t dev)
{
	if (!ofw_bus_status_okay(dev) ||
	    !ofw_bus_is_compatible(dev, "rockchip,rk806"))
		return (ENXIO);

	device_set_desc(dev, "Rockchip RK806 PMIC");
	return (BUS_PROBE_DEFAULT);
}

static void
rk806_poweroff(void *arg, int howto)
{
	struct rk806_softc *sc;
	uint8_t val;
	int error;

	if ((howto & RB_POWEROFF) == 0)
		return;

	sc = arg;
	device_printf(sc->dev, "powering off\n");
	error = rk806_read(sc->dev, RK806_SYS_CFG3, &val);
	if (error == 0)
		error = rk806_write(sc->dev, RK806_SYS_CFG3,
		    val | RK806_DEV_OFF);
	if (error != 0) {
		device_printf(sc->dev, "power off failed: %d\n", error);
		return;
	}
	DELAY(100);
}

static int
rk806_regdev_map(device_t dev, phandle_t xref, int ncells,
    pcell_t *cells __unused, intptr_t *id)
{
	struct rk806_softc *sc;
	int i;

	if (ncells != 0)
		return (ERANGE);

	sc = device_get_softc(dev);
	for (i = 0; i < RK806_REGULATOR_COUNT; i++) {
		if (sc->regulators[i] != NULL &&
		    xref == OF_xref_from_node(
		    regnode_get_ofw_node(sc->regulators[i]))) {
			*id = i;
			return (0);
		}
	}

	return (ERANGE);
}

static int
rk806_attach(device_t dev)
{
	struct rk806_softc *sc;
	phandle_t node;
	uint8_t chip_name, chip_ver, old_sys_cfg3, power_en[6], sys_cfg3;
	uint32_t chip_id, reset_func;
	int error, i, len, nregs;

	sc = device_get_softc(dev);
	sc->dev = dev;
	node = ofw_bus_get_node(dev);

	error = rk806_read(dev, RK806_CHIP_NAME, &chip_name);
	if (error == 0)
		error = rk806_read(dev, RK806_CHIP_VER, &chip_ver);
	if (error == 0) {
		chip_id = ((uint32_t)chip_name << 8) | chip_ver;
		if ((chip_id & RK806_ID_MASK) != RK806_ID) {
			device_printf(dev,
			    "invalid read-only chip identity 0x%04x "
			    "(expected 0x806x)\n", chip_id);
			return (ENXIO);
		}
	}
	for (i = 0; error == 0 && i < nitems(power_en); i++)
		error = rk806_read(dev, RK806_POWER_EN0 + i, &power_en[i]);
	if (error == 0)
		error = rk806_read(dev, RK806_SYS_CFG3, &old_sys_cfg3);
	if (error != 0) {
		device_printf(dev, "read-only register probe failed: %d\n", error);
		return (error);
	}

	reset_func = 0;
	len = OF_getencprop(node, "pmic-reset-func", &reset_func,
	    sizeof(reset_func));
	if (len > 0 && len != sizeof(reset_func)) {
		device_printf(dev, "invalid pmic-reset-func length %d\n", len);
		return (EINVAL);
	}
	if (reset_func > 2) {
		device_printf(dev, "invalid pmic-reset-func %u\n", reset_func);
		return (EINVAL);
	}
	sys_cfg3 = (old_sys_cfg3 & ~RK806_RESET_FUNC_MASK) |
	    (reset_func << 6);
	if (sys_cfg3 != old_sys_cfg3) {
		error = rk806_write(dev, RK806_SYS_CFG3, sys_cfg3);
		if (error != 0)
			return (error);
	}
	error = rk806_read(dev, RK806_SYS_CFG3, &sys_cfg3);
	if (error != 0)
		return (error);
	if ((sys_cfg3 & RK806_RESET_FUNC_MASK) != (reset_func << 6))
		return (EIO);

	nregs = 0;
	for (i = 0; i < RK806_REGULATOR_COUNT; i++) {
		error = rk806_attach_regulator(sc, &rk806_reg_defs[i], i,
		    &sc->regulators[i]);
		if (error == ENOENT)
			continue;
		if (error != 0) {
			device_printf(dev, "cannot register %s regulator: %d\n",
			    rk806_reg_defs[i].node_name, error);
			return (error);
		}
		nregs++;
	}
	device_printf(dev,
	    "chip=0x%02x version=0x%02x regulators=%d/%d "
	    "power_en=%02x/%02x/%02x/%02x/%02x/%02x "
	    "sys_cfg3=0x%02x->0x%02x reset-func=%u source=%s\n",
	    chip_name, chip_ver, nregs, RK806_REGULATOR_COUNT,
	    power_en[0], power_en[1], power_en[2], power_en[3],
	    power_en[4], power_en[5], old_sys_cfg3, sys_cfg3, reset_func,
	    len > 0 ? "DT" : "default");
	if (OF_hasprop(node, "system-power-controller") ||
	    OF_hasprop(node, "rockchip,system-power-controller"))
		EVENTHANDLER_REGISTER(shutdown_final, rk806_poweroff, sc,
		    SHUTDOWN_PRI_LAST - 2);
	return (0);
}

static device_method_t rk806_methods[] = {
	DEVMETHOD(device_probe,		rk806_probe),
	DEVMETHOD(device_attach,	rk806_attach),
	DEVMETHOD(regdev_map,		rk806_regdev_map),

	DEVMETHOD_END
};

static driver_t rk806_driver = {
	"rk806",
	rk806_methods,
	sizeof(struct rk806_softc)
};

DRIVER_MODULE(rk806, spibus, rk806_driver, 0, 0);
MODULE_DEPEND(rk806, spibus, 1, 1, 1);
