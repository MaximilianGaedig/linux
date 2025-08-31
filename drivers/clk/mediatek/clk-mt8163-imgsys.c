// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025-2026 Ben Grisdale <bengris32@protonmail.ch>
 */

#include <linux/clk-provider.h>
#include <linux/platform_device.h>

#include "clk-gate.h"
#include "clk-mtk.h"

#include <dt-bindings/clock/mediatek,mt8163-imgsys.h>

static const struct mtk_gate_regs img_cg_regs = {
	.set_ofs = 0x0004,
	.clr_ofs = 0x0008,
	.sta_ofs = 0x0000,
};

#define GATE_IMG(_id, _name, _parent, _shift)			\
	GATE_MTK(_id, _name, _parent, &img_cg_regs, _shift, &mtk_clk_gate_ops_setclr)

static const struct mtk_gate img_clks[] = {
	GATE_IMG(CLK_IMG_LARB2_SMI, "img_larb2_smi", "mm_sel", 0),
	GATE_IMG(CLK_IMG_JPGENC, "img_jpgenc", "mm_sel", 4),
	GATE_IMG(CLK_IMG_CAM_SMI, "img_cam_smi", "mm_sel", 5),
	GATE_IMG(CLK_IMG_CAM_CAM, "img_cam_cam", "mm_sel", 6),
	GATE_IMG(CLK_IMG_SEN_TG, "img_sen_tg", "camtg_sel", 7),
	GATE_IMG(CLK_IMG_SEN_CAM, "img_sen_cam", "mm_sel", 8),
	GATE_IMG(CLK_IMG_CAM_SV, "img_cam_sv", "mm_sel", 9),
};

static const struct mtk_clk_desc img_desc = {
	.clks = img_clks,
	.num_clks = ARRAY_SIZE(img_clks),
};

static const struct of_device_id of_match_clk_mt8163_imgsys[] = {
	{ .compatible = "mediatek,mt8163-imgsys", .data = &img_desc },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt8163_imgsys);

static struct platform_driver clk_mt8163_imgsys_drv = {
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt8163-imgsys",
		.of_match_table = of_match_clk_mt8163_imgsys,
	},
};
module_platform_driver(clk_mt8163_imgsys_drv);

MODULE_AUTHOR("Ben Grisdale <bengris32@protonmail.ch>");
MODULE_DESCRIPTION("MediaTek MT8163 imgsys clocks driver");
MODULE_LICENSE("GPL");
