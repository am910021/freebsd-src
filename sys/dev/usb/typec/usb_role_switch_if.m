#-
# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
#

#include <sys/bus.h>
#include <dev/usb/typec/usb_typec.h>

INTERFACE usb_role_switch;

CODE {
	static int
	usb_role_switch_default_set(device_t dev, enum usb_role role)
	{
		return (EOPNOTSUPP);
	}
};

METHOD int set {
	device_t dev;
	enum usb_role role;
} DEFAULT usb_role_switch_default_set;
