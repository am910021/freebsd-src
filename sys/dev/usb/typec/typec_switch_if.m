#-
# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
#

#include <sys/bus.h>
#include <dev/usb/typec/usb_typec.h>

INTERFACE typec_switch;

CODE {
	static int
	typec_switch_default_set(device_t dev, enum typec_orientation orientation)
	{
		return (EOPNOTSUPP);
	}
};

METHOD int set {
	device_t dev;
	enum typec_orientation orientation;
} DEFAULT typec_switch_default_set;
