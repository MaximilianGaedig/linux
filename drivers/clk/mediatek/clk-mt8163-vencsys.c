// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025-2026 Ben Grisdale <bengris32@protonmail.ch>
 */

#include <linux/clk-provider.h>
#include <linux/platform_device.h>

#include "clk-mtk.h"
#include "clk-gate.h"

#include <dt-bindings/clock/mediatek,mt8163-vencsys.h>

static const struct mtk_gate_regs venc_cg_regs = {
	.set_ofs = 0x0004,
	.clr_ofs = 0x0008,
	.sta_ofs = 0x0000,
};

#define GATE_VENC(_id, _name, _parent, _shift)	\
		GATE_MTK(_id, _name, _parent, &venc_cg_regs, _shift, &mtk_clk_gate_ops_setclr_inv)

static const struct mtk_gate venc_clks[] = {
	GATE_VENC(CLK_VENC_CKE0, "venc_cke0", "mm_sel", 0),
	GATE_VENC(CLK_VENC_CKE1, "venc_cke1", "mm_sel", 4),
	GATE_VENC(CLK_VENC_CKE2, "venc_cke2", "mm_sel", 8),
	GATE_VENC(CLK_VENC_CKE3, "venc_cke3", "mm_sel", 12),
};

static const struct mtk_clk_desc venc_desc = {
	.clks = venc_clks,
	.num_clks = ARRAY_SIZE(venc_clks),
};

static const struct of_device_id of_match_clk_mt8163_vencsys[] = {
	{ .compatible = "mediatek,mt8163-vencsys", .data = &venc_desc },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt8163_vencsys);

static struct platform_driver clk_mt8163_vencsys_drv = {
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt8163-vencsys",
		.of_match_table = of_match_clk_mt8163_vencsys,
	},
};
module_platform_driver(clk_mt8163_vencsys_drv);

MODULE_AUTHOR("Ben Grisdale <bengris32@protonmail.ch>");
MODULE_DESCRIPTION("MediaTek MT8163 vencsys clocks driver");
MODULE_LICENSE("GPL");
