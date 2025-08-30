// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2015 MediaTek Inc.
 *         James Liao <jamesjj.liao@mediatek.com>
 * Copyright (c) 2025-2026 Ben Grisdale <bengris32@protonmail.ch>
 */

#include <linux/clk-provider.h>
#include <linux/platform_device.h>

#include "clk-gate.h"
#include "clk-mtk.h"
#include "clk-pll.h"

#include <dt-bindings/clock/mediatek,mt8163-apmixedsys.h>

#define REG_REF2USB		0x8

#define MT8163_PLL_FMAX		(2500UL * MHZ)

#define CON0_MT8163_RST_BAR	BIT(24)

static const struct mtk_gate_regs apmixed_cg_regs = {
	.set_ofs = 0x0,
	.clr_ofs = 0x0,
	.sta_ofs = 0x0,
};

#define GATE_APMIXED_FLAGS(_id, _name, _parent, _shift, _flags)		\
	GATE_MTK_FLAGS(_id, _name, _parent, &apmixed_cg_regs,		\
		       _shift, &mtk_clk_gate_ops_no_setclr_inv, _flags)

#define GATE_APMIXED(_id, _name, _parent, _shift)			\
	GATE_APMIXED_FLAGS(_id, _name, _parent, _shift,	0)

/*
 * CRITICAL CLOCK:
 * apmixed_armpll26m is the toppest clock gate of all PLLs.
 */
static const struct mtk_gate apmixed_clks[] = {
	GATE_APMIXED(CLK_APMIXED_SSUSB_26M, "apmixed_ssusb26m", "clk26m", 4),
	GATE_APMIXED_FLAGS(CLK_APMIXED_ARMPLL_26M, "apmixed_armpll26m",
			   "clk26m", 5, CLK_IS_CRITICAL),
	GATE_APMIXED(CLK_APMIXED_MIPI_26M, "apmixed_mipi26m", "clk26m", 6),
};

#define _PLL(_id, _name, _reg, _pwr_reg, _en_mask, _flags, _pcwbits,	\
			_pd_reg, _pd_shift, _tuner_reg, _tuner_en_reg,	\
			_tuner_en_bit, _pcw_reg, _pcw_shift,		\
			_div_table) {					\
		.id = _id,						\
		.name = _name,						\
		.reg = _reg,						\
		.pwr_reg = _pwr_reg,					\
		.en_mask = _en_mask,					\
		.flags = _flags,					\
		.rst_bar_mask = CON0_MT8163_RST_BAR,			\
		.fmax = MT8163_PLL_FMAX,				\
		.pcwbits = _pcwbits,					\
		.pd_reg = _pd_reg,					\
		.pd_shift = _pd_shift,					\
		.tuner_reg = _tuner_reg,				\
		.tuner_en_reg = _tuner_en_reg,				\
		.tuner_en_bit = _tuner_en_bit,				\
		.pcw_reg = _pcw_reg,					\
		.pcw_shift = _pcw_shift,				\
		.div_table = _div_table,				\
	}

#define PLL(_id, _name, _reg, _pwr_reg, _en_mask, _flags, _pcwbits,			\
			_pd_reg, _pd_shift, _tuner_reg, _pcw_reg,			\
			_pcw_shift)							\
		_PLL(_id, _name, _reg, _pwr_reg, _en_mask, _flags, _pcwbits,		\
			_pd_reg, _pd_shift, _tuner_reg, 0, 0, _pcw_reg, _pcw_shift,	\
			NULL)

#define PLL_B(_id, _name, _reg, _pwr_reg, _en_mask, _flags, _pcwbits,			\
			_pd_reg, _pd_shift, _tuner_reg, _pcw_reg,			\
			_pcw_shift, _div_table)						\
		_PLL(_id, _name, _reg, _pwr_reg, _en_mask, _flags, _pcwbits,		\
			_pd_reg, _pd_shift, _tuner_reg, 0, 0, _pcw_reg, _pcw_shift,	\
			_div_table)

#define PLL_C(_id, _name, _reg, _pwr_reg, _en_mask, _flags, _pcwbits,			\
			_pd_reg, _pd_shift, _tuner_reg, _tuner_en_reg,			\
			_tuner_en_bit, _pcw_reg,					\
			_pcw_shift)							\
		_PLL(_id, _name, _reg, _pwr_reg, _en_mask, _flags, _pcwbits,		\
			_pd_reg, _pd_shift, _tuner_reg, _tuner_en_reg,			\
			_tuner_en_bit, _pcw_reg, _pcw_shift,				\
			NULL)

static const struct mtk_pll_div_table armpll_div_table[] = {
	{ .div = 0, .freq = MT8163_PLL_FMAX },
	{ .div = 1, .freq = 1001000000 },
	{ .div = 2, .freq = 520000000 },
	{ .div = 3, .freq = 260000000 },
	{ .div = 4, .freq = 130000000 },
	{ /* sentinel */}
};

static const struct mtk_pll_div_table mmpll_div_table[] = {
	{ .div = 0, .freq = MT8163_PLL_FMAX },
	{ .div = 1, .freq = 1000000000 },
	{ .div = 2, .freq = 625000000 },
	{ .div = 3, .freq = 253500000 },
	{ .div = 4, .freq = 126750000 },
	{ /* sentinel */ }
};

static const struct mtk_pll_data plls[] = {
	PLL_B(CLK_APMIXED_ARMPLL, "armpll", 0x210, 0x21c, 0x1, PLL_AO, 21, 0x214, 24, 0x0, 0x214, 0, armpll_div_table),
	PLL(CLK_APMIXED_MAINPLL, "mainpll", 0x220, 0x22c, 0x1, HAVE_RST_BAR, 21, 0x220, 4, 0x0, 0x224, 0),
	PLL(CLK_APMIXED_UNIVPLL, "univpll", 0x230, 0x23c, 0x1, HAVE_RST_BAR, 21, 0x230, 4, 0x0, 0x234, 0),
	PLL_B(CLK_APMIXED_MMPLL, "mmpll", 0x240, 0x24c, 0x1, 0, 21, 0x244, 24, 0x0, 0x244, 0, mmpll_div_table),
	PLL(CLK_APMIXED_MSDCPLL, "msdcpll", 0x250, 0x25c, 0x1, 0, 21, 0x250, 4, 0x0, 0x254, 0),
	PLL(CLK_APMIXED_VENCPLL, "vencpll", 0x260, 0x26c, 0x1, 0, 21, 0x260, 4, 0x0, 0x264, 0),
	PLL(CLK_APMIXED_TVDPLL, "tvdpll", 0x270, 0x27c, 0x1, 0, 21, 0x270, 4, 0x0, 0x274, 0),
	PLL(CLK_APMIXED_MPLL, "mpll", 0x280, 0x28c, 0x1, 0, 21, 0x280, 4, 0x0, 0x284, 0),
	PLL_C(CLK_APMIXED_AUD1PLL, "aud1pll", 0x2a0, 0x2ac, 0x1, 0, 31, 0x2a0, 4, 0x300, 0x014, 10, 0x2a4, 0),
	PLL_C(CLK_APMIXED_AUD2PLL, "aud2pll", 0x2b0, 0x2bc, 0x1, 0, 31, 0x2b0, 4, 0x304, 0x014, 11, 0x2b4, 0),
	PLL(CLK_APMIXED_LVDSPLL, "lvdspll", 0x2c0, 0x2cc, 0x1, 0, 21, 0x2c0, 4, 0x0, 0x2c4, 0),
};

static const struct of_device_id of_match_clk_mt8163_apmixed[] = {
	{ .compatible = "mediatek,mt8163-apmixedsys" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt8163_apmixed);

static int clk_mt8163_apmixed_probe(struct platform_device *pdev)
{
	struct clk_hw_onecell_data *clk_data;
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node;
	void __iomem *base;
	struct clk_hw *hw;
	int ret;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	clk_data = mtk_alloc_clk_data(CLK_APMIXED_NR_CLK);
	if (!clk_data)
		return -ENOMEM;

	ret = mtk_clk_register_plls(node, plls, ARRAY_SIZE(plls), clk_data);
	if (ret)
		goto free_clk_data;

	ret = mtk_clk_register_gates(&pdev->dev, node, apmixed_clks,
				     ARRAY_SIZE(apmixed_clks), clk_data);
	if (ret)
		goto unregister_plls;

	hw = mtk_clk_register_ref2usb_tx("ref2usb_tx", "clk26m", base + REG_REF2USB);
	if (IS_ERR(hw)) {
		ret = PTR_ERR(hw);
		dev_err(dev, "Failed to register ref2usb_tx: %d\n", ret);
		goto unregister_gates;
	}
	clk_data->hws[CLK_APMIXED_REF2USB_TX] = hw;

	ret = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, clk_data);
	if (ret) {
		dev_err(dev, "Cannot register clock provider: %d\n", ret);
		goto unregister_ref2usb;
	}

	return 0;

unregister_ref2usb:
	mtk_clk_unregister_ref2usb_tx(clk_data->hws[CLK_APMIXED_REF2USB_TX]);
unregister_gates:
	mtk_clk_unregister_gates(apmixed_clks, ARRAY_SIZE(apmixed_clks), clk_data);
unregister_plls:
	mtk_clk_unregister_plls(plls, ARRAY_SIZE(plls), clk_data);
free_clk_data:
	mtk_free_clk_data(clk_data);
	return ret;
}

static void clk_mt8163_apmixed_remove(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct clk_hw_onecell_data *clk_data = platform_get_drvdata(pdev);

	of_clk_del_provider(node);
	mtk_clk_unregister_ref2usb_tx(clk_data->hws[CLK_APMIXED_REF2USB_TX]);
	mtk_clk_unregister_plls(plls, ARRAY_SIZE(plls), clk_data);
	mtk_free_clk_data(clk_data);
}

static struct platform_driver clk_mt8163_apmixed_drv = {
	.probe = clk_mt8163_apmixed_probe,
	.remove = clk_mt8163_apmixed_remove,
	.driver = {
		.name = "clk-mt8163-apmixed",
		.of_match_table = of_match_clk_mt8163_apmixed,
	},
};
module_platform_driver(clk_mt8163_apmixed_drv);

MODULE_AUTHOR("James Liao <jamesjj.liao@mediatek.com>");
MODULE_AUTHOR("Ben Grisdale <bengris32@protonmail.ch>");
MODULE_DESCRIPTION("MediaTek MT8163 apmixed clocks driver");
MODULE_LICENSE("GPL");
