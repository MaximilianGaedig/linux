// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT8163 AFE ASoC platform driver.
 * Copyright (c) 2025-2026 Ben Grisdale <bengris32@protonmail.ch>
 */

#ifndef _MT8163_AFE_COMMON_H_
#define _MT8163_AFE_COMMON_H_

#include <sound/soc.h>
#include <linux/clk.h>
#include <linux/list.h>
#include <linux/regmap.h>
#include "../common/mtk-base-afe.h"

#define MTK_PCM_RATES (SNDRV_PCM_RATE_8000_48000 |\
		       SNDRV_PCM_RATE_88200 |\
		       SNDRV_PCM_RATE_96000 |\
		       SNDRV_PCM_RATE_176400 |\
		       SNDRV_PCM_RATE_192000)

#define MTK_PCM_FORMATS (SNDRV_PCM_FMTBIT_S16_LE |\
			 SNDRV_PCM_FMTBIT_S24_LE |\
			 SNDRV_PCM_FMTBIT_S32_LE)

/* Various providing clocks to AFE */
enum {
	MT8163_BASE_CLK_INFRASYS_AUD = 0,
	MT8163_BASE_CLK_TOP_PDN_AUD_BUS,
	MT8163_BASE_CLK_TOP_PDN_AUD,
	MT8163_BASE_CLK_TOP_I2S,
	MT8163_BASE_CLK_AFE,
	MT8163_BASE_CLK_ADC,
	MT8163_BASE_CLK_DAC,
	MT8163_BASE_CLK_DAC_PREDIS,
	MT8163_BASE_CLK_NUM,
};

enum {
	MT8163_MISC_CLK_APLL1 = 0,
	MT8163_MISC_CLK_APLL1_TUNER,
	MT8163_MISC_CLK_APLL2,
	MT8163_MISC_CLK_APLL2_TUNER,
	MT8163_MISC_CLK_22M,
	MT8163_MISC_CLK_24M,
	MT8163_MISC_CLK_APLL1_DIV,
	MT8163_MISC_CLK_APLL2_DIV,
	MT8163_MISC_CLK_AUD_1_SEL,
	MT8163_MISC_CLK_AUD_2_SEL,
	MT8163_MISC_CLK_I2S0,
	MT8163_MISC_CLK_I2S1,
	MT8163_MISC_CLK_I2S2,
	MT8163_MISC_CLK_I2S3,
	MT8163_MISC_CLK_CLK26M,
	MT8163_MISC_CLK_NUM
};

/* MCLK */
enum {
	MT8163_I2S0_MCK = 0,
	MT8163_I2S1_MCK,
	MT8163_I2S2_MCK,
	MT8163_I2S3_MCK,
	MT8163_MCK_NUM,
};

/* Digital blocks on SoC */
enum {
	/* Memory Interfaces (MEMIF) */
	MT8163_AFE_MEMIF_DL1 = 0,
	MT8163_AFE_MEMIF_DL2,
	MT8163_AFE_MEMIF_VUL,
	MT8163_AFE_MEMIF_AWB,
	/* I2S memif is not a real memif. */
	MT8163_AFE_MEMIF_I2S,
	MT8163_AFE_MEMIF_NUM,

	/* Digital Audio Interfaces (DAI) */
#if 0 // TODO: implement PCM support?
	MT8163_AFE_DAI_PCM1 = MT8163_AFE_MEMIF_NUM,
	MT8163_AFE_DAI_PCM2,
	MT8163_AFE_DAI_I2S0,
#else
	MT8163_AFE_DAI_I2S0 = MT8163_AFE_MEMIF_NUM,
#endif
	MT8163_AFE_DAI_I2S1,
	MT8163_AFE_DAI_I2S2,
	MT8163_AFE_DAI_I2S3,
#if 0 // TODO: implement hw gain support?
	MT8163_AFE_DAI_HW_GAIN1,
	MT8163_AFE_DAI_HW_GAIN2,
#endif
	MT8163_AFE_DAI_NUM,
};

enum {
	MT8163_AFE_IRQ1_MCU = 0,
	MT8163_AFE_IRQ2_MCU,
	MT8163_AFE_IRQ5_MCU,
	MT8163_AFE_IRQ7_MCU,
	MT8163_AFE_IRQ_NUM,
};

struct mt8163_afe_private {
	/* clocks */
	struct clk_bulk_data base_clocks[MT8163_BASE_CLK_NUM];
	struct clk_bulk_data misc_clocks[MT8163_MISC_CLK_NUM];

	/* dai */
	void *dai_priv[MT8163_AFE_DAI_NUM];
};

int mt8163_general_rate_transform(unsigned int rate);

int mt8163_dai_i2s_set_share(struct mtk_base_afe *afe, const char *main_i2s_name,
			     const char *secondary_i2s_name);

/* dai register */
int mt8163_dai_pcm_register(struct mtk_base_afe *afe);
int mt8163_dai_i2s_register(struct mtk_base_afe *afe);

#endif /* _MT8163_AFE_COMMON_H_ */
