/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 */

#include <sys/cdefs.h>
#include "opt_platform.h"

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/callout.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/rman.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <sys/taskqueue.h>

#include <machine/resource.h>

#include <dev/iicbus/iicbus.h>
#include <dev/iicbus/iiconf.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#include <dev/ofw/ofw_graph.h>

#include <dev/extres/regulator/regulator.h>

#include <dev/usb/typec/usb_typec.h>

#include "typec_switch_if.h"
#include "usb_role_switch_if.h"

#define FUSB_DEVICE_ID		0x01
#define FUSB_SWITCHES0		0x02
#define  FUSB_CC1_PU_MEASURE	0x44
#define  FUSB_CC2_PU_MEASURE	0x88
#define FUSB_MEASURE		0x04
#define  FUSB_MDAC_RD		0x26
#define  FUSB_MDAC_RA		0x04
#define FUSB_CONTROL0		0x06
#define  FUSB_CONTROL0_MASKED	0x24
#define  FUSB_CONTROL0_UNMASKED	0x04
#define FUSB_CONTROL2		0x08
#define  FUSB_CONTROL2_DFP	0x06
#define  FUSB_CONTROL2_TOGGLE	0x07
#define FUSB_MASK		0x0a
#define  FUSB_MASK_COMP_ONLY	0xdf
#define FUSB_POWER		0x0b
#define  FUSB_POWER_ALL		0x0f
#define FUSB_RESET		0x0c
#define  FUSB_SW_RESET		0x01
#define FUSB_MASKA		0x0e
#define  FUSB_MASKA_TOGDONE	0xbf
#define FUSB_MASKB		0x0f
#define FUSB_STATUS1A		0x3d
#define  FUSB_TOGSS_MASK	0x38
#define  FUSB_TOGSS_SRC1	0x08
#define  FUSB_TOGSS_SRC2	0x10
#define FUSB_INTERRUPTA		0x3e
#define  FUSB_INT_TOGDONE	0x40
#define FUSB_INTERRUPTB		0x3f
#define FUSB_STATUS0		0x40
#define  FUSB_STATUS0_COMP	0x20
#define FUSB_INTERRUPT		0x42
#define  FUSB_INT_COMP_CHNG	0x20

#define FUSB_VSAFE0V_MS		650
#define FUSB_ROLE_RETRY_MS	50

enum fusb_cc_state {
	FUSB_CC_OPEN,
	FUSB_CC_RD,
	FUSB_CC_RA,
};

struct fusb302_softc {
	device_t	dev;
	phandle_t	node;
	struct intr_config_hook init_hook;
	struct resource	*irq_res;
	void		*irq_cookie;
	struct mtx	state_mtx;
	struct task	state_task;
	struct callout	state_callout;
	device_t	orientation_switch;
	device_t	role_switch;
	regulator_t	vbus_supply;
	int		irq_rid;
	int		orientation;
	int		desired_orientation;
	int		last_togss;
	int		irq_count;
	int		attach_count;
	int		detach_count;
	int		invalid_count;
	int		error_count;
	bool		attached;
	bool		desired_attached;
	bool		vbus_enabled;
};

static const struct ofw_compat_data fusb302_compat[] = {
	{ "fcs,fusb302", 1 },
	{ NULL, 0 }
};

static int
fusb_read(struct fusb302_softc *sc, uint8_t reg, uint8_t *value)
{

	return (iicdev_readfrom(sc->dev, reg, value, 1, IIC_INTRWAIT));
}

static int
fusb_write(struct fusb302_softc *sc, uint8_t reg, uint8_t value)
{

	return (iicdev_writeto(sc->dev, reg, &value, 1, IIC_INTRWAIT));
}

static int
fusb_clear_interrupts(struct fusb302_softc *sc)
{
	uint8_t value;
	int error;

	error = fusb_read(sc, FUSB_INTERRUPT, &value);
	if (error == 0)
		error = fusb_read(sc, FUSB_INTERRUPTA, &value);
	if (error == 0)
		error = fusb_read(sc, FUSB_INTERRUPTB, &value);
	return (error);
}

static int
fusb_start_toggle(struct fusb302_softc *sc)
{
	int error;

	sc->attached = false;
	sc->orientation = -1;

	error = fusb_write(sc, FUSB_CONTROL0, FUSB_CONTROL0_MASKED);
	if (error == 0)
		error = fusb_write(sc, FUSB_CONTROL2, FUSB_CONTROL2_DFP);
	if (error == 0)
		error = fusb_write(sc, FUSB_CONTROL2, 0);
	if (error == 0)
		error = fusb_write(sc, FUSB_MASK, 0xff);
	if (error == 0)
		error = fusb_write(sc, FUSB_MASKA, 0xff);
	if (error == 0)
		error = fusb_write(sc, FUSB_MASKB, 0xff);
	if (error == 0)
		error = fusb_write(sc, FUSB_SWITCHES0, 0);
	if (error == 0)
		error = fusb_clear_interrupts(sc);
	if (error == 0)
		error = fusb_write(sc, FUSB_CONTROL2, FUSB_CONTROL2_DFP);
	if (error == 0)
		error = fusb_write(sc, FUSB_MASKA, FUSB_MASKA_TOGDONE);
	if (error == 0)
		error = fusb_write(sc, FUSB_CONTROL2, FUSB_CONTROL2_TOGGLE);
	if (error == 0)
		error = fusb_write(sc, FUSB_CONTROL0, FUSB_CONTROL0_UNMASKED);
	return (error);
}

static int
fusb_measure_cc(struct fusb302_softc *sc, uint8_t switches,
    enum fusb_cc_state *state)
{
	uint8_t status;
	int error;

	error = fusb_write(sc, FUSB_SWITCHES0, switches);
	if (error == 0)
		error = fusb_write(sc, FUSB_MEASURE, FUSB_MDAC_RD);
	if (error != 0)
		return (error);
	DELAY(100);
	error = fusb_read(sc, FUSB_STATUS0, &status);
	if (error != 0)
		return (error);
	if ((status & FUSB_STATUS0_COMP) != 0) {
		*state = FUSB_CC_OPEN;
		return (0);
	}

	error = fusb_write(sc, FUSB_MEASURE, FUSB_MDAC_RA);
	if (error != 0)
		return (error);
	DELAY(100);
	error = fusb_read(sc, FUSB_STATUS0, &status);
	if (error != 0)
		return (error);
	*state = (status & FUSB_STATUS0_COMP) != 0 ? FUSB_CC_RD : FUSB_CC_RA;
	return (0);
}

static const char *
fusb_cc_name(enum fusb_cc_state state)
{

	switch (state) {
	case FUSB_CC_OPEN:
		return ("Open");
	case FUSB_CC_RD:
		return ("Rd");
	case FUSB_CC_RA:
		return ("Ra");
	}
	return ("invalid");
}

static int
fusb_set_disconnected(struct fusb302_softc *sc)
{
	int error, first_error;

	first_error = 0;
	if (sc->vbus_enabled) {
		error = regulator_disable(sc->vbus_supply);
		if (error == 0) {
			sc->vbus_enabled = false;
			/* FUSB302 cannot report vSafe0V; use the USB PD limit. */
			pause("fusbdsg", MAX(1, hz * FUSB_VSAFE0V_MS / 1000));
		} else if (first_error == 0)
			first_error = error;
	}
	error = USB_ROLE_SWITCH_SET(sc->role_switch, USB_ROLE_NONE);
	if (error != 0 && first_error == 0)
		first_error = error;
	if (error == 0) {
		error = TYPEC_SWITCH_SET(sc->orientation_switch,
		    TYPEC_ORIENTATION_NONE);
		if (error != 0 && first_error == 0)
			first_error = error;
	}
	return (first_error);
}

static int
fusb_set_connected(struct fusb302_softc *sc, int orientation)
{
	enum typec_orientation typec_orientation;
	int error;

	typec_orientation = orientation == 0 ? TYPEC_ORIENTATION_NORMAL :
	    TYPEC_ORIENTATION_REVERSE;

	/* Stop the old role before changing the SuperSpeed lane mapping. */
	error = USB_ROLE_SWITCH_SET(sc->role_switch, USB_ROLE_NONE);
	if (error != 0)
		return (error);
	error = TYPEC_SWITCH_SET(sc->orientation_switch, typec_orientation);
	if (error != 0)
		return (error);
	if (!sc->vbus_enabled) {
		error = regulator_enable(sc->vbus_supply);
		if (error != 0)
			goto fail;
		sc->vbus_enabled = true;
	}
	error = USB_ROLE_SWITCH_SET(sc->role_switch, USB_ROLE_HOST);
	if (error == 0)
		return (0);

fail:
	(void)fusb_set_disconnected(sc);
	return (error);
}

static void
fusb_state_retry(void *arg)
{
	struct fusb302_softc *sc;

	sc = arg;
	taskqueue_enqueue(taskqueue_thread, &sc->state_task);
}

static void
fusb_state_task(void *arg, int pending)
{
	struct fusb302_softc *sc;
	int error, orientation;
	bool attached;

	sc = arg;
	mtx_lock(&sc->state_mtx);
	attached = sc->desired_attached;
	orientation = sc->desired_orientation;
	mtx_unlock(&sc->state_mtx);

	error = attached ? fusb_set_connected(sc, orientation) :
	    fusb_set_disconnected(sc);
	if (error == EBUSY) {
		callout_reset(&sc->state_callout,
		    MAX(1, hz * FUSB_ROLE_RETRY_MS / 1000),
		    fusb_state_retry, sc);
		return;
	}
	callout_stop(&sc->state_callout);
	if (error != 0) {
		sc->error_count++;
		device_printf(sc->dev, "Type-C %s failed: %d\n",
		    attached ? "host attach" : "detach", error);
	} else
		device_printf(sc->dev, "Type-C port %s%s\n",
		    attached ? "active orientation=" : "inactive",
		    attached ? (orientation == 0 ? "normal" : "reverse") : "");
}

static void
fusb_schedule_state(struct fusb302_softc *sc, bool attached, int orientation)
{

	mtx_lock(&sc->state_mtx);
	sc->desired_attached = attached;
	sc->desired_orientation = orientation;
	mtx_unlock(&sc->state_mtx);
	taskqueue_enqueue(taskqueue_thread, &sc->state_task);
}

static int
fusb_handle_togdone(struct fusb302_softc *sc)
{
	enum fusb_cc_state active, other, cc1, cc2;
	uint8_t active_switch, other_switch, status0, status1a, togss;
	int error, orientation;

	error = fusb_read(sc, FUSB_STATUS1A, &status1a);
	if (error != 0)
		return (error);
	togss = status1a & FUSB_TOGSS_MASK;
	sc->last_togss = togss;
	if (togss == FUSB_TOGSS_SRC1) {
		orientation = 0;
		active_switch = FUSB_CC1_PU_MEASURE;
		other_switch = FUSB_CC2_PU_MEASURE;
	} else if (togss == FUSB_TOGSS_SRC2) {
		orientation = 1;
		active_switch = FUSB_CC2_PU_MEASURE;
		other_switch = FUSB_CC1_PU_MEASURE;
	} else {
		sc->invalid_count++;
		device_printf(sc->dev, "invalid TOGSS=0x%02x\n", togss);
		return (fusb_start_toggle(sc));
	}

	error = fusb_write(sc, FUSB_CONTROL0, FUSB_CONTROL0_MASKED);
	if (error == 0)
		error = fusb_write(sc, FUSB_MASK, 0xff);
	if (error == 0)
		error = fusb_measure_cc(sc, active_switch, &active);
	if (error == 0)
		error = fusb_write(sc, FUSB_CONTROL2, FUSB_CONTROL2_DFP);
	if (error == 0)
		error = fusb_write(sc, FUSB_CONTROL2, 0);
	if (error == 0)
		error = fusb_write(sc, FUSB_MASKA, 0xff);
	if (error == 0)
		error = fusb_measure_cc(sc, other_switch, &other);
	if (error != 0)
		return (error);

	if (orientation == 0) {
		cc1 = active;
		cc2 = other;
	} else {
		cc1 = other;
		cc2 = active;
	}
	if (cc1 == FUSB_CC_RD &&
	    (cc2 == FUSB_CC_OPEN || cc2 == FUSB_CC_RA)) {
		orientation = 0;
		active_switch = FUSB_CC1_PU_MEASURE;
	} else if (cc2 == FUSB_CC_RD &&
	    (cc1 == FUSB_CC_OPEN || cc1 == FUSB_CC_RA)) {
		orientation = 1;
		active_switch = FUSB_CC2_PU_MEASURE;
	} else {
		sc->invalid_count++;
		device_printf(sc->dev,
		    "rejected TOGSS orientation=%s "
		    "cc1=%s cc2=%s togss=0x%02x invalid=%d\n",
		    orientation == 0 ? "normal" : "reverse",
		    fusb_cc_name(cc1), fusb_cc_name(cc2), togss,
		    sc->invalid_count);
		return (fusb_start_toggle(sc));
	}

	error = fusb_write(sc, FUSB_SWITCHES0, active_switch);
	if (error == 0)
		error = fusb_write(sc, FUSB_MEASURE, FUSB_MDAC_RD);
	if (error == 0)
		error = fusb_write(sc, FUSB_MASK, FUSB_MASK_COMP_ONLY);
	if (error == 0)
		DELAY(100);
	if (error == 0)
		error = fusb_clear_interrupts(sc);
	if (error == 0)
		error = fusb_read(sc, FUSB_STATUS0, &status0);
	if (error == 0 && (status0 & FUSB_STATUS0_COMP) != 0) {
		sc->invalid_count++;
		device_printf(sc->dev,
		    "active CC opened "
		    "during attach validation; restarting toggle\n");
		return (fusb_start_toggle(sc));
	}
	if (error == 0)
		error = fusb_write(sc, FUSB_CONTROL0, FUSB_CONTROL0_UNMASKED);
	if (error != 0)
		return (error);

	sc->orientation = orientation;
	sc->attached = true;
	sc->attach_count++;
	device_printf(sc->dev,
	    "Type-C CC attach orientation=%s "
	    "cc1=%s cc2=%s togss=0x%02x count=%d\n",
	    orientation == 0 ? "normal" : "reverse", fusb_cc_name(cc1),
	    fusb_cc_name(cc2), togss, sc->attach_count);
	fusb_schedule_state(sc, true, orientation);
	return (0);
}

static void
fusb_fail(struct fusb302_softc *sc, const char *where, int error)
{

	sc->attached = false;
	sc->orientation = -1;
	sc->error_count++;
	device_printf(sc->dev,
	    "%s failed: %d; controller stopped\n",
	    where, error);
	fusb_schedule_state(sc, false, -1);
	(void)fusb_write(sc, FUSB_RESET, FUSB_SW_RESET);
}

static void
fusb302_intr(void *arg)
{
	struct fusb302_softc *sc;
	uint8_t interrupt, interrupta, interruptb, status0;
	int error;

	sc = arg;
	sc->irq_count++;
	error = fusb_read(sc, FUSB_INTERRUPT, &interrupt);
	if (error == 0)
		error = fusb_read(sc, FUSB_INTERRUPTA, &interrupta);
	if (error == 0)
		error = fusb_read(sc, FUSB_INTERRUPTB, &interruptb);
	if (error == 0)
		error = fusb_read(sc, FUSB_STATUS0, &status0);
	if (error != 0) {
		fusb_fail(sc, "IRQ snapshot", error);
		return;
	}

	if (bootverbose)
		device_printf(sc->dev,
		    "IRQ int=%02x inta=%02x intb=%02x status0=%02x count=%d\n",
		    interrupt, interrupta, interruptb, status0, sc->irq_count);
	if ((interrupta & FUSB_INT_TOGDONE) != 0) {
		error = fusb_handle_togdone(sc);
		if (error != 0)
			fusb_fail(sc, "TOGDONE", error);
		return;
	}

	if (sc->attached && (interrupt & FUSB_INT_COMP_CHNG) != 0 &&
	    (status0 & FUSB_STATUS0_COMP) != 0) {
		sc->attached = false;
		sc->orientation = -1;
		sc->detach_count++;
		device_printf(sc->dev, "Type-C detach count=%d\n",
		    sc->detach_count);
		fusb_schedule_state(sc, false, -1);
		error = fusb_start_toggle(sc);
		if (error != 0)
			fusb_fail(sc, "detach restart", error);
	}
}

static device_t
fusb_graph_find_provider(phandle_t node, const char *property)
{
	phandle_t endpoint, port, provider, remote;
	device_t dev;
	uint32_t index;

	for (index = 0; index < 4; index++) {
		port = ofw_graph_get_port_by_idx(node, index);
		if (port == 0)
			continue;
		endpoint = ofw_graph_get_endpoint_by_idx(port, 0);
		if (endpoint == 0)
			continue;
		remote = ofw_graph_get_remote_endpoint(endpoint);
		if (remote == 0)
			continue;
		provider = ofw_graph_get_remote_parent(remote);
		if (provider == 0 || !OF_hasprop(provider, property))
			continue;
		dev = OF_device_from_xref(OF_xref_from_node(provider));
		if (dev != NULL)
			return (dev);
	}
	return (NULL);
}

static int
fusb_get_links(struct fusb302_softc *sc)
{
	phandle_t connector;
	char data_role[16], power_role[16];
	int error;

	connector = ofw_bus_find_child(sc->node, "connector");
	if (connector == 0) {
		device_printf(sc->dev, "usb-c-connector child is missing\n");
		return (ENXIO);
	}

	memset(data_role, 0, sizeof(data_role));
	memset(power_role, 0, sizeof(power_role));
	(void)OF_getprop(connector, "data-role", data_role,
	    sizeof(data_role) - 1);
	(void)OF_getprop(connector, "power-role", power_role,
	    sizeof(power_role) - 1);
	if ((strcmp(data_role, "host") != 0 &&
	    strcmp(data_role, "dual") != 0) ||
	    (strcmp(power_role, "source") != 0 &&
	    strcmp(power_role, "dual") != 0)) {
		device_printf(sc->dev,
		    "only host/source policy is currently supported\n");
		return (EOPNOTSUPP);
	}

	sc->role_switch = fusb_graph_find_provider(connector,
	    "usb-role-switch");
	if (sc->role_switch == NULL)
		sc->role_switch = fusb_graph_find_provider(sc->node,
		    "usb-role-switch");
	sc->orientation_switch = fusb_graph_find_provider(connector,
	    "orientation-switch");
	if (sc->role_switch == NULL || sc->orientation_switch == NULL) {
		device_printf(sc->dev,
		    "Type-C graph providers are not attached\n");
		return (ENXIO);
	}

	error = regulator_get_by_ofw_property(sc->dev, sc->node,
	    "vbus-supply", &sc->vbus_supply);
	if (error != 0) {
		device_printf(sc->dev, "cannot get vbus-supply: %d\n", error);
		return (error);
	}
	return (0);
}

static int
fusb302_probe(device_t dev)
{

	if (!ofw_bus_status_okay(dev) ||
	    ofw_bus_search_compatible(dev, fusb302_compat)->ocd_data == 0)
		return (ENXIO);
	device_set_desc(dev, "FUSB302 Type-C source controller");
	return (BUS_PROBE_DEFAULT);
}

static void
fusb302_add_sysctls(struct fusb302_softc *sc)
{
	struct sysctl_ctx_list *ctx;
	struct sysctl_oid_list *children;

	ctx = device_get_sysctl_ctx(sc->dev);
	children = SYSCTL_CHILDREN(device_get_sysctl_tree(sc->dev));
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "orientation", CTLFLAG_RD,
	    &sc->orientation, 0, "-1 none, 0 normal/CC1, 1 reverse/CC2");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "last_togss", CTLFLAG_RD,
	    &sc->last_togss, 0, "Last raw TOGSS field");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "irq_count", CTLFLAG_RD,
	    &sc->irq_count, 0, "Interrupt count");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "attach_count", CTLFLAG_RD,
	    &sc->attach_count, 0, "Validated attach count");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "detach_count", CTLFLAG_RD,
	    &sc->detach_count, 0, "Comparator detach count");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "invalid_count", CTLFLAG_RD,
	    &sc->invalid_count, 0, "Rejected TOGDONE count");
	SYSCTL_ADD_INT(ctx, children, OID_AUTO, "error_count", CTLFLAG_RD,
	    &sc->error_count, 0, "Controller and role lifecycle error count");
}

static void
fusb302_init(void *arg)
{
	struct fusb302_softc *sc;
	uint8_t device_id;
	int error;

	sc = arg;
	config_intrhook_disestablish(&sc->init_hook);
	error = fusb_get_links(sc);
	if (error != 0) {
		sc->error_count++;
		return;
	}
	(void)regulator_stop(sc->vbus_supply);
	error = bus_activate_resource(sc->dev, SYS_RES_IRQ, sc->irq_rid,
	    sc->irq_res);
	if (error != 0) {
		sc->error_count++;
		device_printf(sc->dev,
		    "cannot activate IRQ %ju: "
		    "%d\n", (uintmax_t)rman_get_start(sc->irq_res), error);
		return;
	}
	if (bootverbose)
		device_printf(sc->dev, "IRQ %ju activated\n",
		    (uintmax_t)rman_get_start(sc->irq_res));

	error = fusb_read(sc, FUSB_DEVICE_ID, &device_id);
	if (error != 0) {
		sc->error_count++;
		device_printf(sc->dev, "cannot read device ID: %d\n", error);
		return;
	}
	error = fusb_write(sc, FUSB_RESET, FUSB_SW_RESET);
	if (error == 0)
		error = fusb_write(sc, FUSB_CONTROL0, FUSB_CONTROL0_MASKED);
	if (error == 0)
		error = fusb_write(sc, FUSB_POWER, FUSB_POWER_ALL);
	if (error == 0)
		error = fusb_write(sc, FUSB_MASK, 0xff);
	if (error == 0)
		error = fusb_write(sc, FUSB_MASKA, 0xff);
	if (error == 0)
		error = fusb_write(sc, FUSB_MASKB, 0xff);
	if (error == 0)
		error = fusb_clear_interrupts(sc);
	if (error != 0) {
		fusb_fail(sc, "deferred initialization", error);
		return;
	}

	error = bus_setup_intr(sc->dev, sc->irq_res,
	    INTR_TYPE_MISC | INTR_MPSAFE, NULL, fusb302_intr, sc,
	    &sc->irq_cookie);
	if (error != 0) {
		sc->error_count++;
		device_printf(sc->dev, "cannot setup interrupt: %d\n", error);
		(void)fusb_write(sc, FUSB_RESET, FUSB_SW_RESET);
		return;
	}

	error = fusb_start_toggle(sc);
	if (error != 0) {
		fusb_fail(sc, "initial toggle", error);
		return;
	}
	device_printf(sc->dev,
	    "Type-C host policy active id=0x%02x\n", device_id);
	return;
}

static int
fusb302_attach(device_t dev)
{
	struct fusb302_softc *sc;
	int error;

	sc = device_get_softc(dev);
	sc->dev = dev;
	sc->node = ofw_bus_get_node(dev);
	sc->orientation = -1;
	sc->desired_orientation = -1;
	mtx_init(&sc->state_mtx, "fusb302 state", NULL, MTX_DEF);
	TASK_INIT(&sc->state_task, 0, fusb_state_task, sc);
	callout_init(&sc->state_callout, 1);
	sc->irq_rid = 0;
	sc->irq_res = bus_alloc_resource_any(dev, SYS_RES_IRQ, &sc->irq_rid,
	    0);
	if (sc->irq_res == NULL) {
		device_printf(dev, "cannot allocate interrupt\n");
		mtx_destroy(&sc->state_mtx);
		return (ENXIO);
	}

	fusb302_add_sysctls(sc);
	sc->init_hook.ich_func = fusb302_init;
	sc->init_hook.ich_arg = sc;
	error = config_intrhook_establish(&sc->init_hook);
	if (error != 0) {
		bus_release_resource(dev, SYS_RES_IRQ, sc->irq_rid, sc->irq_res);
		sc->irq_res = NULL;
		mtx_destroy(&sc->state_mtx);
		return (ENOMEM);
	}
	return (0);
}

static int
fusb302_detach(device_t dev)
{
	struct fusb302_softc *sc;

	sc = device_get_softc(dev);
	config_intrhook_drain(&sc->init_hook);
	if (sc->irq_cookie != NULL)
		bus_teardown_intr(dev, sc->irq_res, sc->irq_cookie);
	callout_drain(&sc->state_callout);
	taskqueue_drain(taskqueue_thread, &sc->state_task);
	if (sc->role_switch != NULL && sc->orientation_switch != NULL)
		(void)fusb_set_disconnected(sc);
	if (sc->irq_cookie != NULL)
		(void)fusb_write(sc, FUSB_RESET, FUSB_SW_RESET);
	if (sc->vbus_supply != NULL)
		regulator_release(sc->vbus_supply);
	if (sc->irq_res != NULL)
		bus_release_resource(dev, SYS_RES_IRQ, sc->irq_rid, sc->irq_res);
	mtx_destroy(&sc->state_mtx);
	return (0);
}

static device_method_t fusb302_methods[] = {
	DEVMETHOD(device_probe,		fusb302_probe),
	DEVMETHOD(device_attach,	fusb302_attach),
	DEVMETHOD(device_detach,	fusb302_detach),

	DEVMETHOD_END
};

static driver_t fusb302_driver = {
	"fusb302",
	fusb302_methods,
	sizeof(struct fusb302_softc)
};

DRIVER_MODULE(fusb302, iicbus, fusb302_driver, 0, 0);
MODULE_DEPEND(fusb302, iicbus, IICBUS_MINVER, IICBUS_PREFVER,
    IICBUS_MAXVER);
IICBUS_FDT_PNP_INFO(fusb302_compat);
