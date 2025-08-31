// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025-2026 Ben Grisdale <bengris32@protonmail.ch>
 */

#include <linux/clk-provider.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

#include "clk-gate.h"
#include "clk-mtk.h"

#include <dt-bindings/clock/mediatek,mt8163-audsys.h>

#define AUDIO_CLK_AUDDIV_0	0x05a0
#define AUDIO_CLK_AUDDIV_1	0x05a4

static const char * const apll_div_parents[] = {
	"aud_1_sel",
	"aud_2_sel"
};

static const struct mtk_gate_regs aud_cg_regs = {
	.set_ofs = 0x0,
	.clr_ofs = 0x0,
	.sta_ofs = 0x0,
};

#define GATE_AUD(_id, _name, _parent, _shift)		\
	GATE_MTK(_id, _name, _parent, &aud_cg_regs, _shift, &mtk_clk_gate_ops_no_setclr)

static const struct mtk_gate aud_clks[] = {
	GATE_AUD(CLK_AUDIO_AFE, "aud_afe", "audio_sel", 2),
	GATE_AUD(CLK_AUDIO_I2S, "aud_i2s", NULL, 6),
	GATE_AUD(CLK_AUDIO_22M, "aud_22m", "audio_sel", 8),
	GATE_AUD(CLK_AUDIO_24M, "aud_24m", "audio_sel", 9),
	GATE_AUD(CLK_AUDIO_SPDF2, "aud_spdf2", NULL, 11),
	GATE_AUD(CLK_AUDIO_APLL2_TUNER, "aud_apll2_tnr", "aud_2_sel", 18),
	GATE_AUD(CLK_AUDIO_APLL_TUNER, "aud_apll_tnr", "aud_1_sel", 19),
	GATE_AUD(CLK_AUDIO_HDMI, "aud_hdmi", NULL, 20),
	GATE_AUD(CLK_AUDIO_SPDF, "aud_spdf", NULL, 21),
	GATE_AUD(CLK_AUDIO_ADC, "aud_adc", "audio_sel", 24),
	GATE_AUD(CLK_AUDIO_DAC, "aud_dac", "audio_sel", 25),
	GATE_AUD(CLK_AUDIO_DAC_PREDIS, "aud_dac_predis", "audio_sel", 26),
	GATE_AUD(CLK_AUDIO_TML, "aud_tml", "audio_sel", 27),
};

#define AUD_DIV_GATE(_id, _name, _parent, _div_shift, _gate_shift)	\
	DIV_GATE(_id, _name, _parent, AUDIO_CLK_AUDDIV_0, _gate_shift,	\
		 AUDIO_CLK_AUDDIV_0, 3 /* div_width */, _div_shift)

#define AUD_MUX_DIV(_id, _name, _mux_shift, _div_shift, _gate_shift)	\
	MUX_DIV_GATE_FLAGS(_id, _name, apll_div_parents, AUDIO_CLK_AUDDIV_0,	\
		_mux_shift, 1 /* mux_width */, AUDIO_CLK_AUDDIV_1,	\
		_div_shift, 8 /* div_width */, AUDIO_CLK_AUDDIV_0,	\
		_gate_shift, 0)

static const struct mtk_composite aud_divs[] = {
	AUD_DIV_GATE(CLK_AUDIO_APLL1_DIV0, "aud_apll1_div0", "aud_1_sel", 24, 0),
	AUD_DIV_GATE(CLK_AUDIO_APLL2_DIV0, "aud_apll2_div0", "aud_2_sel", 28, 1),
	AUD_MUX_DIV(CLK_AUDIO_APLL_I2S0, "aud_apll_i2s0", 8, 0, 2),
	AUD_MUX_DIV(CLK_AUDIO_APLL_I2S1, "aud_apll_i2s1", 9, 8, 3),
	AUD_MUX_DIV(CLK_AUDIO_APLL_I2S2, "aud_apll_i2s2", 10, 16, 4),
	AUD_MUX_DIV(CLK_AUDIO_APLL_I2S3, "aud_apll_i2s3", 11, 24, 5),
};

static const struct mtk_clk_desc aud_desc = {
	.clks = aud_clks,
	.num_clks = ARRAY_SIZE(aud_clks),
	.composite_clks = aud_divs,
	.num_composite_clks = ARRAY_SIZE(aud_divs),
};

static int clk_mt8163_audio_probe(struct platform_device *pdev)
{
	int ret;

	ret = mtk_clk_simple_probe(pdev);
	if (ret)
		return ret;

	ret = devm_of_platform_populate(&pdev->dev);
	if (ret)
		mtk_clk_simple_remove(pdev);

	return ret;
}

static void clk_mt8163_audio_remove(struct platform_device *pdev)
{
	of_platform_depopulate(&pdev->dev);
	mtk_clk_simple_remove(pdev);
}

static const struct of_device_id of_match_clk_mt8163_audsys[] = {
	{ .compatible = "mediatek,mt8163-audsys", .data = &aud_desc },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt8163_audsys);

static struct platform_driver clk_mt8163_audsys_drv = {
	.probe = clk_mt8163_audio_probe,
	.remove = clk_mt8163_audio_remove,
	.driver = {
		.name = "clk-mt8163-audsys",
		.of_match_table = of_match_clk_mt8163_audsys,
	},
};
module_platform_driver(clk_mt8163_audsys_drv);

MODULE_AUTHOR("Ben Grisdale <bengris32@protonmail.ch>");
MODULE_DESCRIPTION("MediaTek MT8163 audiosys clocks driver");
MODULE_LICENSE("GPL");
