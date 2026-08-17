/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/linker_set.h>

#include <dev/usb/controller/dwc3_soc.h>

SET_DECLARE(dwc3_soc_set, const struct dwc3_soc_ops);

const struct dwc3_soc_ops *
dwc3_soc_find(device_t dev)
{
	const struct dwc3_soc_ops **ops;

	SET_FOREACH(ops, dwc3_soc_set) {
		if (*ops != NULL && (*ops)->match != NULL &&
		    (*ops)->match(dev))
			return (*ops);
	}
	return (NULL);
}
