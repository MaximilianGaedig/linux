// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025 Ben Grisdale <bengris32@protonmail.ch>
 */

#include <linux/clk-provider.h>
#include <linux/platform_device.h>

#include "clk-mtk.h"
#include "clk-gate.h"

#include <dt-bindings/clock/mediatek,mt8163-mmsys.h>

static const struct mtk_gate_regs mm0_cg_regs = {
	.set_ofs = 0x0104,
	.clr_ofs = 0x0108,
	.sta_ofs = 0x0100,
};

static const struct mtk_gate_regs mm1_cg_regs = {
	.set_ofs = 0x0114,
	.clr_ofs = 0x0118,
	.sta_ofs = 0x0110,
};

#define GATE_MM0(_id, _name, _parent, _shift)	\
	GATE_MTK(_id, _name, _parent, &mm0_cg_regs, _shift, &mtk_clk_gate_ops_setclr)

#define GATE_MM1(_id, _name, _parent, _shift)	\
	GATE_MTK(_id, _name, _parent, &mm1_cg_regs, _shift, &mtk_clk_gate_ops_setclr)

static const struct mtk_gate mm_clks[] = {
	/* MM0 */
	GATE_MM0(CLK_MM_SMI_COMMON, "mm_smi_common", "mm_sel", 0),
	GATE_MM0(CLK_MM_SMI_LARB0, "mm_smi_larb0", "mm_sel", 1),
	GATE_MM0(CLK_MM_CAM_MDP, "mm_cam_mdp", "mm_sel", 2),
	GATE_MM0(CLK_MM_MDP_RDMA, "mm_mdp_rdma", "mm_sel", 3),
	GATE_MM0(CLK_MM_MDP_RSZ0, "mm_mdp_rsz0", "mm_sel", 4),
	GATE_MM0(CLK_MM_MDP_RSZ1, "mm_mdp_rsz1", "mm_sel", 5),
	GATE_MM0(CLK_MM_MDP_TDSHP, "mm_mdp_tdshp", "mm_sel", 6),
	GATE_MM0(CLK_MM_MDP_WDMA, "mm_mdp_wdma", "mm_sel", 7),
	GATE_MM0(CLK_MM_MDP_WROT, "mm_mdp_wrot", "mm_sel", 8),
	GATE_MM0(CLK_MM_FAKE_ENG, "mm_fake_eng", "mm_sel", 9),
	GATE_MM0(CLK_MM_DISP_OVL0, "mm_disp_ovl0", "mm_sel", 10),
	GATE_MM0(CLK_MM_DISP_OVL1, "mm_disp_ovl1", "mm_sel", 11),
	GATE_MM0(CLK_MM_DISP_RDMA0, "mm_disp_rdma0", "mm_sel", 12),
	GATE_MM0(CLK_MM_DISP_RDMA1, "mm_disp_rdma1", "mm_sel", 13),
	GATE_MM0(CLK_MM_DISP_WDMA0, "mm_disp_wdma0", "rtc_sel", 14),
	GATE_MM0(CLK_MM_DISP_COLOR, "mm_disp_color", "mm_sel", 15),
	GATE_MM0(CLK_MM_DISP_CCORR, "mm_disp_ccorr", "mm_sel", 16),
	GATE_MM0(CLK_MM_DISP_AAL, "mm_disp_aal", "mm_sel", 17),
	GATE_MM0(CLK_MM_DISP_GAMMA, "mm_disp_gamma", "mm_sel", 18),
	GATE_MM0(CLK_MM_DISP_DITHER, "mm_disp_dither", "mm_sel", 19),
	GATE_MM0(CLK_MM_DISP_UFOE, "mm_disp_ufoe", "mm_sel", 20),
	GATE_MM0(CLK_MM_LARB4_AXI_ASIF_MM, "mm_larb4_mm", "mm_sel", 21),
	GATE_MM0(CLK_MM_LARB4_AXI_ASIF_MJC, "mm_larb4_mjc", "mm_sel", 22),
	GATE_MM0(CLK_MM_DISP_WDMA1, "mm_disp_wdma1", "mm_sel", 23),
	GATE_MM0(CLK_MM_UFOD_RDMA0_L0, "mm_ufodrdma0_l0", "mm_sel", 24),
	GATE_MM0(CLK_MM_UFOD_RDMA0_L1, "mm_ufodrdma0_l1", "mm_sel", 25),
	GATE_MM0(CLK_MM_UFOD_RDMA0_L2, "mm_ufodrdma0_l2", "mm_sel", 26),
	GATE_MM0(CLK_MM_UFOD_RDMA0_L3, "mm_ufodrdma0_l3", "mm_sel", 27),
	GATE_MM0(CLK_MM_UFOD_RDMA1_L0, "mm_ufodrdma1_l0", "mm_sel", 28),
	GATE_MM0(CLK_MM_UFOD_RDMA1_L1, "mm_ufodrdma1_l1", "mm_sel", 29),
	GATE_MM0(CLK_MM_UFOD_RDMA1_L2, "mm_ufodrdma1_l2", "mm_sel", 30),
	GATE_MM0(CLK_MM_UFOD_RDMA1_L3, "mm_ufodrdma1_l3", "mm_sel", 31),

	/* MM1 */
	GATE_MM1(CLK_MM_DISP_PWM_MM, "mm_disp_pwm0mm", "mm_sel", 0),
	GATE_MM1(CLK_MM_DISP_PWM_26M, "mm_disp_pwm026m", "clk26m", 1),
	GATE_MM1(CLK_MM_DSI_ENGINE, "mm_dsi0_engine", "mm_sel", 2),
	GATE_MM1(CLK_MM_DSI_DIGITAL, "mm_dsi0_digital", "dsi0_lntc_dsick", 3),
	GATE_MM1(CLK_MM_DPI0_PIXEL, "mm_dpi_pixel", "mm_sel", 4),
	GATE_MM1(CLK_MM_DPI0_ENGINE, "mm_dpi_engine", "dpi0_sel", 5),
	GATE_MM1(CLK_MM_LVDS_PIXEL, "mm_lvds_pixel", "dpi0_sel", 6),
	GATE_MM1(CLK_MM_LVDS_CTS, "mm_lvds_cts", "lvdstx_dig_cts", 7),
	GATE_MM1(CLK_MM_DPI1_PIXEL, "mm_dpi1_pixel", "mm_sel", 8),
	GATE_MM1(CLK_MM_DPI1_ENGINE, "mm_dpi1_engine", "dpi1_sel", 9),
	GATE_MM1(CLK_MM_HDMI_PIXEL, "mm_hdmi_pixel", "dpi1_sel", 10),
	GATE_MM1(CLK_MM_HDMI_SPDIF, "mm_hdmi_spdif", "apll2_div1", 11),
	GATE_MM1(CLK_MM_HDMI_ADSP, "mm_hdmi_audio", "apll2_div0", 12),
	GATE_MM1(CLK_MM_HDMI_PLLCK, "mm_hdmi_pllck", "hdmi_sel", 13),
	GATE_MM1(CLK_MM_DISP_DSC_ENGINE, "mm_disp_dsc_eng", "mm_sel", 14),
	GATE_MM1(CLK_MM_DISP_DSC_MEM, "mm_disp_dsc_mem", "mm_sel", 15),
};

static const struct mtk_clk_desc mm_desc = {
	.clks = mm_clks,
	.num_clks = ARRAY_SIZE(mm_clks),
};

static const struct platform_device_id clk_mt8163_mm_id_table[] = {
	{ .name = "clk-mt8163-mm", .driver_data = (kernel_ulong_t)&mm_desc },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(platform, clk_mt8163_mm_id_table);

static struct platform_driver clk_mt8163_mm_drv = {
	.probe = mtk_clk_pdev_probe,
	.remove = mtk_clk_pdev_remove,
	.driver = {
		.name = "clk-mt8163-mmsys",
	},
	.id_table = clk_mt8163_mm_id_table,
};
module_platform_driver(clk_mt8163_mm_drv);

MODULE_AUTHOR("Ben Grisdale <bengris32@protonmail.ch>");
MODULE_DESCRIPTION("MediaTek MT8163 mmsys clocks driver");
MODULE_LICENSE("GPL");
