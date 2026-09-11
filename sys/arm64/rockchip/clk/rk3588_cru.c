/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Fang, Wei-Xiang (Yuri) <am910021@gmail.com>
 *
 * Minimal RK3588 CRU bring-up support.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/rman.h>

#include <machine/bus.h>

#include <dev/fdt/simplebus.h>

#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include <arm64/rockchip/clk/rk_cru.h>

/* The shared helpers expect CRU_* wrappers around register offsets. */
#define	CRU_CLKSEL_CON(x)		(x)
#define	CRU_CLKGATE_CON(x)		(x)

#define	RK3588_CLKSEL_CON(x)		((x) * 0x4 + 0x300)
#define	RK3588_CLKGATE_CON(x)		((x) * 0x4 + 0x800)
#define	RK3588_SOFTRST_CON(x)		((x) * 0x4 + 0xa00)
#define	RK3588_PLL_CON(x)		((x) * 0x4)
#define	RK3588_MODE_CON0		0x280

#define	RK3588_PHP_CRU_BASE		0x8000
#define	RK3588_PMU_CRU_BASE		0x30000

#define	RK3588_PHP_CLKGATE_CON(x)	((x) * 0x4 + RK3588_PHP_CRU_BASE + 0x800)
#define	RK3588_PHP_SOFTRST_CON(x)	((x) * 0x4 + RK3588_PHP_CRU_BASE + 0xa00)
#define	RK3588_PMU_CLKGATE_CON(x)	((x) * 0x4 + RK3588_PMU_CRU_BASE + 0x800)
#define	RK3588_PMU_SOFTRST_CON(x)	((x) * 0x4 + RK3588_PMU_CRU_BASE + 0xa00)

/*
 * IDs used by the RK3588 vendor/mainline DTS.  Keep this local while the
 * bring-up driver only registers a small subset of the clock tree.
 */
#define	PLL_AUPLL			4
#define	HCLK_I2S0_8CH			40
#define	CLK_I2S0_8CH_TX_SRC		41
#define	CLK_I2S0_8CH_TX_FRAC		42
#define	MCLK_I2S0_8CH_TX		43
#define	CLK_I2S0_8CH_TX		44
#define	CLK_I2S0_8CH_RX_SRC		45
#define	CLK_I2S0_8CH_RX_FRAC		46
#define	MCLK_I2S0_8CH_RX		47
#define	CLK_I2S0_8CH_RX		48
#define	HCLK_AUDIO_ROOT			52
#define	PCLK_BUSTIMER0			84
#define	CLK_BUS_TIMER_ROOT		86
#define	CLK_BUSTIMER0			87
#define	ACLK_DMAC0			110
#define	PCLK_WDT0			99
#define	TCLK_WDT0			100
#define	ACLK_DMAC1			111
#define	ACLK_DMAC2			112
#define	ACLK_BUS_ROOT			113
#define	PCLK_GPIO1			115
#define	DBCLK_GPIO1			116
#define	PCLK_GPIO2			117
#define	DBCLK_GPIO2			118
#define	PCLK_GPIO3			119
#define	DBCLK_GPIO3			120
#define	PCLK_GPIO4			121
#define	DBCLK_GPIO4			122
#define	PCLK_I2C1			123
#define	PCLK_I2C2			124
#define	PCLK_I2C3			125
#define	PCLK_I2C4			126
#define	PCLK_I2C5			127
#define	PCLK_I2C6			128
#define	PCLK_I2C7			129
#define	PCLK_I2C8			130
#define	CLK_I2C1			131
#define	CLK_I2C2			132
#define	CLK_I2C3			133
#define	CLK_I2C4			134
#define	CLK_I2C5			135
#define	CLK_I2C6			136
#define	CLK_I2C7			137
#define	CLK_I2C8			138
#define	PCLK_OTPC_NS			139
#define	CLK_OTPC_NS			140
#define	CLK_OTPC_ARB			141
#define	CLK_OTP_PHY_G			143
#define	PCLK_SPI2			148
#define	CLK_SPI2			153
#define	PCLK_TSADC			157
#define	CLK_TSADC			158
#define	HCLK_EMMC			298
#define	ACLK_EMMC			299
#define	CCLK_EMMC			300
#define	BCLK_EMMC			301
#define	TMCLK_EMMC			302
#define	HCLK_NVM_ROOT			306
#define	ACLK_NVM_ROOT			307
#define	CLK_GMAC_125M			310
#define	CLK_GMAC_50M			311
#define	ACLK_MMU_PHP			314
#define	PCLK_GMAC0			344
#define	PCLK_GMAC1			345
#define	ACLK_GMAC0			349
#define	ACLK_GMAC1			350
#define	ACLK_USB3OTG2			360
#define	SUSPEND_CLK_USB3OTG2		361
#define	REF_CLK_USB3OTG2		362
#define	CLK_UTMI_OTG2			363
#define	ACLK_USB_ROOT			396
#define	HCLK_USB_ROOT			397
#define	HCLK_HOST0			398
#define	HCLK_HOST_ARB0			399
#define	HCLK_HOST1			400
#define	HCLK_HOST_ARB1			401
#define	ACLK_USB3OTG0			402
#define	SUSPEND_CLK_USB3OTG0		403
#define	REF_CLK_USB3OTG0		404
#define	ACLK_USB3OTG1			405
#define	SUSPEND_CLK_USB3OTG1		406
#define	REF_CLK_USB3OTG1		407
#define	PCLK_HDMITX0			528
#define	CLK_HDMITX0_EARC		529
#define	CLK_HDMITX0_REF			530
#define	PCLK_HDMITX1			531
#define	CLK_HDMITX1_EARC		532
#define	CLK_HDMITX1_REF			533
#define	CLK_HDMITRX_REFSRC		534
#define	ACLK_HDCP1_ROOT			537
#define	HCLK_VO1_ROOT			539
#define	PCLK_VO1_ROOT			541
#define	ACLK_VO1USB_TOP_ROOT		592
#define	HCLK_VO1USB_TOP_ROOT		593
#define	CLK_HDMIHDP0			594
#define	CLK_HDMIHDP1			595
#define	PCLK_HDPTX0			596
#define	PCLK_HDPTX1			597
#define	PCLK_USBDPPHY0			598
#define	PCLK_USBDPPHY1			599
#define	ACLK_VOP_ROOT			600
#define	HCLK_VOP_ROOT			602
#define	PCLK_VOP_ROOT			603
#define	HCLK_VOP			604
#define	ACLK_VOP			605
#define	DCLK_VOP0_SRC			606
#define	DCLK_VOP1_SRC			607
#define	DCLK_VOP2_SRC			608
#define	DCLK_VOP0			609
#define	DCLK_VOP1			610
#define	DCLK_VOP2			611
#define	DCLK_VOP3			612
#define	ACLK_VOP_SUB_SRC		619
#define	CLK_USBDP_PHY0_IMMORTAL		620
#define	CLK_USBDP_PHY1_IMMORTAL		621
#define	PCLK_I2C0			627
#define	CLK_I2C0			628
#define	CLK_USB2PHY_HDPTXRXPHY_REF	672
#define	CLK_USBDPPHY_MIPIDCPPHY_REF	673
#define	HCLK_NVM			703
#define	ACLK_USB			704
#define	HCLK_USB			705
#define	HCLK_VO1			717
#define	PCLK_GPIO0			625
#define	DBCLK_GPIO0			626

#define	CLK_CR_PARA			671
#define	ACLK_PCIE_4L_DBI		315
#define	ACLK_PCIE_2L_DBI		316
#define	ACLK_PCIE_1L0_DBI		317
#define	ACLK_PCIE_1L1_DBI		318
#define	ACLK_PCIE_1L2_DBI		319
#define	ACLK_PCIE_4L_MSTR		320
#define	ACLK_PCIE_2L_MSTR		321
#define	ACLK_PCIE_1L0_MSTR		322
#define	ACLK_PCIE_1L1_MSTR		323
#define	ACLK_PCIE_1L2_MSTR		324
#define	ACLK_PCIE_4L_SLV		325
#define	ACLK_PCIE_2L_SLV		326
#define	ACLK_PCIE_1L0_SLV		327
#define	ACLK_PCIE_1L1_SLV		328
#define	ACLK_PCIE_1L2_SLV		329
#define	PCLK_PCIE_4L			330
#define	PCLK_PCIE_2L			331
#define	PCLK_PCIE_1L0			332
#define	PCLK_PCIE_1L1			333
#define	PCLK_PCIE_1L2			334
#define	CLK_PCIE_AUX0			335
#define	CLK_PCIE_AUX1			336
#define	CLK_PCIE_AUX2			337
#define	CLK_PCIE_AUX3			338
#define	CLK_PCIE_AUX4			339
#define	CLK_PIPEPHY0_REF		340
#define	CLK_PMALIVE0			351
#define	ACLK_SATA0			354
#define	CLK_RXOOB0			357
#define	ACLK_MMU_PCIE			313
#define	CLK_PIPEPHY0_PIPE_G		364
#define	CLK_PIPEPHY1_PIPE_G		365
#define	CLK_PIPEPHY2_PIPE_G		366
#define	CLK_PIPEPHY0_PIPE_ASIC_G	367
#define	PCLK_PHP_ROOT			343
#define	ACLK_PCIE_ROOT			346
#define	ACLK_PHP_ROOT			347
#define	ACLK_PCIE_BRIDGE		348
#define	PCLK_PCIE_COMBO_PIPE_PHY0	374
#define	PCLK_PCIE_COMBO_PIPE_PHY1	375
#define	PCLK_PCIE_COMBO_PIPE_PHY2	376
#define	PCLK_PCIE_COMBO_PIPE_PHY		377
#define	CLK_REF_PIPE_PHY0		680
#define	CLK_REF_PIPE_PHY1		681
#define	CLK_REF_PIPE_PHY2		682
#define	CLK_PCIE1L2_PIPE		371
#define	CLK_PCIE4L_PIPE			372
#define	CLK_PCIE2L_PIPE			373
#define	CLK_PCIE1L0_PIPE		687
#define	CLK_PCIE1L1_PIPE		688

#define	SRST_P_TSADC			86
#define	SRST_TSADC			87
#define	SRST_M_I2S0_8CH_TX		42
#define	SRST_M_I2S0_8CH_RX		43
#define	SRST_A_GMAC0			291
#define	SRST_A_GMAC1			292
#define	SRST_PCIE0_POWER_UP		294
#define	SRST_PCIE1_POWER_UP		295
#define	SRST_PCIE2_POWER_UP		296
#define	SRST_PCIE3_POWER_UP		297
#define	SRST_PCIE4_POWER_UP		298
#define	SRST_P_PCIE0			299
#define	SRST_P_PCIE1			300
#define	SRST_P_PCIE2			301
#define	SRST_P_PCIE3			302
#define	SRST_P_PCIE4			303
#define	SRST_H_EMMC			278
#define	SRST_A_EMMC			279
#define	SRST_C_EMMC			280
#define	SRST_B_EMMC			281
#define	SRST_T_EMMC			282
#define	SRST_A_USB_BIU			336
#define	SRST_H_USB_BIU			337
#define	SRST_A_USB3OTG0			338
#define	SRST_A_USB3OTG1			339
#define	SRST_H_HOST0			340
#define	SRST_H_HOST_ARB0		341
#define	SRST_H_HOST1			342
#define	SRST_H_HOST_ARB1		343
#define	SRST_A_USB_GRF			344
#define	SRST_C_USB2P0_HOST0		345
#define	SRST_C_USB2P0_HOST1		346
#define	SRST_HOST_UTMI0			347
#define	SRST_HOST_UTMI1			348
#define	SRST_P_USB2PHY_U3_0_GRF0	541
#define	SRST_P_USB2PHY_U3_1_GRF0	542
#define	SRST_P_USB2PHY_U2_0_GRF0	543
#define	SRST_P_USB2PHY_U2_1_GRF0	544
#define	SRST_P_HDPTX0			538
#define	SRST_P_HDPTX1			539
#define	SRST_HDPTX0_INIT		611
#define	SRST_HDPTX0_CMN			612
#define	SRST_HDPTX0_LANE		613
#define	SRST_HDPTX1_INIT		614
#define	SRST_HDPTX1_CMN			615
#define	SRST_HDPTX1_LANE		616
#define	SRST_P_PCIE2_PHY0		579
#define	SRST_REF_PIPE_PHY0		572
#define	SRST_REF_PIPE_PHY1		573
#define	SRST_REF_PIPE_PHY2		574
#define	SRST_P_PCIE2_PHY1		580
#define	SRST_P_PCIE2_PHY2		581
#define	SRST_P_PCIE3_PHY		582
#define	SRST_PCIE30_PHY		584
#define	SRST_OTGPHY_U3_0		621
#define	SRST_OTGPHY_U3_1		622
#define	SRST_OTGPHY_U2_0		623
#define	SRST_OTGPHY_U2_1		624
#define	SRST_USBDP_COMBO_PHY0_INIT	11
#define	SRST_USBDP_COMBO_PHY0_CMN	12
#define	SRST_USBDP_COMBO_PHY0_LANE	13
#define	SRST_USBDP_COMBO_PHY0_PCS	14
#define	SRST_USBDP_COMBO_PHY1_INIT	15
#define	SRST_USBDP_COMBO_PHY1_CMN	16
#define	SRST_USBDP_COMBO_PHY1_LANE	17
#define	SRST_USBDP_COMBO_PHY1_PCS	18
#define	SRST_P_USBDPPHY0		535
#define	SRST_P_USBDPPHY1		537

PLIST(xin24m_p) = { "xin24m" };
PLIST(gpll_cpll_p) = { "gpll", "cpll" };
PLIST(gpll_aupll_p) = { "gpll", "aupll" };
PLIST(gpll_cpll_24m_p) = { "gpll", "cpll", "xin24m" };
PLIST(gpll_24m_p) = { "gpll", "xin24m" };
PLIST(mux_150m_50m_24m_p) =
    { "clk_150m_src", "clk_50m_src", "xin24m" };
PLIST(mux_150m_100m_24m_p) =
    { "clk_150m_src", "clk_100m_src", "xin24m" };
PLIST(mux_150m_100m_50m_24m_p) =
    { "clk_150m_src", "clk_100m_src", "clk_50m_src", "xin24m" };
PLIST(mux_200m_100m_50m_24m_p) =
    { "clk_200m_src", "clk_100m_src", "clk_50m_src", "xin24m" };
PLIST(mux_200m_150m_24m_p) =
    { "clk_200m_src", "clk_150m_src", "xin24m" };
PLIST(mux_100m_50m_24m_p) =
    { "clk_100m_src", "clk_50m_src", "xin24m" };
PLIST(mux_24m_100m_p) = { "xin24m", "clk_100m_src" };
PLIST(gpll_cpll_dmyaupll_npll_spll_p) =
    { "gpll", "cpll", "dummy_aupll", "npll", "spll" };
PLIST(gpll_cpll_v0pll_aupll_p) =
    { "gpll", "cpll", "v0pll", "aupll" };
PLIST(aclk_vop_sub_src_p) =
    { "aclk_vop_root", "aclk_vop_div2_src" };
PLIST(dclk_vop0_p) =
    { "dclk_vop0_src", "clk_hdmiphy_pixel0", "clk_hdmiphy_pixel1" };
PLIST(dclk_vop1_p) =
    { "dclk_vop1_src", "clk_hdmiphy_pixel0", "clk_hdmiphy_pixel1" };
PLIST(dclk_vop2_p) =
    { "dclk_vop2_src", "clk_hdmiphy_pixel0", "clk_hdmiphy_pixel1" };
PLIST(i2s0_tx_p) =
    { "clk_i2s0_8ch_tx_src", "clk_i2s0_8ch_tx_frac",
      "i2s0_mclkin", "xin12m" };
PLIST(i2s0_rx_p) =
    { "clk_i2s0_8ch_rx_src", "clk_i2s0_8ch_rx_frac",
      "i2s0_mclkin", "xin12m" };
PLIST(clk_ref_pipe_phy0_p) =
    { "clk_ref_pipe_phy0_osc_src", "clk_ref_pipe_phy0_pll_src" };
PLIST(clk_ref_pipe_phy1_p) =
    { "clk_ref_pipe_phy1_osc_src", "clk_ref_pipe_phy1_pll_src" };
PLIST(clk_ref_pipe_phy2_p) =
    { "clk_ref_pipe_phy2_osc_src", "clk_ref_pipe_phy2_pll_src" };
PLIST(aclk_hdcp1_root_p) = { "gpll", "cpll", "clk_hdmitrx_refsrc" };

#define	RK3588_PLL_RO(_name, _base, _shift)				\
{									\
	.type = RK3588_CLK_PLL,						\
	.clk.pll = &(struct rk_clk_pll_def) {				\
		.clkdef.id = 0,						\
		.clkdef.name = _name,					\
		.clkdef.parent_names = xin24m_p,				\
		.clkdef.parent_cnt = nitems(xin24m_p),			\
		.clkdef.flags = CLK_NODE_STATIC_STRINGS,			\
		.base_offset = _base,					\
		.mode_reg = RK3588_MODE_CON0,				\
		.mode_shift = _shift,					\
	},								\
}

static struct rk_clk rk3588_clks[] = {
	LINK("xin24m"),
	FRATE(0, "xin12m", 12000000),
	FRATE(0, "i2s0_mclkin", 0),
	FRATE(0, "clk_rtc_32k", 32768),
	/* U-Boot leaves AUPLL in its 24 MHz bypass state. */
	FRATE(PLL_AUPLL, "aupll", 24000000),
	FRATE(0, "gpll", 1188000000),
	FRATE(0, "cpll", 1500000000),
	FRATE(0, "ppll", 1100000000),
	FRATE(0, "clk_50m_src", 50000000),
	FRATE(0, "clk_100m_src", 100000000),
	FRATE(0, "clk_150m_src", 150000000),
	FRATE(0, "clk_200m_src", 198000000),
	FRATE(0, "clk_400m_src", 396000000),
	FRATE(0, "pclk_top_root", 100000000),
	FRATE(0, "pclk_pmu0_root", 100000000),
	FRATE(0, "hclk_nvm_root", 200000000),
	FRATE(ACLK_NVM_ROOT, "aclk_nvm_root", 400000000),
	COMP(0, "aclk_bus_root_c", gpll_cpll_p, 0,
	    RK3588_CLKSEL_CON(38), 0, 5, 5, 1),
	COMP(0, "clk_tsadc_c", gpll_24m_p, 0,
	    RK3588_CLKSEL_CON(41), 0, 8, 8, 1),
	MUX(0, "clk_spi2_sel", mux_200m_150m_24m_p, 0,
	    RK3588_CLKSEL_CON(59), 6, 2),
	COMP(0, "cclk_emmc_sel", gpll_cpll_24m_p, 0,
	    RK3588_CLKSEL_CON(77), 8, 6, 14, 2),
	COMP(0, "bclk_emmc_sel", gpll_cpll_p, 0,
	    RK3588_CLKSEL_CON(78), 0, 5, 5, 1),
	COMP(0, "aclk_vo1usb_top_root_sel", gpll_cpll_p, 0,
	    RK3588_CLKSEL_CON(170), 0, 5, 5, 1),
	MUX(0, "hclk_vo1usb_top_root_sel", mux_200m_100m_50m_24m_p, 0,
	    RK3588_CLKSEL_CON(170), 6, 2),
	COMP(0, "aclk_hdcp1_root_sel", aclk_hdcp1_root_p, 0,
	    RK3588_CLKSEL_CON(128), 0, 5, 5, 2),
	MUX(0, "hclk_vo1_root_sel", mux_200m_100m_50m_24m_p, 0,
	    RK3588_CLKSEL_CON(128), 13, 2),
	MUX(0, "pclk_vo1_root_sel", mux_150m_100m_24m_p, 0,
	    RK3588_CLKSEL_CON(129), 2, 2),
	MUX(0, "clk_bus_timer_root_sel", mux_24m_100m_p, 0,
	    RK3588_CLKSEL_CON(60), 2, 1),
	COMP(0, "clk_hdmitx0_earc_sel", gpll_cpll_p, 0,
	    RK3588_CLKSEL_CON(133), 1, 5, 6, 1),
	COMP(0, "clk_hdmitx1_earc_sel", gpll_cpll_p, 0,
	    RK3588_CLKSEL_CON(136), 1, 5, 6, 1),
	COMP(0, "clk_hdmitrx_refsrc_sel", gpll_cpll_p,
	    RK_CLK_COMPOSITE_DIV_HALF, RK3588_CLKSEL_CON(157), 2, 5, 7, 1),
	COMP(0, "clk_utmi_otg2_sel", mux_150m_50m_24m_p, 0,
	    RK3588_CLKSEL_CON(84), 8, 4, 12, 2),
	COMP(0, "clk_rxoob0_c", gpll_cpll_p, 0,
	    RK3588_CLKSEL_CON(82), 0, 7, 7, 1),
	COMP(0, "clk_gmac_125m_sel", gpll_cpll_p, 0,
	    RK3588_CLKSEL_CON(83), 8, 7, 15, 1),
	COMP(0, "clk_gmac_50m_sel", gpll_cpll_p, 0,
	    RK3588_CLKSEL_CON(84), 0, 7, 7, 1),
	COMP(0, "aclk_usb_root", gpll_cpll_p, 0,
	    RK3588_CLKSEL_CON(96), 0, 5, 5, 1),
	MUX(0, "hclk_usb_root", mux_150m_100m_50m_24m_p, 0,
	    RK3588_CLKSEL_CON(96), 6, 2),
	MUX(0, "hclk_audio_root_sel", mux_200m_100m_50m_24m_p, 0,
	    RK3588_CLKSEL_CON(24), 0, 2),
	COMP(0, "clk_i2s0_8ch_tx_src_c", gpll_aupll_p, 0,
	    RK3588_CLKSEL_CON(24), 4, 5, 9, 1),
	FRACT(0, "clk_i2s0_8ch_tx_frac_c", "clk_i2s0_8ch_tx_src", 0,
	    RK3588_CLKSEL_CON(25)),
	MUX(CLK_I2S0_8CH_TX, "clk_i2s0_8ch_tx", i2s0_tx_p, 0,
	    RK3588_CLKSEL_CON(26), 0, 2),
	COMP(0, "clk_i2s0_8ch_rx_src_c", gpll_aupll_p, 0,
	    RK3588_CLKSEL_CON(26), 2, 5, 7, 1),
	FRACT(0, "clk_i2s0_8ch_rx_frac_c", "clk_i2s0_8ch_rx_src", 0,
	    RK3588_CLKSEL_CON(27)),
	MUX(CLK_I2S0_8CH_RX, "clk_i2s0_8ch_rx", i2s0_rx_p, 0,
	    RK3588_CLKSEL_CON(28), 0, 2),
	FRATE(ACLK_PCIE_ROOT, "aclk_pcie_root", 500000000),
	FRATE(PCLK_PHP_ROOT, "pclk_php_root", 150000000),
	FRATE(ACLK_PHP_ROOT, "aclk_php_root", 396000000),
	FRATE(ACLK_PCIE_BRIDGE, "aclk_pcie_bridge", 500000000),
	FRATE(ACLK_MMU_PCIE, "aclk_mmu_pcie", 500000000),
	FRATE(0, "clk_pipephy0_pipe_i", 100000000),
	FRATE(0, "clk_pipephy1_pipe_i", 100000000),
	FRATE(0, "clk_pipephy2_pipe_i", 100000000),
	/* Linux models this mux input as an intentionally absent parent. */
	FRATE(0, "dummy_aupll", 0),
	RK3588_PLL_RO("npll", RK3588_PLL_CON(120), 0),
	LINK("spll"),
	RK3588_PLL_RO("v0pll", RK3588_PLL_CON(88), 4),
	LINK("clk_hdmiphy_pixel0"),
	LINK("clk_hdmiphy_pixel1"),
	COMP(0, "aclk_vop_root_sel", gpll_cpll_dmyaupll_npll_spll_p, 0,
	    RK3588_CLKSEL_CON(110), 0, 5, 5, 3),
	FFACT(0, "aclk_vop_div2_src", "aclk_vop_root", 1, 2),
	MUX(ACLK_VOP_SUB_SRC, "aclk_vop_sub_src", aclk_vop_sub_src_p, 0,
	    RK3588_CLKSEL_CON(115), 9, 1),
	MUX(0, "hclk_vop_root_sel", mux_200m_100m_50m_24m_p, 0,
	    RK3588_CLKSEL_CON(110), 10, 2),
	MUX(0, "pclk_vop_root_sel", mux_100m_50m_24m_p, 0,
	    RK3588_CLKSEL_CON(110), 12, 2),
	COMP(0, "dclk_vop0_src_sel", gpll_cpll_v0pll_aupll_p, 0,
	    RK3588_CLKSEL_CON(111), 0, 7, 7, 2),
	COMP(0, "dclk_vop1_src_sel", gpll_cpll_v0pll_aupll_p, 0,
	    RK3588_CLKSEL_CON(111), 9, 5, 14, 2),
	COMP(0, "dclk_vop2_src_sel", gpll_cpll_v0pll_aupll_p, 0,
	    RK3588_CLKSEL_CON(112), 0, 5, 5, 2),
	MUX(0, "dclk_vop0_sel", dclk_vop0_p, 0,
	    RK3588_CLKSEL_CON(112), 7, 2),
	MUX(0, "dclk_vop1_sel", dclk_vop1_p, 0,
	    RK3588_CLKSEL_CON(112), 9, 2),
	MUX(0, "dclk_vop2_sel", dclk_vop2_p, 0,
	    RK3588_CLKSEL_CON(112), 11, 2),
	COMP(0, "dclk_vop3_sel", gpll_cpll_v0pll_aupll_p, 0,
	    RK3588_CLKSEL_CON(113), 0, 7, 7, 2),
	CDIV(0, "clk_ref_pipe_phy0_pll_div", "ppll", 0,
	    RK3588_CLKSEL_CON(176), 0, 6),
	CDIV(0, "clk_ref_pipe_phy1_pll_div", "ppll", 0,
	    RK3588_CLKSEL_CON(176), 6, 6),
	CDIV(0, "clk_ref_pipe_phy2_pll_div", "ppll", 0,
	    RK3588_CLKSEL_CON(177), 0, 6),
	MUX(CLK_REF_PIPE_PHY0, "clk_ref_pipe_phy0", clk_ref_pipe_phy0_p,
	    RK_CLK_MUX_REPARENT, RK3588_CLKSEL_CON(177), 6, 1),
	MUX(CLK_REF_PIPE_PHY1, "clk_ref_pipe_phy1", clk_ref_pipe_phy1_p,
	    RK_CLK_MUX_REPARENT, RK3588_CLKSEL_CON(177), 7, 1),
	MUX(CLK_REF_PIPE_PHY2, "clk_ref_pipe_phy2", clk_ref_pipe_phy2_p,
	    RK_CLK_MUX_REPARENT, RK3588_CLKSEL_CON(177), 8, 1),
};

static struct rk_cru_gate rk3588_gates[] = {
	GATE(PCLK_GPIO0, "pclk_gpio0", "pclk_pmu0_root", RK3588_PMU_CLKGATE_CON(5), 5),
	GATE(DBCLK_GPIO0, "dbclk_gpio0", "xin24m", RK3588_PMU_CLKGATE_CON(5), 6),
	GATE(PCLK_WDT0, "pclk_wdt0", "pclk_top_root", RK3588_CLKGATE_CON(15), 0),
	GATE(TCLK_WDT0, "tclk_wdt0", "xin24m", RK3588_CLKGATE_CON(15), 1),
	GATE(PCLK_BUSTIMER0, "pclk_bustimer0", "pclk_top_root",
	    RK3588_CLKGATE_CON(15), 12),
	GATE(CLK_BUS_TIMER_ROOT, "clk_bus_timer_root",
	    "clk_bus_timer_root_sel", RK3588_CLKGATE_CON(15), 14),
	GATE(CLK_BUSTIMER0, "clk_bustimer0", "clk_bus_timer_root",
	    RK3588_CLKGATE_CON(15), 15),
	GATE(PCLK_OTPC_NS, "pclk_otpc_ns", "pclk_top_root", RK3588_CLKGATE_CON(18), 9),
	GATE(CLK_OTPC_NS, "clk_otpc_ns", "xin24m", RK3588_CLKGATE_CON(18), 10),
	GATE(CLK_OTPC_ARB, "clk_otpc_arb", "xin24m", RK3588_CLKGATE_CON(18), 11),
	GATE(CLK_OTP_PHY_G, "clk_otp_phy_g", "xin24m", RK3588_CLKGATE_CON(18), 13),
	GATE(PCLK_GPIO1, "pclk_gpio1", "pclk_top_root", RK3588_CLKGATE_CON(16), 14),
	GATE(DBCLK_GPIO1, "dbclk_gpio1", "xin24m", RK3588_CLKGATE_CON(16), 15),
	GATE(PCLK_GPIO2, "pclk_gpio2", "pclk_top_root", RK3588_CLKGATE_CON(17), 0),
	GATE(DBCLK_GPIO2, "dbclk_gpio2", "xin24m", RK3588_CLKGATE_CON(17), 1),
	GATE(PCLK_GPIO3, "pclk_gpio3", "pclk_top_root", RK3588_CLKGATE_CON(17), 2),
	GATE(DBCLK_GPIO3, "dbclk_gpio3", "xin24m", RK3588_CLKGATE_CON(17), 3),
	GATE(PCLK_GPIO4, "pclk_gpio4", "pclk_top_root", RK3588_CLKGATE_CON(17), 4),
	GATE(DBCLK_GPIO4, "dbclk_gpio4", "xin24m", RK3588_CLKGATE_CON(17), 5),

	GATE(PCLK_I2C0, "pclk_i2c0", "pclk_pmu0_root", RK3588_PMU_CLKGATE_CON(2), 1),
	GATE(CLK_I2C0, "clk_i2c0", "pclk_pmu0_root", RK3588_PMU_CLKGATE_CON(2), 2),
	GATE(PCLK_I2C1, "pclk_i2c1", "pclk_top_root", RK3588_CLKGATE_CON(10), 8),
	GATE(PCLK_I2C2, "pclk_i2c2", "pclk_top_root", RK3588_CLKGATE_CON(10), 9),
	GATE(PCLK_I2C3, "pclk_i2c3", "pclk_top_root", RK3588_CLKGATE_CON(10), 10),
	GATE(PCLK_I2C4, "pclk_i2c4", "pclk_top_root", RK3588_CLKGATE_CON(10), 11),
	GATE(PCLK_I2C5, "pclk_i2c5", "pclk_top_root", RK3588_CLKGATE_CON(10), 12),
	GATE(PCLK_I2C6, "pclk_i2c6", "pclk_top_root", RK3588_CLKGATE_CON(10), 13),
	GATE(PCLK_I2C7, "pclk_i2c7", "pclk_top_root", RK3588_CLKGATE_CON(10), 14),
	GATE(PCLK_I2C8, "pclk_i2c8", "pclk_top_root", RK3588_CLKGATE_CON(10), 15),
	GATE(ACLK_BUS_ROOT, "aclk_bus_root", "aclk_bus_root_c", RK3588_CLKGATE_CON(10), 0),
	GATE(ACLK_DMAC0, "aclk_dmac0", "aclk_bus_root", RK3588_CLKGATE_CON(10), 5),
	GATE(ACLK_DMAC1, "aclk_dmac1", "aclk_bus_root", RK3588_CLKGATE_CON(10), 6),
	GATE(ACLK_DMAC2, "aclk_dmac2", "aclk_bus_root", RK3588_CLKGATE_CON(10), 7),
	GATE(CLK_I2C1, "clk_i2c1", "pclk_top_root", RK3588_CLKGATE_CON(11), 0),
	GATE(CLK_I2C2, "clk_i2c2", "pclk_top_root", RK3588_CLKGATE_CON(11), 1),
	GATE(CLK_I2C3, "clk_i2c3", "pclk_top_root", RK3588_CLKGATE_CON(11), 2),
	GATE(CLK_I2C4, "clk_i2c4", "pclk_top_root", RK3588_CLKGATE_CON(11), 3),
	GATE(CLK_I2C5, "clk_i2c5", "pclk_top_root", RK3588_CLKGATE_CON(11), 4),
	GATE(CLK_I2C6, "clk_i2c6", "pclk_top_root", RK3588_CLKGATE_CON(11), 5),
	GATE(CLK_I2C7, "clk_i2c7", "pclk_top_root", RK3588_CLKGATE_CON(11), 6),
	GATE(CLK_I2C8, "clk_i2c8", "pclk_top_root", RK3588_CLKGATE_CON(11), 7),
	GATE(HCLK_AUDIO_ROOT, "hclk_audio_root", "hclk_audio_root_sel",
	    RK3588_CLKGATE_CON(7), 0),
	GATE(HCLK_I2S0_8CH, "hclk_i2s0_8ch", "hclk_audio_root",
	    RK3588_CLKGATE_CON(7), 4),
	GATE(CLK_I2S0_8CH_TX_SRC, "clk_i2s0_8ch_tx_src",
	    "clk_i2s0_8ch_tx_src_c", RK3588_CLKGATE_CON(7), 5),
	GATE(CLK_I2S0_8CH_TX_FRAC, "clk_i2s0_8ch_tx_frac",
	    "clk_i2s0_8ch_tx_frac_c", RK3588_CLKGATE_CON(7), 6),
	GATE(MCLK_I2S0_8CH_TX, "mclk_i2s0_8ch_tx", "clk_i2s0_8ch_tx",
	    RK3588_CLKGATE_CON(7), 7),
	GATE(CLK_I2S0_8CH_RX_SRC, "clk_i2s0_8ch_rx_src",
	    "clk_i2s0_8ch_rx_src_c", RK3588_CLKGATE_CON(7), 8),
	GATE(CLK_I2S0_8CH_RX_FRAC, "clk_i2s0_8ch_rx_frac",
	    "clk_i2s0_8ch_rx_frac_c", RK3588_CLKGATE_CON(7), 9),
	GATE(MCLK_I2S0_8CH_RX, "mclk_i2s0_8ch_rx", "clk_i2s0_8ch_rx",
	    RK3588_CLKGATE_CON(7), 10),
	GATE(PCLK_SPI2, "pclk_spi2", "pclk_top_root", RK3588_CLKGATE_CON(14), 8),
	GATE(CLK_SPI2, "clk_spi2", "clk_spi2_sel", RK3588_CLKGATE_CON(14), 13),

	GATE(PCLK_TSADC, "pclk_tsadc", "pclk_top_root", RK3588_CLKGATE_CON(12), 0),
	GATE(CLK_TSADC, "clk_tsadc", "clk_tsadc_c", RK3588_CLKGATE_CON(12), 1),

	GATE(HCLK_NVM, "hclk_nvm", "hclk_nvm_root", RK3588_CLKGATE_CON(31), 2),
	GATE(HCLK_EMMC, "hclk_emmc", "hclk_nvm", RK3588_CLKGATE_CON(31), 4),
	GATE(ACLK_EMMC, "aclk_emmc", "aclk_nvm_root", RK3588_CLKGATE_CON(31), 5),
	GATE(CCLK_EMMC, "cclk_emmc", "cclk_emmc_sel", RK3588_CLKGATE_CON(31), 6),
	GATE(BCLK_EMMC, "bclk_emmc", "bclk_emmc_sel", RK3588_CLKGATE_CON(31), 7),
	GATE(TMCLK_EMMC, "tmclk_emmc", "xin24m", RK3588_CLKGATE_CON(31), 8),

	GATE(ACLK_MMU_PHP, "aclk_mmu_php", "aclk_php_root", RK3588_CLKGATE_CON(34), 8),
	GATE(PCLK_GMAC0, "pclk_gmac0", "pclk_php_root", RK3588_CLKGATE_CON(32), 3),
	GATE(PCLK_GMAC1, "pclk_gmac1", "pclk_php_root", RK3588_CLKGATE_CON(32), 4),
	GATE(ACLK_GMAC0, "aclk_gmac0", "aclk_mmu_php", RK3588_CLKGATE_CON(32), 10),
	GATE(ACLK_GMAC1, "aclk_gmac1", "aclk_mmu_php", RK3588_CLKGATE_CON(32), 11),
	GATE(CLK_GMAC_125M, "clk_gmac_125m", "clk_gmac_125m_sel", RK3588_CLKGATE_CON(35), 5),
	GATE(CLK_GMAC_50M, "clk_gmac_50m", "clk_gmac_50m_sel", RK3588_CLKGATE_CON(35), 6),
	GATE(ACLK_HDCP1_ROOT, "aclk_hdcp1_root", "aclk_hdcp1_root_sel", RK3588_CLKGATE_CON(59), 0),
	GATE(HCLK_VO1_ROOT, "hclk_vo1_root", "hclk_vo1_root_sel", RK3588_CLKGATE_CON(59), 2),
	GATE(PCLK_VO1_ROOT, "pclk_vo1_root", "pclk_vo1_root_sel", RK3588_CLKGATE_CON(59), 4),
	GATE(HCLK_VO1, "hclk_vo1", "hclk_vo1_root", RK3588_CLKGATE_CON(59), 9),
	GATE(PCLK_HDMITX0, "pclk_hdmitx0", "pclk_vo1_root", RK3588_CLKGATE_CON(60), 11),
	GATE(CLK_HDMITX0_EARC, "clk_hdmitx0_earc", "clk_hdmitx0_earc_sel", RK3588_CLKGATE_CON(60), 15),
	GATE(CLK_HDMITX0_REF, "clk_hdmitx0_ref", "aclk_hdcp1_root", RK3588_CLKGATE_CON(61), 0),
	GATE(PCLK_HDMITX1, "pclk_hdmitx1", "pclk_vo1_root", RK3588_CLKGATE_CON(61), 2),
	GATE(CLK_HDMITX1_EARC, "clk_hdmitx1_earc", "clk_hdmitx1_earc_sel", RK3588_CLKGATE_CON(61), 6),
	GATE(CLK_HDMITX1_REF, "clk_hdmitx1_ref", "aclk_hdcp1_root", RK3588_CLKGATE_CON(61), 7),
	GATE(CLK_HDMITRX_REFSRC, "clk_hdmitrx_refsrc", "clk_hdmitrx_refsrc_sel", RK3588_CLKGATE_CON(65), 9),
	GATE(ACLK_VO1USB_TOP_ROOT, "aclk_vo1usb_top_root", "aclk_vo1usb_top_root_sel", RK3588_CLKGATE_CON(74), 0),
	GATE(HCLK_VO1USB_TOP_ROOT, "hclk_vo1usb_top_root", "hclk_vo1usb_top_root_sel", RK3588_CLKGATE_CON(74), 2),
	GATE(CLK_HDMIHDP0, "clk_hdmihdp0", "xin24m", RK3588_CLKGATE_CON(73), 12),
	GATE(CLK_HDMIHDP1, "clk_hdmihdp1", "xin24m", RK3588_CLKGATE_CON(73), 13),
	GATE(ACLK_VOP_ROOT, "aclk_vop_root", "aclk_vop_root_sel", RK3588_CLKGATE_CON(52), 0),
	GATE(HCLK_VOP_ROOT, "hclk_vop_root", "hclk_vop_root_sel", RK3588_CLKGATE_CON(52), 2),
	GATE(PCLK_VOP_ROOT, "pclk_vop_root", "pclk_vop_root_sel", RK3588_CLKGATE_CON(52), 3),
	GATE(HCLK_VOP, "hclk_vop", "hclk_vop_root", RK3588_CLKGATE_CON(52), 8),
	GATE(ACLK_VOP, "aclk_vop", "aclk_vop_sub_src", RK3588_CLKGATE_CON(52), 9),
	GATE(DCLK_VOP0_SRC, "dclk_vop0_src", "dclk_vop0_src_sel", RK3588_CLKGATE_CON(52), 10),
	GATE(DCLK_VOP1_SRC, "dclk_vop1_src", "dclk_vop1_src_sel", RK3588_CLKGATE_CON(52), 11),
	GATE(DCLK_VOP2_SRC, "dclk_vop2_src", "dclk_vop2_src_sel", RK3588_CLKGATE_CON(52), 12),
	GATE(DCLK_VOP0, "dclk_vop0", "dclk_vop0_sel", RK3588_CLKGATE_CON(52), 13),
	GATE(DCLK_VOP1, "dclk_vop1", "dclk_vop1_sel", RK3588_CLKGATE_CON(53), 0),
	GATE(DCLK_VOP2, "dclk_vop2", "dclk_vop2_sel", RK3588_CLKGATE_CON(53), 1),
	GATE(DCLK_VOP3, "dclk_vop3", "dclk_vop3_sel", RK3588_CLKGATE_CON(53), 2),
	GATE(ACLK_USB_ROOT, "aclk_usb_root_gate", "aclk_usb_root", RK3588_CLKGATE_CON(42), 0),
	GATE(HCLK_USB_ROOT, "hclk_usb_root_gate", "hclk_usb_root", RK3588_CLKGATE_CON(42), 1),
	GATE(ACLK_USB, "aclk_usb", "aclk_usb_root_gate", RK3588_CLKGATE_CON(42), 2),
	GATE(HCLK_USB, "hclk_usb", "hclk_usb_root_gate", RK3588_CLKGATE_CON(42), 3),
	GATE(ACLK_USB3OTG0, "aclk_usb3otg0", "aclk_usb", RK3588_CLKGATE_CON(42), 4),
	GATE(SUSPEND_CLK_USB3OTG0, "suspend_clk_usb3otg0", "xin24m", RK3588_CLKGATE_CON(42), 5),
	GATE(REF_CLK_USB3OTG0, "ref_clk_usb3otg0", "xin24m", RK3588_CLKGATE_CON(42), 6),
	GATE(ACLK_USB3OTG1, "aclk_usb3otg1", "aclk_usb", RK3588_CLKGATE_CON(42), 7),
	GATE(SUSPEND_CLK_USB3OTG1, "suspend_clk_usb3otg1", "xin24m", RK3588_CLKGATE_CON(42), 8),
	GATE(REF_CLK_USB3OTG1, "ref_clk_usb3otg1", "xin24m", RK3588_CLKGATE_CON(42), 9),
	GATE(HCLK_HOST0, "hclk_host0", "hclk_usb", RK3588_CLKGATE_CON(42), 10),
	GATE(HCLK_HOST_ARB0, "hclk_host_arb0", "hclk_usb", RK3588_CLKGATE_CON(42), 11),
	GATE(HCLK_HOST1, "hclk_host1", "hclk_usb", RK3588_CLKGATE_CON(42), 12),
	GATE(HCLK_HOST_ARB1, "hclk_host_arb1", "hclk_usb", RK3588_CLKGATE_CON(42), 13),
	GATE(ACLK_USB3OTG2, "aclk_usb3otg2", "aclk_mmu_php", RK3588_CLKGATE_CON(35), 7),
	GATE(SUSPEND_CLK_USB3OTG2, "suspend_clk_usb3otg2", "xin24m", RK3588_CLKGATE_CON(35), 8),
	GATE(REF_CLK_USB3OTG2, "ref_clk_usb3otg2", "xin24m", RK3588_CLKGATE_CON(35), 9),
	GATE(CLK_UTMI_OTG2, "clk_utmi_otg2", "clk_utmi_otg2_sel", RK3588_CLKGATE_CON(35), 10),
	GATE(CLK_USB2PHY_HDPTXRXPHY_REF, "clk_usb2phy_hdptxrxphy_ref", "xin24m", RK3588_PMU_CLKGATE_CON(4), 7),
	GATE(PCLK_USBDPPHY0, "pclk_usbdpphy0", "pclk_top_root", RK3588_CLKGATE_CON(72), 2),
	GATE(PCLK_USBDPPHY1, "pclk_usbdpphy1", "pclk_top_root", RK3588_CLKGATE_CON(72), 4),
	GATE(PCLK_HDPTX0, "pclk_hdptx0", "pclk_top_root", RK3588_CLKGATE_CON(72), 5),
	GATE(PCLK_HDPTX1, "pclk_hdptx1", "pclk_top_root", RK3588_CLKGATE_CON(72), 6),
	GATE(CLK_USBDP_PHY0_IMMORTAL, "clk_usbdp_phy0_immortal", "xin24m", RK3588_CLKGATE_CON(2), 8),
	GATE(CLK_USBDP_PHY1_IMMORTAL, "clk_usbdp_phy1_immortal", "xin24m", RK3588_CLKGATE_CON(2), 15),
	GATE(CLK_CR_PARA, "clk_cr_para", "xin24m", RK3588_PMU_CLKGATE_CON(4), 11),
	GATE(CLK_USBDPPHY_MIPIDCPPHY_REF, "clk_usbdpphy_mipidcpphy_ref", "xin24m", RK3588_PMU_CLKGATE_CON(4), 3),
	GATE(0, "clk_ref_pipe_phy0_osc_src", "xin24m", RK3588_CLKGATE_CON(77), 0),
	GATE(0, "clk_ref_pipe_phy1_osc_src", "xin24m", RK3588_CLKGATE_CON(77), 1),
	GATE(0, "clk_ref_pipe_phy2_osc_src", "xin24m", RK3588_CLKGATE_CON(77), 2),
	GATE(0, "clk_ref_pipe_phy0_pll_src", "clk_ref_pipe_phy0_pll_div", RK3588_CLKGATE_CON(77), 3),
	GATE(0, "clk_ref_pipe_phy1_pll_src", "clk_ref_pipe_phy1_pll_div", RK3588_CLKGATE_CON(77), 4),
	GATE(0, "clk_ref_pipe_phy2_pll_src", "clk_ref_pipe_phy2_pll_div", RK3588_CLKGATE_CON(77), 5),

	GATE(ACLK_PCIE_ROOT, "aclk_pcie_root_gate", "aclk_pcie_root", RK3588_CLKGATE_CON(32), 6),
	GATE(ACLK_PCIE_BRIDGE, "aclk_pcie_bridge_gate", "aclk_pcie_bridge", RK3588_CLKGATE_CON(32), 8),
	GATE(ACLK_PCIE_4L_DBI, "aclk_pcie_4l_dbi", "aclk_php_root", RK3588_CLKGATE_CON(32), 13),
	GATE(ACLK_PCIE_2L_DBI, "aclk_pcie_2l_dbi", "aclk_php_root", RK3588_CLKGATE_CON(32), 14),
	GATE(ACLK_PCIE_1L0_DBI, "aclk_pcie_1l0_dbi", "aclk_php_root", RK3588_CLKGATE_CON(32), 15),
	GATE(ACLK_PCIE_1L1_DBI, "aclk_pcie_1l1_dbi", "aclk_php_root", RK3588_CLKGATE_CON(33), 0),
	GATE(ACLK_PCIE_1L2_DBI, "aclk_pcie_1l2_dbi", "aclk_php_root", RK3588_CLKGATE_CON(33), 1),
	GATE(ACLK_PCIE_4L_MSTR, "aclk_pcie_4l_mstr", "aclk_mmu_pcie", RK3588_CLKGATE_CON(33), 2),
	GATE(ACLK_PCIE_2L_MSTR, "aclk_pcie_2l_mstr", "aclk_mmu_pcie", RK3588_CLKGATE_CON(33), 3),
	GATE(ACLK_PCIE_1L0_MSTR, "aclk_pcie_1l0_mstr", "aclk_mmu_pcie", RK3588_CLKGATE_CON(33), 4),
	GATE(ACLK_PCIE_1L1_MSTR, "aclk_pcie_1l1_mstr", "aclk_mmu_pcie", RK3588_CLKGATE_CON(33), 5),
	GATE(ACLK_PCIE_1L2_MSTR, "aclk_pcie_1l2_mstr", "aclk_mmu_pcie", RK3588_CLKGATE_CON(33), 6),
	GATE(ACLK_PCIE_4L_SLV, "aclk_pcie_4l_slv", "aclk_php_root", RK3588_CLKGATE_CON(33), 7),
	GATE(ACLK_PCIE_2L_SLV, "aclk_pcie_2l_slv", "aclk_php_root", RK3588_CLKGATE_CON(33), 8),
	GATE(ACLK_PCIE_1L0_SLV, "aclk_pcie_1l0_slv", "aclk_php_root", RK3588_CLKGATE_CON(33), 9),
	GATE(ACLK_PCIE_1L1_SLV, "aclk_pcie_1l1_slv", "aclk_php_root", RK3588_CLKGATE_CON(33), 10),
	GATE(ACLK_PCIE_1L2_SLV, "aclk_pcie_1l2_slv", "aclk_php_root", RK3588_CLKGATE_CON(33), 11),
	GATE(PCLK_PCIE_4L, "pclk_pcie_4l", "pclk_php_root", RK3588_CLKGATE_CON(33), 12),
	GATE(PCLK_PCIE_2L, "pclk_pcie_2l", "pclk_php_root", RK3588_CLKGATE_CON(33), 13),
	GATE(PCLK_PCIE_1L0, "pclk_pcie_1l0", "pclk_php_root", RK3588_CLKGATE_CON(33), 14),
	GATE(PCLK_PCIE_1L1, "pclk_pcie_1l1", "pclk_php_root", RK3588_CLKGATE_CON(33), 15),
	GATE(PCLK_PCIE_1L2, "pclk_pcie_1l2", "pclk_php_root", RK3588_CLKGATE_CON(34), 0),
	GATE(CLK_PCIE_AUX0, "clk_pcie_aux0", "xin24m", RK3588_CLKGATE_CON(34), 1),
	GATE(CLK_PCIE_AUX1, "clk_pcie_aux1", "xin24m", RK3588_CLKGATE_CON(34), 2),
	GATE(CLK_PCIE_AUX2, "clk_pcie_aux2", "xin24m", RK3588_CLKGATE_CON(34), 3),
	GATE(CLK_PCIE_AUX3, "clk_pcie_aux3", "xin24m", RK3588_CLKGATE_CON(34), 4),
	GATE(CLK_PCIE_AUX4, "clk_pcie_aux4", "xin24m", RK3588_CLKGATE_CON(34), 5),
	GATE(CLK_PIPEPHY0_REF, "clk_pipephy0_ref", "xin24m", RK3588_CLKGATE_CON(37), 0),
	GATE(CLK_PMALIVE0, "clk_pmalive0", "xin24m", RK3588_CLKGATE_CON(37), 4),
	GATE(ACLK_SATA0, "aclk_sata0", "aclk_mmu_php", RK3588_CLKGATE_CON(37), 7),
	GATE(CLK_RXOOB0, "clk_rxoob0", "clk_rxoob0_c", RK3588_CLKGATE_CON(37), 10),
	GATE(PCLK_PCIE_COMBO_PIPE_PHY0, "pclk_pcie_combo_pipe_phy0", "pclk_top_root", RK3588_PHP_CLKGATE_CON(0), 5),
	GATE(PCLK_PCIE_COMBO_PIPE_PHY1, "pclk_pcie_combo_pipe_phy1", "pclk_top_root", RK3588_PHP_CLKGATE_CON(0), 6),
	GATE(PCLK_PCIE_COMBO_PIPE_PHY2, "pclk_pcie_combo_pipe_phy2", "pclk_top_root", RK3588_PHP_CLKGATE_CON(0), 7),
	GATE(PCLK_PCIE_COMBO_PIPE_PHY, "pclk_pcie_combo_pipe_phy", "pclk_top_root", RK3588_PHP_CLKGATE_CON(0), 8),
	GATE(CLK_PIPEPHY0_PIPE_G, "clk_pipephy0_pipe_g", "clk_pipephy0_pipe_i", RK3588_CLKGATE_CON(38), 3),
	GATE(CLK_PIPEPHY1_PIPE_G, "clk_pipephy1_pipe_g", "clk_pipephy1_pipe_i", RK3588_CLKGATE_CON(38), 4),
	GATE(CLK_PIPEPHY2_PIPE_G, "clk_pipephy2_pipe_g", "clk_pipephy2_pipe_i", RK3588_CLKGATE_CON(38), 5),
	GATE(CLK_PIPEPHY0_PIPE_ASIC_G, "clk_pipephy0_pipe_asic_g", "clk_pipephy0_pipe_i", RK3588_CLKGATE_CON(38), 6),
	GATE(CLK_PCIE4L_PIPE, "clk_pcie4l_pipe", "clk_pipephy0_pipe_g", RK3588_CLKGATE_CON(39), 0),
	GATE(CLK_PCIE2L_PIPE, "clk_pcie2l_pipe", "clk_pipephy2_pipe_g", RK3588_CLKGATE_CON(39), 1),
	GATE(CLK_PCIE1L2_PIPE, "clk_pcie1l2_pipe", "clk_pipephy0_pipe_g", RK3588_CLKGATE_CON(38), 13),
	GATE(CLK_PCIE1L0_PIPE, "clk_pcie1l0_pipe", "clk_pipephy1_pipe_g", RK3588_CLKGATE_CON(38), 14),
	GATE(CLK_PCIE1L1_PIPE, "clk_pcie1l1_pipe", "clk_pipephy2_pipe_g", RK3588_CLKGATE_CON(38), 15),
};

static const struct rk_cru_reset rk3588_resets[] = {
	{ SRST_M_I2S0_8CH_TX, RK3588_SOFTRST_CON(7), 7 },
	{ SRST_M_I2S0_8CH_RX, RK3588_SOFTRST_CON(7), 10 },
	{ SRST_P_TSADC, RK3588_SOFTRST_CON(12), 0 },
	{ SRST_TSADC, RK3588_SOFTRST_CON(12), 1 },
	{ SRST_USBDP_COMBO_PHY0_INIT, RK3588_SOFTRST_CON(2), 8 },
	{ SRST_USBDP_COMBO_PHY0_CMN, RK3588_SOFTRST_CON(2), 9 },
	{ SRST_USBDP_COMBO_PHY0_LANE, RK3588_SOFTRST_CON(2), 10 },
	{ SRST_USBDP_COMBO_PHY0_PCS, RK3588_SOFTRST_CON(2), 11 },
	{ SRST_USBDP_COMBO_PHY1_INIT, RK3588_SOFTRST_CON(2), 15 },
	{ SRST_USBDP_COMBO_PHY1_CMN, RK3588_SOFTRST_CON(3), 0 },
	{ SRST_USBDP_COMBO_PHY1_LANE, RK3588_SOFTRST_CON(3), 1 },
	{ SRST_USBDP_COMBO_PHY1_PCS, RK3588_SOFTRST_CON(3), 2 },
	{ SRST_H_EMMC, RK3588_SOFTRST_CON(31), 4 },
	{ SRST_A_EMMC, RK3588_SOFTRST_CON(31), 5 },
	{ SRST_C_EMMC, RK3588_SOFTRST_CON(31), 6 },
	{ SRST_B_EMMC, RK3588_SOFTRST_CON(31), 7 },
	{ SRST_T_EMMC, RK3588_SOFTRST_CON(31), 8 },
	{ SRST_A_USB_BIU, RK3588_SOFTRST_CON(42), 2 },
	{ SRST_H_USB_BIU, RK3588_SOFTRST_CON(42), 3 },
	{ SRST_A_USB3OTG0, RK3588_SOFTRST_CON(42), 4 },
	{ SRST_A_USB3OTG1, RK3588_SOFTRST_CON(42), 7 },
	{ SRST_H_HOST0, RK3588_SOFTRST_CON(42), 10 },
	{ SRST_H_HOST_ARB0, RK3588_SOFTRST_CON(42), 11 },
	{ SRST_H_HOST1, RK3588_SOFTRST_CON(42), 12 },
	{ SRST_H_HOST_ARB1, RK3588_SOFTRST_CON(42), 13 },
	{ SRST_A_USB_GRF, RK3588_SOFTRST_CON(42), 14 },
	{ SRST_C_USB2P0_HOST0, RK3588_SOFTRST_CON(42), 15 },
	{ SRST_C_USB2P0_HOST1, RK3588_SOFTRST_CON(43), 0 },
	{ SRST_HOST_UTMI0, RK3588_SOFTRST_CON(43), 1 },
	{ SRST_HOST_UTMI1, RK3588_SOFTRST_CON(43), 2 },
	{ SRST_P_USBDPPHY0, RK3588_SOFTRST_CON(72), 2 },
	{ SRST_P_USBDPPHY1, RK3588_SOFTRST_CON(72), 4 },
	{ SRST_P_HDPTX0, RK3588_SOFTRST_CON(72), 5 },
	{ SRST_P_HDPTX1, RK3588_SOFTRST_CON(72), 6 },
	{ SRST_P_USB2PHY_U3_0_GRF0, RK3588_SOFTRST_CON(72), 8 },
	{ SRST_P_USB2PHY_U3_1_GRF0, RK3588_SOFTRST_CON(72), 9 },
	{ SRST_P_USB2PHY_U2_0_GRF0, RK3588_SOFTRST_CON(72), 10 },
	{ SRST_P_USB2PHY_U2_1_GRF0, RK3588_SOFTRST_CON(72), 11 },
	{ SRST_PCIE0_POWER_UP, RK3588_SOFTRST_CON(32), 13 },
	{ SRST_PCIE1_POWER_UP, RK3588_SOFTRST_CON(32), 14 },
	{ SRST_PCIE2_POWER_UP, RK3588_SOFTRST_CON(32), 15 },
	{ SRST_PCIE3_POWER_UP, RK3588_SOFTRST_CON(33), 0 },
	{ SRST_PCIE4_POWER_UP, RK3588_SOFTRST_CON(33), 1 },
	{ SRST_P_PCIE0, RK3588_SOFTRST_CON(33), 12 },
	{ SRST_P_PCIE1, RK3588_SOFTRST_CON(33), 13 },
	{ SRST_P_PCIE2, RK3588_SOFTRST_CON(33), 14 },
	{ SRST_P_PCIE3, RK3588_SOFTRST_CON(33), 15 },
	{ SRST_P_PCIE4, RK3588_SOFTRST_CON(34), 0 },
	{ SRST_A_GMAC0, RK3588_SOFTRST_CON(32), 10 },
	{ SRST_A_GMAC1, RK3588_SOFTRST_CON(32), 11 },
	{ SRST_P_PCIE2_PHY0, RK3588_PHP_SOFTRST_CON(0), 5 },
	{ SRST_REF_PIPE_PHY0, RK3588_SOFTRST_CON(77), 6 },
	{ SRST_REF_PIPE_PHY1, RK3588_SOFTRST_CON(77), 7 },
	{ SRST_REF_PIPE_PHY2, RK3588_SOFTRST_CON(77), 8 },
	{ SRST_P_PCIE2_PHY1, RK3588_PHP_SOFTRST_CON(0), 6 },
	{ SRST_P_PCIE2_PHY2, RK3588_PHP_SOFTRST_CON(0), 7 },
	{ SRST_P_PCIE3_PHY, RK3588_PHP_SOFTRST_CON(0), 8 },
	{ SRST_PCIE30_PHY, RK3588_PHP_SOFTRST_CON(0), 10 },
	{ SRST_OTGPHY_U3_0, RK3588_PMU_SOFTRST_CON(4), 7 },
	{ SRST_OTGPHY_U3_1, RK3588_PMU_SOFTRST_CON(4), 8 },
	{ SRST_OTGPHY_U2_0, RK3588_PMU_SOFTRST_CON(4), 9 },
	{ SRST_OTGPHY_U2_1, RK3588_PMU_SOFTRST_CON(4), 10 },
	{ SRST_HDPTX0_INIT, RK3588_PMU_SOFTRST_CON(3), 11 },
	{ SRST_HDPTX0_CMN, RK3588_PMU_SOFTRST_CON(3), 12 },
	{ SRST_HDPTX0_LANE, RK3588_PMU_SOFTRST_CON(3), 13 },
	{ SRST_HDPTX1_INIT, RK3588_PMU_SOFTRST_CON(3), 15 },
	{ SRST_HDPTX1_CMN, RK3588_PMU_SOFTRST_CON(4), 0 },
	{ SRST_HDPTX1_LANE, RK3588_PMU_SOFTRST_CON(4), 1 },
};

static int
rk3588_cru_probe(device_t dev)
{
	if (!ofw_bus_status_okay(dev))
		return (ENXIO);

	if (ofw_bus_is_compatible(dev, "rockchip,rk3588-cru")) {
		device_set_desc(dev, "Rockchip RK3588 Clock & Reset Unit");
		return (BUS_PROBE_DEFAULT);
	}

	return (ENXIO);
}

static int
rk3588_cru_attach(device_t dev)
{
	struct rk_cru_softc *sc;
	int error;

	sc = device_get_softc(dev);
	sc->dev = dev;
	sc->clks = rk3588_clks;
	sc->nclks = nitems(rk3588_clks);
	sc->gates = rk3588_gates;
	sc->ngates = nitems(rk3588_gates);
	sc->gate_flags = RK_CLK_GATE_HIWORD_MASK | RK_CLK_GATE_SET_TO_DISABLE;
	sc->reset_offset = RK3588_SOFTRST_CON(0);
	sc->reset_num = 2048;
	sc->reset_map = rk3588_resets;
	sc->nreset_map = nitems(rk3588_resets);
	sc->reset_map_strict = true;

	device_printf(dev,
	    "RK3588-DIAG V2114 CRU strict logical reset map enabled; "
	    "unsafe linear fallback disabled\n");

	error = rk_cru_attach(dev);
	if (error != 0)
		return (error);

	device_printf(dev,
	    "RK3588-DIAG V2239 VOP assigned clocks raw "
	    "sel110/111/112/113/115=%#x/%#x/%#x/%#x/%#x "
	    "gate52/53=%#x/%#x\n",
	    bus_read_4(sc->res, RK3588_CLKSEL_CON(110)),
	    bus_read_4(sc->res, RK3588_CLKSEL_CON(111)),
	    bus_read_4(sc->res, RK3588_CLKSEL_CON(112)),
	    bus_read_4(sc->res, RK3588_CLKSEL_CON(113)),
	    bus_read_4(sc->res, RK3588_CLKSEL_CON(115)),
	    bus_read_4(sc->res, RK3588_CLKGATE_CON(52)),
	    bus_read_4(sc->res, RK3588_CLKGATE_CON(53)));
	return (0);
}

static device_method_t rk3588_cru_methods[] = {
	DEVMETHOD(device_probe,		rk3588_cru_probe),
	DEVMETHOD(device_attach,	rk3588_cru_attach),

	DEVMETHOD_END
};

DEFINE_CLASS_1(rk3588_cru, rk3588_cru_driver, rk3588_cru_methods,
    sizeof(struct rk_cru_softc), rk_cru_driver);

EARLY_DRIVER_MODULE(rk3588_cru, simplebus, rk3588_cru_driver,
    0, 0, BUS_PASS_BUS + BUS_PASS_ORDER_MIDDLE);
