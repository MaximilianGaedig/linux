// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025-2026 Ben Grisdale <bengris32@protonmail.ch>
 */

#include <linux/clk-provider.h>
#include <linux/platform_device.h>

#include "clk-gate.h"
#include "clk-mtk.h"

#include <dt-bindings/clock/mediatek,mt8163-mfgcfg.h>

static const struct mtk_gate_regs mfgcfg_cg_regs = {
	.set_ofs = 0x0004,
	.clr_ofs = 0x0008,
	.sta_ofs = 0x0000,
};

#define GATE_MFG(_id, _name, _parent, _shift)			\
	GATE_MTK(_id, _name, _parent, &mfgcfg_cg_regs, _shift, &mtk_clk_gate_ops_setclr)

static const struct mtk_gate mfgcfg_gates[] = {
	GATE_MFG(CLK_MFG_BG3D, "mfg_bg3d", "mfg_sel", 0),
};

static const struct mtk_clk_desc mfgcfg_clks = {
	.clks = mfgcfg_gates,
	.num_clks = ARRAY_SIZE(mfgcfg_gates),
};

static const struct of_device_id of_match_mt8163_mfgcfg[] = {
	{ .compatible = "mediatek,mt8163-mfgcfg", .data = &mfgcfg_clks },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_match_mt8163_mfgcfg);

static struct platform_driver clk_mt8163_mfgcfg = {
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt8163-mfgcfg",
		.of_match_table = of_match_mt8163_mfgcfg,
	},
};
module_platform_driver(clk_mt8163_mfgcfg);

MODULE_AUTHOR("Ben Grisdale <bengris32@protonmail.ch>");
MODULE_DESCRIPTION("Mediatek MT8163 mfgcfg clock driver");
MODULE_LICENSE("GPL");
