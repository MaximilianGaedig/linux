// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT8163 ALSA SoC Audio AFE clock control
 * Copyright (c) 2025-2026 Ben Grisdale <bengris32@protonmail.ch>
 */

#include <linux/device.h>

#include "mt8163-afe-regs.h"
#include "mt8163-afe-common.h"
#include "mt8163-afe-clk.h"

#define MT8163_APLL1_RATE	90316800
#define MT8163_APLL2_RATE	98304000

static const char *mt8163_afe_base_clks[MT8163_BASE_CLK_NUM] = {
	[MT8163_BASE_CLK_INFRASYS_AUD] = "infrasys_aud",
	[MT8163_BASE_CLK_TOP_PDN_AUD_BUS] = "top_pdn_aud_bus",
	[MT8163_BASE_CLK_TOP_PDN_AUD] = "top_pdn_aud",
	[MT8163_BASE_CLK_TOP_I2S] = "top_i2s",
	[MT8163_BASE_CLK_AFE] = "afe",
	[MT8163_BASE_CLK_ADC] = "adc",
	[MT8163_BASE_CLK_DAC] = "dac",
	[MT8163_BASE_CLK_DAC_PREDIS] = "dac_predis",
};

static const char *mt8163_afe_misc_clks[MT8163_MISC_CLK_NUM] = {
	[MT8163_MISC_CLK_APLL1] = "apll1",
	[MT8163_MISC_CLK_APLL1_TUNER] = "apll1_tuner",
	[MT8163_MISC_CLK_APLL2] = "apll2",
	[MT8163_MISC_CLK_APLL2_TUNER] = "apll2_tuner",
	[MT8163_MISC_CLK_22M] = "22m",
	[MT8163_MISC_CLK_24M] = "24m",
	[MT8163_MISC_CLK_APLL1_DIV] = "apll1_div",
	[MT8163_MISC_CLK_APLL2_DIV] = "apll2_div",
	[MT8163_MISC_CLK_AUD_1_SEL] = "aud_1_sel",
	[MT8163_MISC_CLK_AUD_2_SEL] = "aud_2_sel",
	[MT8163_MISC_CLK_I2S0] = "i2s0",
	[MT8163_MISC_CLK_I2S1] = "i2s1",
	[MT8163_MISC_CLK_I2S2] = "i2s2",
	[MT8163_MISC_CLK_I2S3] = "i2s3",
	[MT8163_MISC_CLK_CLK26M] = "clk26m",
};

int mt8163_init_clock(struct mtk_base_afe *afe)
{
	struct mt8163_afe_private *priv = afe->platform_priv;
	int i, ret;

	for (i = 0; i < MT8163_BASE_CLK_NUM; i++)
		priv->base_clocks[i].id = mt8163_afe_base_clks[i];
	for (i = 0; i < MT8163_MISC_CLK_NUM; i++)
		priv->misc_clocks[i].id = mt8163_afe_misc_clks[i];

	ret = devm_clk_bulk_get(afe->dev, MT8163_BASE_CLK_NUM, priv->base_clocks);
	if (ret) {
		dev_err(afe->dev, "Failed to get AFE base clocks: %d.\n", ret);
		return ret;
	}

	ret = devm_clk_bulk_get(afe->dev, MT8163_MISC_CLK_NUM, priv->misc_clocks);
	if (ret) {
		dev_err(afe->dev, "Failed to get AFE misc clocks: %d.\n", ret);
		return ret;
	}

	return 0;
}

int mt8163_afe_enable_clock(struct mtk_base_afe *afe)
{
	struct mt8163_afe_private *priv = afe->platform_priv;
	int ret;

	/* Bulk prepare and enable base clocks. */
	ret = clk_bulk_prepare_enable(MT8163_BASE_CLK_NUM, priv->base_clocks);
	if (ret) {
		dev_err(afe->dev, "Failed to enable base clocks: %d.\n", ret);
		return ret;
	}

	return 0;
}

void mt8163_afe_disable_clock(struct mtk_base_afe *afe)
{
	struct mt8163_afe_private *priv = afe->platform_priv;

	clk_bulk_disable_unprepare(MT8163_BASE_CLK_NUM, priv->base_clocks);
}

int mt8163_apll1_enable(struct mtk_base_afe *afe)
{
	struct mt8163_afe_private *priv = afe->platform_priv;
	int ret;

	/* Make sure APLL is prepared and enabled. */
	ret = clk_prepare_enable(priv->misc_clocks[MT8163_MISC_CLK_APLL1].clk);
	if (ret) {
		dev_err(afe->dev, "Failed to enable APLL1: %d.\n", ret);
		return ret;
	}

	/*
	 * Set the APLL to a predetermined rate, none of the dividers set
	 * CLK_SET_RATE_PARENT since we want to avoid changing the PLL rate
	 * from what we set initially.
	 */
	ret = clk_set_rate(priv->misc_clocks[MT8163_MISC_CLK_APLL1].clk,
			   MT8163_APLL1_RATE);
	if (ret) {
		dev_err(afe->dev, "Failed to set APLL1 rate: %d.\n", ret);
		return ret;
	}

	/* Switch the top audio muxes to PLL source. */
	ret = clk_set_parent(priv->misc_clocks[MT8163_MISC_CLK_AUD_1_SEL].clk,
			     priv->misc_clocks[MT8163_MISC_CLK_APLL1].clk);
	if (ret) {
		dev_err(afe->dev, "Failed to reparent audio clock to PLL: %d.\n", ret);
		return ret;
	}

	/* Enable and set the APLL divider to /4. */
	ret = clk_prepare_enable(priv->misc_clocks[MT8163_MISC_CLK_APLL1_DIV].clk);
	if (ret) {
		dev_err(afe->dev, "Failed to enable APLL1 divider: %d.\n", ret);
		return ret;
	}

	ret = clk_set_rate(priv->misc_clocks[MT8163_MISC_CLK_APLL1_DIV].clk,
			   (MT8163_APLL1_RATE >> 2));
	if (ret) {
		dev_err(afe->dev, "Failed to set APLL1 divider rate: %d.\n", ret);
		return ret;
	}

	/* Enable top 22M clock */
	ret = clk_prepare_enable(priv->misc_clocks[MT8163_MISC_CLK_22M].clk);
	if (ret) {
		dev_err(afe->dev, "Failed to enable 22M clock: %d.\n", ret);
		return ret;
	}

	/* Enable APLL1 tuner */
	ret = clk_prepare_enable(priv->misc_clocks[MT8163_MISC_CLK_APLL1_TUNER].clk);
	if (ret) {
		dev_err(afe->dev, "Failed to enable APLL1 tuner: %d.\n", ret);
		return ret;
	}

	/* Set APLL1 tuner configuration */
	regmap_update_bits(afe->regmap, AFE_APLL1_TUNER_CFG,
			   0x0000FFF7, 0x00008033);

	return 0;
}

void mt8163_apll1_disable(struct mtk_base_afe *afe)
{
	struct mt8163_afe_private *priv = afe->platform_priv;
	int ret;

	/* Reparent audio mux to clk26m */
	ret = clk_set_parent(priv->misc_clocks[MT8163_MISC_CLK_AUD_1_SEL].clk,
			     priv->misc_clocks[MT8163_MISC_CLK_CLK26M].clk);
	if (ret) {
		dev_err(afe->dev, "Failed to reparent audio clock to clk26m: %d.\n", ret);
	}

	clk_disable_unprepare(priv->misc_clocks[MT8163_MISC_CLK_APLL1_DIV].clk);
	clk_disable_unprepare(priv->misc_clocks[MT8163_MISC_CLK_APLL1].clk);
	clk_disable_unprepare(priv->misc_clocks[MT8163_MISC_CLK_APLL1_TUNER].clk);
	clk_disable_unprepare(priv->misc_clocks[MT8163_MISC_CLK_22M].clk);
}

int mt8163_apll2_enable(struct mtk_base_afe *afe)
{
	struct mt8163_afe_private *priv = afe->platform_priv;
	int ret;

	/* Make sure APLL2 is prepared and enabled. */
	ret = clk_prepare_enable(priv->misc_clocks[MT8163_MISC_CLK_APLL2].clk);
	if (ret) {
		dev_err(afe->dev, "Failed to enable APLL2: %d.\n", ret);
		return ret;
	}

	/*
	 * Set the APLL to a predetermined rate, none of the dividers set
	 * CLK_SET_RATE_PARENT since we want to avoid changing the PLL rate
	 * from what we set initially.
	 */
	ret = clk_set_rate(priv->misc_clocks[MT8163_MISC_CLK_APLL2].clk,
			   MT8163_APLL2_RATE);
	if (ret) {
		dev_err(afe->dev, "Failed to set APLL1 rate: %d.\n", ret);
		return ret;
	}

	/* Switch the top audio muxes to PLL source. */
	ret = clk_set_parent(priv->misc_clocks[MT8163_MISC_CLK_AUD_2_SEL].clk,
			     priv->misc_clocks[MT8163_MISC_CLK_APLL2].clk);
	if (ret) {
		dev_err(afe->dev, "Failed to reparent audio clock to PLL: %d.\n", ret);
		return ret;
	}

	/* Enable and set the APLL2 divider to /4. */
	ret = clk_prepare_enable(priv->misc_clocks[MT8163_MISC_CLK_APLL2_DIV].clk);
	if (ret) {
		dev_err(afe->dev, "Failed to enable APLL2 divider: %d.\n", ret);
		return ret;
	}

	ret = clk_set_rate(priv->misc_clocks[MT8163_MISC_CLK_APLL2_DIV].clk,
			   (MT8163_APLL2_RATE >> 2));
	if (ret) {
		dev_err(afe->dev, "Failed to set APLL2 divider rate: %d.\n", ret);
		return ret;
	}

	/* Enable top 24M clock */
	ret = clk_prepare_enable(priv->misc_clocks[MT8163_MISC_CLK_24M].clk);
	if (ret) {
		dev_err(afe->dev, "Failed to enable 24M clock: %d.\n", ret);
		return ret;
	}

	/* Enable APLL2 tuner */
	ret = clk_prepare_enable(priv->misc_clocks[MT8163_MISC_CLK_APLL2_TUNER].clk);
	if (ret) {
		dev_err(afe->dev, "Failed to enable APLL2 tuner: %d.\n", ret);
		return ret;
	}

	/* Set APLL2 tuner configuration */
	regmap_update_bits(afe->regmap, AFE_APLL2_TUNER_CFG,
			   0x0000FFF7, 0x00000435);

	return 0;
}

void mt8163_apll2_disable(struct mtk_base_afe *afe)
{
	struct mt8163_afe_private *priv = afe->platform_priv;
	int ret;

	/* Reparent audio mux to clk26m */
	ret = clk_set_parent(priv->misc_clocks[MT8163_MISC_CLK_AUD_2_SEL].clk,
			     priv->misc_clocks[MT8163_MISC_CLK_CLK26M].clk);
	if (ret) {
		dev_err(afe->dev, "Failed to reparent audio clock to clk26m: %d.\n", ret);
	}

	clk_disable_unprepare(priv->misc_clocks[MT8163_MISC_CLK_APLL2_DIV].clk);
	clk_disable_unprepare(priv->misc_clocks[MT8163_MISC_CLK_APLL2].clk);
	clk_disable_unprepare(priv->misc_clocks[MT8163_MISC_CLK_APLL2_TUNER].clk);
	clk_disable_unprepare(priv->misc_clocks[MT8163_MISC_CLK_24M].clk);
}

int mt8163_get_apll_rate(int apll)
{
	return (apll == MT8163_APLL1) ? MT8163_APLL1_RATE : MT8163_APLL2_RATE;
}

int mt8163_get_apll_by_rate(int rate)
{
	if (!(MT8163_APLL1_RATE % rate))
		return MT8163_APLL1;
	else if (!(MT8163_APLL2_RATE % rate))
		return MT8163_APLL2;
	else
		return -1;
}

int mt8163_get_apll_by_name(const char *name)
{
	if (strcmp(name, APLL1_W_NAME) == 0)
		return MT8163_APLL1;
	else
		return MT8163_APLL2;
}

/* mck */
static const int mck_clks[MT8163_MCK_NUM] = {
	[MT8163_I2S0_MCK] = MT8163_MISC_CLK_I2S0,
	[MT8163_I2S1_MCK] = MT8163_MISC_CLK_I2S1,
	[MT8163_I2S2_MCK] = MT8163_MISC_CLK_I2S2,
	[MT8163_I2S3_MCK] = MT8163_MISC_CLK_I2S3,
};

int mt8163_mck_enable(struct mtk_base_afe *afe, int mck_id, int rate)
{
	struct mt8163_afe_private *afe_priv = afe->platform_priv;
	int apll = mt8163_get_apll_by_rate(rate);
	int apll_clk_id = apll == MT8163_APLL1 ?
			  MT8163_MISC_CLK_AUD_1_SEL : MT8163_MISC_CLK_AUD_2_SEL;
	int i2s_clk_id = mck_clks[mck_id];
	int ret;

	if (apll < 0)
		return -EINVAL;

	/* Enable divider and select PLL */
	ret = clk_prepare_enable(afe_priv->misc_clocks[i2s_clk_id].clk);
	if (ret) {
		dev_err(afe->dev, "Failed to enable I2S clock %s: %d.\n",
			mt8163_afe_misc_clks[i2s_clk_id], ret);
		return ret;
	}
	ret = clk_set_parent(afe_priv->misc_clocks[i2s_clk_id].clk,
			     afe_priv->misc_clocks[apll_clk_id].clk);
	if (ret) {
		dev_err(afe->dev, "Failed to reparent I2S clock %s to PLL %s: %d.\n",
			mt8163_afe_misc_clks[i2s_clk_id],
			mt8163_afe_misc_clks[apll_clk_id], ret);
		goto disable_i2s_clk;
	}

	/* Set rate, change divider */
	ret = clk_set_rate(afe_priv->misc_clocks[i2s_clk_id].clk, rate);
	if (ret) {
		dev_err(afe->dev, "Failed to set rate for I2S clock %s: %d.\n",
			mt8163_afe_misc_clks[i2s_clk_id], ret);
		goto disable_i2s_clk;
	}

	return 0;

disable_i2s_clk:
	clk_disable_unprepare(afe_priv->misc_clocks[i2s_clk_id].clk);
	return ret;
}

void mt8163_mck_disable(struct mtk_base_afe *afe, int mck_id)
{
	struct mt8163_afe_private *afe_priv = afe->platform_priv;
	int i2s_clk_id = mck_clks[mck_id];

	clk_disable_unprepare(afe_priv->misc_clocks[i2s_clk_id].clk);
}
