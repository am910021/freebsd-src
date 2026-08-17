/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 */

#ifndef _ARM64_ROCKCHIP_RK3588_HDMI_MODE_H_
#define _ARM64_ROCKCHIP_RK3588_HDMI_MODE_H_

#include <sys/types.h>

struct videomode;

int rk3588_hdmi_qp_get_edid(int port, uint8_t *edid, size_t capacity,
    size_t *length);
int rk3588_hdmi_qp_get_hpd(int port, int *connected);
int rk3588_hdmi_qp_get_mode(int port, struct videomode *mode);
int rk3588_hdmi_qp_prepare_output(int port);
int rk3588_hdmi_qp_set_output(int port, bool enable);
int rk3588_hdmi_qp_set_mode(int port, const struct videomode *mode);
int rk3588_hdptx_configure_mode(int port, const struct videomode *mode);
int rk3588_hdptx_disable(int port);
int rk3588_vop2_prepare_mode(int port);
int rk3588_vop2_configure_mode(int port);
void rk3588_vop2_hpd_changed(void);

#endif
