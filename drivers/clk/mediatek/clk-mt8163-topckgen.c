// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2015 MediaTek Inc.
 *         James Liao <jamesjj.liao@mediatek.com>
 * Copyright (c) 2025-2026 Ben Grisdale <bengris32@protonmail.ch>
 */

#include <linux/clk-provider.h>
#include <linux/platform_device.h>

#include "clk-mtk.h"
#include "clk-gate.h"
#include "clk-mux.h"

#include <dt-bindings/clock/mediatek,mt8163-topckgen.h>

static DEFINE_SPINLOCK(mt8163_topckgen_lock);

static const struct mtk_fixed_clk topckgen_fixed_clks[] = {
	FIXED_CLK(CLK_TOP_DMPLL, "dmpll_ck", NULL, 0),
	FIXED_CLK(CLK_TOP_DSI0_LNTC_DSICK, "dsi0_lntc_dsick", "clk26m", 75 * MHZ),
	FIXED_CLK(CLK_TOP_F26M_MEM_CKGEN_OCC, "f26m_mem_ckgen", "clk26m", 26 * MHZ),
	FIXED_CLK(CLK_TOP_LVDSTX_CLKDIG_CTS, "lvdstx_dig_cts", "clk26m", 52500000),
};

static const struct mtk_fixed_factor topckgen_factors[] = {
	FACTOR(CLK_TOP_MAIN_H546M, "main_h546m", "mainpll", 1, 2),
	FACTOR(CLK_TOP_MAIN_H364M, "main_h364m", "mainpll", 1, 3),
	FACTOR(CLK_TOP_MAIN_H218P4M, "main_h218p4m", "mainpll", 1, 5),
	FACTOR(CLK_TOP_MAIN_H156M, "main_h156m", "mainpll", 1, 7),

	FACTOR(CLK_TOP_UNIV_624M, "univ_624m", "univpll", 1, 2),
	FACTOR(CLK_TOP_UNIV_416M, "univ_416m", "univpll", 1, 3),
	FACTOR(CLK_TOP_UNIV_249P6M, "univ_249p6m", "univpll", 1, 5),
	FACTOR(CLK_TOP_UNIV_178P3M, "univ_178p3m", "univpll", 1, 7),
	FACTOR(CLK_TOP_UNIV_48M, "univ_48m", "univpll", 1, 26),

	FACTOR(CLK_TOP_CLKRTC_INT, "clkrtc_int", "clk26m", 1, 793),

	FACTOR(CLK_TOP_HDMI_CTS, "hdmi_cts_ck", "hdmitx_dig_cts", 1, 2),
	FACTOR(CLK_TOP_HDMI_CTS_D2, "hdmi_cts_d2", "hdmitx_dig_cts", 1, 3),
	FACTOR(CLK_TOP_HDMI_CTS_D3, "hdmi_cts_d3", "hdmitx_dig_cts", 1, 2),

	FACTOR(CLK_TOP_LVDSPLL_D2, "lvdspll_d2", "lvdspll", 1, 2),
	FACTOR(CLK_TOP_LVDSPLL_D4, "lvdspll_d4", "lvdspll", 1, 4),
	FACTOR(CLK_TOP_LVDSPLL_D8, "lvdspll_d8", "lvdspll", 1, 8),
	FACTOR(CLK_TOP_LVDSPLL_ETH, "lvdspll_eth_ck", "lvdspll", 1, 4),

	FACTOR(CLK_TOP_MSDCPLL_D2, "msdcpll_d2", "msdcpll", 1, 2),
	FACTOR(CLK_TOP_MSDCPLL_D4, "msdcpll_d4", "msdcpll", 1, 4),

	FACTOR(CLK_TOP_SYSPLL_D2P5, "syspll_d2p5", "main_h218p4m", 2, 1),
	FACTOR(CLK_TOP_SYSPLL1_D2, "syspll1_d2", "main_h546m", 1, 2),
	FACTOR(CLK_TOP_SYSPLL1_D4, "syspll1_d4", "main_h546m", 1, 4),
	FACTOR(CLK_TOP_SYSPLL1_D8, "syspll1_d8", "main_h546m", 1, 8),
	FACTOR(CLK_TOP_SYSPLL1_D16, "syspll1_d16", "main_h546m", 1, 16),
	FACTOR(CLK_TOP_SYSPLL2_D2, "syspll2_d2", "main_h364m", 1, 2),
	FACTOR(CLK_TOP_SYSPLL2_D4, "syspll2_d4", "main_h364m", 1, 4),
	FACTOR(CLK_TOP_SYSPLL2_D8, "syspll2_d8", "main_h364m", 1, 8),
	FACTOR(CLK_TOP_SYSPLL3_D2, "syspll3_d2", "main_h218p4m", 1, 2),
	FACTOR(CLK_TOP_SYSPLL3_D4, "syspll3_d4", "main_h218p4m", 1, 4),
	FACTOR(CLK_TOP_SYSPLL4_D2, "syspll4_d2", "main_h156m", 1, 2),
	FACTOR(CLK_TOP_SYSPLL4_D4, "syspll4_d4", "main_h156m", 1, 4),

	FACTOR(CLK_TOP_TVDPLL_D2, "tvdpll_d2", "tvdpll", 1, 2),
	FACTOR(CLK_TOP_TVDPLL_D4, "tvdpll_d4", "tvdpll", 1, 4),
	FACTOR(CLK_TOP_TVDPLL_D8, "tvdpll_d8", "tvdpll", 1, 8),
	FACTOR(CLK_TOP_TVDPLL_D16, "tvdpll_d16", "tvdpll", 1, 16),

	FACTOR(CLK_TOP_UNIVPLL_D2P5, "univpll_d2p5", "univ_249p6m", 2, 1),
	FACTOR(CLK_TOP_UNIVPLL1_D2, "univpll1_d2", "univ_624m", 1, 2),
	FACTOR(CLK_TOP_UNIVPLL1_D4, "univpll1_d4", "univ_624m", 1, 4),
	FACTOR(CLK_TOP_UNIVPLL2_D2, "univpll2_d2", "univ_416m", 1, 2),
	FACTOR(CLK_TOP_UNIVPLL2_D4, "univpll2_d4", "univ_416m", 1, 4),
	FACTOR(CLK_TOP_UNIVPLL2_D8, "univpll2_d8", "univ_416m", 1, 8),
	FACTOR(CLK_TOP_UNIVPLL3_D2, "univpll3_d2", "univ_249p6m", 1, 2),
	FACTOR(CLK_TOP_UNIVPLL3_D4, "univpll3_d4", "univ_249p6m", 1, 4),
	FACTOR(CLK_TOP_UNIVPLL3_D8, "univpll3_d8", "univ_249p6m", 1, 8),
};

static const char * const axi_parents[] = {
	"clk26m",
	"syspll1_d4"
};

static const char * const mem_parents[] = {
	"clk26m",
	"dmpll_ck",
	"f26m_mem_ckgen"
};

static const char * const ddrphycfg_parents[] = {
	"clk26m",
	"syspll1_d8"
};

static const char * const mm_parents[] = {
	"clk26m",
	"main_h364m",
	"univ_416m",
	"vencpll"
};

static const char * const pwm_parents[] = {
	"clk26m",
	"univpll2_d2",
	"univpll2_d4"
};

static const char * const vdec_parents[] = {
	"clk26m",
	"main_h364m",
	"syspll1_d2",
	"univpll1_d2",
	"syspll1_d4"
};

static const char * const mfg_parents[] = {
	"clk26m",
	"mmpll",
	"univ_416m",
	"main_h364m"
};

static const char * const camtg_parents[] = {
	"clk26m",
	"univ_48m",
	"univpll2_d2"
};

static const char * const uart_parents[] = {
	"clk26m",
	"univpll2_d8"
};

static const char * const spi_parents[] = {
	"clk26m",
	"syspll3_d2"
};

static const char * const msdc30_0_parents[] = {
	"clk26m",
	"univpll2_d2",
	"msdcpll_d2",
	"univpll1_d4",
	"syspll2_d2",
	"main_h156m",
	"univ_178p3m"
};

static const char * const msdc30_1_parents[] = {
	"clk26m",
	"univpll2_d2",
	"msdcpll_d2",
	"univpll1_d4",
	"syspll2_d2",
	"main_h156m",
	"univ_178p3m"
};

static const char * const msdc30_2_parents[] = {
	"clk26m",
	"univpll2_d2",
	"msdcpll_d2",
	"univpll1_d4",
	"syspll2_d2",
	"main_h156m",
	"univ_178p3m"
};

static const char * const msdc50_3_h_parents[] = {
	"clk26m",
	"syspll1_d2",
	"syspll2_d2",
	"syspll4_d2"
};

static const char * const msdc50_3_parents[] = {
	"clk26m",
	"msdcpll",
	"msdcpll_d2",
	"univpll1_d4",
	"syspll2_d2",
	"main_h156m",
	"msdcpll_d4",
	"univ_624m",
	"univpll1_d2"
};

static const char * const audio_parents[] = {
	"clk26m",
	"syspll3_d4",
	"syspll4_d4",
	"syspll1_d16"
};

static const char * const aud_intbus_parents[] = {
	"clk26m",
	"syspll1_d4",
	"syspll4_d2"
};

static const char * const pmicspi_parents[] = {
	"clk26m",
	"univ_48m"
};

static const char * const scp_parents[] = {
	"clk26m",
	"mpll",
	"syspll1_d4"
};

static const char * const atb_parents[] = {
	"clk26m",
	"syspll1_d2"
};

static const char * const mjc_parents[] = {
	"clk26m",
	"main_h218p4m",
	"univ_249p6m"
};

static const char * const dpi0_parents[] = {
	"clk26m",
	"lvdspll",
	"lvdspll_d2",
	"lvdspll_d4",
	"lvdspll_d8"
};

static const char * const scam_parents[] = {
	"clk26m",
	"syspll3_d2"
};

static const char * const aud_1_parents[] = {
	"clk26m",
	"aud1pll"
};

static const char * const aud_2_parents[] = {
	"clk26m",
	"aud2pll"
};

static const char * const dpi1_parents[] = {
	"clk26m",
	"tvdpll_d2",
	"tvdpll_d4",
	"tvdpll_d8",
	"tvdpll_d16"
};

static const char * const ufoenc_parents[] = {
	"clk26m",
	"univ_624m",
	"main_h546m",
	"univpll_d2p5",
	"syspll_d2p5",
	"univ_416m",
	"main_h364m",
	"syspll1_d2"
};

static const char * const ufodec_parents[] = {
	"clk26m",
	"main_h546m",
	"univpll_d2p5",
	"syspll_d2p5",
	"univ_416m",
	"main_h364m",
	"syspll1_d2"
};

static const char * const eth_parents[] = {
	"clk26m",
	"syspll3_d4",
	"univpll2_d8",
	"lvdspll_eth_ck",
	"univ_48m",
	"syspll2_d8",
	"syspll4_d4",
	"univpll3_d8",
	"clk26m"
};

static const char * const onfi_parents[] = {
	"clk26m",
	"syspll2_d2",
	"main_h156m",
	"univpll3_d2",
	"syspll2_d4",
	"univpll3_d4",
	"syspll4_d4"
};

static const char * const snfi_parents[] = {
	"clk26m",
	"univpll2_d8",
	"univpll3_d4",
	"syspll4_d2",
	"univpll2_d4",
	"univpll3_d2",
	"syspll1_d4",
	"univpll1_d4",
	"clk26m"
};

static const char * const hdmi_parents[] = {
	"clk26m",
	"hdmi_cts_ck",
	"hdmi_cts_d2",
	"hdmi_cts_d3"
};

static const char * const rtc_parents[] = {
	"clkrtc_int",
	"clk32k",
	"clk26m",
	"univpll3_d8"
};

static const struct mtk_mux topckgen_muxes[] = {
	/* CLK_CFG_0 */
	MUX_GATE_UPD_FLAGS(CLK_TOP_AXI_SEL, "axi_sel", axi_parents,
		0x0040, 0, 1, 7, 0x0004, 0,
		CLK_IS_CRITICAL | CLK_SET_RATE_PARENT),
	MUX_GATE_UPD_FLAGS(CLK_TOP_MEM_SEL, "mem_sel", mem_parents,
		0x0040, 8, 2, 15, 0x0004, 1,
		CLK_IS_CRITICAL | CLK_SET_RATE_PARENT),
	MUX_GATE_UPD_FLAGS(CLK_TOP_DDRPHYCFG_SEL, "ddrphycfg_sel", ddrphycfg_parents,
		0x0040, 16, 1, 23, 0x0004, 2,
		CLK_IS_CRITICAL | CLK_SET_RATE_PARENT),
	MUX_GATE_UPD(CLK_TOP_MM_SEL, "mm_sel", mm_parents,
		0x0040, 24, 2, 31, 0x0004, 3),

	/* CLK_CFG_1 */
	MUX_GATE_UPD(CLK_TOP_PWM_SEL, "pwm_sel", pwm_parents,
		0x0050, 0, 2, 7, 0x0004, 4),
	MUX_GATE_UPD(CLK_TOP_VDEC_SEL, "vdec_sel", vdec_parents,
		0x0050, 8, 3, 15, 0x0004, 5),
	MUX_GATE_UPD(CLK_TOP_MFG_SEL, "mfg_sel", mfg_parents,
		0x0050, 24, 2, 31, 0x0004, 7),

	/* CLK_CFG_2 */
	MUX_GATE_UPD(CLK_TOP_CAMTG_SEL, "camtg_sel", camtg_parents,
		0x0060, 0, 2, 7, 0x0004, 8),
	MUX_GATE_UPD(CLK_TOP_UART_SEL, "uart_sel", uart_parents,
		0x0060, 8, 1, 15, 0x0004, 9),
	MUX_GATE_UPD(CLK_TOP_SPI_SEL, "spi_sel", spi_parents,
		0x0060, 16, 1, 23, 0x0004, 10),

	/* CLK_CFG_3 */
	MUX_GATE_UPD(CLK_TOP_MSDC30_0_SEL, "msdc30_0_sel", msdc30_0_parents,
		0x0070, 0, 3, 7, 0x0004, 12),
	MUX_GATE_UPD(CLK_TOP_MSDC30_1_SEL, "msdc30_1_sel", msdc30_1_parents,
		0x0070, 8, 3, 15, 0x0004, 13),
	MUX_GATE_UPD(CLK_TOP_MSDC30_2_SEL, "msdc30_2_sel", msdc30_2_parents,
		0x0070, 16, 3, 23, 0x0004, 14),

	/* CLK_CFG_4 */
	MUX_GATE_UPD(CLK_TOP_MSDC50_3_HSEL, "msdc50_3_hsel", msdc50_3_h_parents,
		0x0080, 0, 2, 7, 0x0004, 15),
	MUX_GATE_UPD(CLK_TOP_MSDC50_3_SEL, "msdc50_3_sel", msdc50_3_parents,
		0x0080, 8, 4, 15, 0x0004, 16),
	MUX_GATE_UPD(CLK_TOP_AUDIO_SEL, "audio_sel", audio_parents,
		0x0080, 16, 2, 23, 0x0004, 17),
	MUX_GATE_UPD(CLK_TOP_AUD_INTBUS_SEL, "aud_intbus_sel", aud_intbus_parents,
		0x0080, 24, 2, 31, 0x0004, 18),

	/* CLK_CFG_5 */
	MUX_GATE_UPD(CLK_TOP_PMICSPI_SEL, "pmicspi_sel", pmicspi_parents,
		0x0090, 0, 1, 7, 0x0004, 19),
	MUX_GATE_UPD(CLK_TOP_SCP_SEL, "scp_sel", scp_parents,
		0x0090, 8, 2, 15, 0x0004, 20),
	MUX_GATE_UPD(CLK_TOP_ATB_SEL, "atb_sel", atb_parents,
		0x0090, 16, 1, 23, 0x0004, 21),
	MUX_GATE_UPD(CLK_TOP_MJC_SEL, "mjc_sel", mjc_parents,
		0x0090, 24, 2, 31, 0x0004, 22),

	/* CLK_CFG_6 */
	/*
	 * The dpi0_sel clock should not propagate rate changes to its parent
	 * clock so the dpi driver can have full control over PLL and divider.
	 */
	MUX_GATE_UPD_FLAGS(CLK_TOP_DPI0_SEL, "dpi0_sel", dpi0_parents,
		0x00a0, 0, 3, 7, 0x0004, 23,
		0),
	MUX_GATE_UPD(CLK_TOP_SCAM_SEL, "scam_sel", scam_parents,
		0x00a0, 8, 1, 15, 0x0004, 24),
	MUX_GATE_UPD(CLK_TOP_AUD_1_SEL, "aud_1_sel", aud_1_parents,
		0x00a0, 16, 1, 23, 0x0004, 25),
	MUX_GATE_UPD(CLK_TOP_AUD_2_SEL, "aud_2_sel", aud_2_parents,
		0x00a0, 24, 1, 31, 0x0004, 26),

	/* CLK_CFG_7 */
	MUX_GATE_UPD(CLK_TOP_DPI1_SEL, "dpi1_sel", dpi1_parents,
		0x00b0, 0, 3, 7, 0x0004, 6),
	MUX_GATE_UPD(CLK_TOP_UFOENC_SEL, "ufoenc_sel", ufoenc_parents,
		0x00b0, 8, 3, 15, 0x0004, 27),
	MUX_GATE_UPD(CLK_TOP_UFODEC_SEL, "ufodec_sel", ufodec_parents,
		0x00b0, 16, 3, 23, 0x0004, 28),
	MUX_GATE_UPD(CLK_TOP_ETH_SEL, "eth_sel", eth_parents,
		0x00b0, 24, 4, 31, 0x0004, 29),

	/* CLK_CFG_8 */
	MUX_GATE_UPD(CLK_TOP_ONFI_SEL, "onfi_sel", onfi_parents,
		0x00c0, 0, 3, 7, 0x0004, 30),
	MUX_GATE_UPD(CLK_TOP_SNFI_SEL, "snfi_sel", snfi_parents,
		0x00c0, 8, 2, 15, 0x0004, 31),
	MUX_GATE_UPD(CLK_TOP_HDMI_SEL, "hdmi_sel", hdmi_parents,
		0x00c0, 16, 2, 23, 0x0004, 11),
	MUX_GATE_UPD_FLAGS(CLK_TOP_RTC_SEL, "rtc_sel", rtc_parents,
		0x00c0, 24, 2, 31, 0x0008, 0,
		CLK_IS_CRITICAL | CLK_SET_RATE_PARENT),
};

static const struct mtk_gate_regs top_cg_regs = {
	.set_ofs = 0x104,
	.clr_ofs = 0x104,
	.sta_ofs = 0x104,
};

#define GATE_TOP(_id, _name, _parent, _shift)				\
	GATE_MTK(_id, _name, _parent, &top_cg_regs, _shift, &mtk_clk_gate_ops_no_setclr_inv)

static const struct mtk_gate topckgen_gates[] = {
	/* CLK_MISC_CFG_0 */
	GATE_TOP(CLK_TOP_ARMPLL_DIVIDER_PLL1_EN,
		  "arm_div_pll1_en", "mainpll", 4),
	GATE_TOP(CLK_TOP_ARMPLL_DIVIDER_PLL2_EN,
		  "arm_div_pll2_en", "univpll", 5),
};

static const struct mtk_clk_desc topckgen_desc = {
	.clks = topckgen_gates,
	.num_clks = ARRAY_SIZE(topckgen_gates),
	.fixed_clks = topckgen_fixed_clks,
	.num_fixed_clks = ARRAY_SIZE(topckgen_fixed_clks),
	.factor_clks = topckgen_factors,
	.num_factor_clks = ARRAY_SIZE(topckgen_factors),
	.mux_clks = topckgen_muxes,
	.num_mux_clks = ARRAY_SIZE(topckgen_muxes),
	.clk_lock = &mt8163_topckgen_lock,
};

static const struct of_device_id of_match_mt8163_topckgen[] = {
	{ .compatible = "mediatek,mt8163-topckgen", .data = &topckgen_desc},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_match_mt8163_topckgen);

static struct platform_driver clk_mt8163_topckgen = {
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt8163-topckgen",
		.of_match_table = of_match_mt8163_topckgen,
	},
};
module_platform_driver(clk_mt8163_topckgen);

MODULE_AUTHOR("James Liao <jamesjj.liao@mediatek.com>");
MODULE_AUTHOR("Ben Grisdale <bengris32@protonmail.ch>");
MODULE_DESCRIPTION("MediaTek MT8163 topckgen clock driver");
MODULE_LICENSE("GPL");
