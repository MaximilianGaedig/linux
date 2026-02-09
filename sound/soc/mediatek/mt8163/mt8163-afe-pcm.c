// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT8163 AFE ASoC platform driver.
 * Copyright (c) 2025-2026 Ben Grisdale <bengris32@protonmail.ch>
 */

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/pm_runtime.h>

#include "mt8163-afe-common.h"
#include "mt8163-afe-clk.h"
#include "mt8163-afe-regs.h"
#include "../common/mtk-afe-platform-driver.h"
#include "../common/mtk-afe-fe-dai.h"

enum {
	MT8163_AFE_RATE_8K = 0,
	MT8163_AFE_RATE_11K = 1,
	MT8163_AFE_RATE_12K = 2,
	MT8163_AFE_RATE_16K = 4,
	MT8163_AFE_RATE_22K = 5,
	MT8163_AFE_RATE_24K = 6,
	MT8163_AFE_RATE_32K = 8,
	MT8163_AFE_RATE_44K = 9,
	MT8163_AFE_RATE_48K = 10,
	MT8163_AFE_RATE_88K = 11,
	MT8163_AFE_RATE_96K = 12,
	MT8163_AFE_RATE_174K = 13,
	MT8163_AFE_RATE_192K = 14,
	MT8163_AFE_RATE_260K = 15,
};

static const unsigned int mt8163_afe_backup_list[] = {
	AUDIO_TOP_CON1,
	AUDIO_TOP_CON2,
	AUDIO_TOP_CON3,
	AFE_DAC_CON0,
	AFE_DAC_CON1,
	AFE_I2S_CON,
	AFE_DAIBT_CON0,
	AFE_CONN0,
	AFE_CONN1,
	AFE_CONN2,
	AFE_CONN3,
	AFE_CONN4,
	AFE_I2S_CON1,
	AFE_I2S_CON2,
	AFE_MRGIF_CON,
	AFE_DL1_BASE,
	AFE_DL1_CUR,
	AFE_DL1_END,
	AFE_DL1_D2_BASE,
	AFE_DL1_D2_CUR,
	AFE_DL1_D2_END,
	AFE_VUL_D2_BASE,
	AFE_VUL_D2_END,
	AFE_VUL_D2_CUR,
	AFE_I2S_CON3,
	AFE_DL2_BASE,
	AFE_DL2_CUR,
	AFE_DL2_END,
	AFE_CONN5,
	AFE_CONN_24BIT,
	AFE_AWB_BASE,
	AFE_AWB_END,
	AFE_AWB_CUR,
	AFE_VUL_BASE,
	AFE_VUL_END,
	AFE_VUL_CUR,
	AFE_DAI_BASE,
	AFE_DAI_END,
	AFE_DAI_CUR,
	AFE_CONN6,
	AFE_MEMIF_MSB,
	AFE_ADDA_DL_SRC2_CON0,
	AFE_ADDA_DL_SRC2_CON1,
	AFE_ADDA_UL_SRC_CON0,
	AFE_ADDA_UL_SRC_CON1,
	AFE_ADDA_TOP_CON0,
	AFE_ADDA_UL_DL_CON0,
	AFE_ADDA_NEWIF_CFG0,
	AFE_ADDA_NEWIF_CFG1,
	AFE_SIDETONE_CON0,
	AFE_SIDETONE_COEFF,
	AFE_SIDETONE_CON1,
	AFE_SIDETONE_GAIN,
	AFE_SGEN_CON0,
	AFE_TOP_CON0,
	AFE_ADDA_PREDIS_CON0,
	AFE_ADDA_PREDIS_CON1,
	AFE_MOD_DAI_BASE,
	AFE_MOD_DAI_END,
	AFE_MOD_DAI_CUR,
	AFE_IRQ_MCU_CON,
	AFE_IRQ_MCU_CNT1,
	AFE_IRQ_MCU_CNT2,
	AFE_IRQ_MCU_EN,
	AFE_MEMIF_MAXLEN,
	AFE_MEMIF_PBUF_SIZE,
	AFE_IRQ_MCU_CNT7,
	AFE_APLL1_TUNER_CFG,
	AFE_APLL2_TUNER_CFG,
	AFE_GAIN1_CON0,
	AFE_GAIN1_CON1,
	AFE_GAIN1_CON2,
	AFE_GAIN1_CON3,
	AFE_GAIN1_CONN,
	AFE_GAIN1_CUR,
	AFE_GAIN2_CON0,
	AFE_GAIN2_CON1,
	AFE_GAIN2_CON2,
	AFE_GAIN2_CON3,
	AFE_GAIN2_CONN,
	AFE_GAIN2_CUR,
	AFE_GAIN2_CONN2,
	AFE_GAIN2_CONN3,
	AFE_GAIN1_CONN2,
	AFE_GAIN1_CONN3,
	AFE_CONN7,
	AFE_CONN8,
	AFE_CONN9,
	AFE_CONN10,
	FPGA_CFG2,
	FPGA_CFG3,
	FPGA_CFG0,
	FPGA_CFG1,
	AFE_ASRC_CON0,
	AFE_ASRC_CON1,
	AFE_ASRC_CON2,
	AFE_ASRC_CON3,
	AFE_ASRC_CON4,
	AFE_ASRC_CON5,
	AFE_ASRC_CON6,
	AFE_ASRC_CON7,
	AFE_ASRC_CON8,
	AFE_ASRC_CON9,
	AFE_ASRC_CON10,
	AFE_ASRC_CON11,
	PCM_INTF_CON1,
	PCM_INTF_CON2,
	PCM2_INTF_CON,
	AUDIO_CLK_AUDDIV_0,
	AUDIO_CLK_AUDDIV_1,
	AFE_ASRC4_CON0,
	AFE_ASRC4_CON1,
	AFE_ASRC4_CON2,
	AFE_ASRC4_CON3,
	AFE_ASRC4_CON4,
	AFE_ASRC4_CON5,
	AFE_ASRC4_CON6,
	AFE_ASRC4_CON7,
	AFE_ASRC4_CON8,
	AFE_ASRC4_CON9,
	AFE_ASRC4_CON10,
	AFE_ASRC4_CON11,
	AFE_ASRC4_CON12,
	AFE_ASRC4_CON13,
	AFE_ASRC4_CON14,
	AFE_ASRC_CON13,
	AFE_ASRC_CON14,
	AFE_ASRC_CON15,
	AFE_ASRC_CON16,
	AFE_ASRC_CON17,
	AFE_ASRC_CON18,
	AFE_ASRC_CON19,
	AFE_ASRC_CON20,
	AFE_ASRC_CON21,
	AFE_ASRC2_CON0,
	AFE_ASRC2_CON1,
	AFE_ASRC2_CON2,
	AFE_ASRC2_CON3,
	AFE_ASRC2_CON4,
	AFE_ASRC2_CON5,
	AFE_ASRC2_CON6,
	AFE_ASRC2_CON7,
	AFE_ASRC2_CON8,
	AFE_ASRC2_CON9,
	AFE_ASRC2_CON10,
	AFE_ASRC2_CON11,
	AFE_ASRC2_CON12,
	AFE_ASRC2_CON13,
	AFE_ASRC2_CON14,
	AFE_ASRC3_CON0,
	AFE_ASRC3_CON1,
	AFE_ASRC3_CON2,
	AFE_ASRC3_CON3,
	AFE_ASRC3_CON4,
	AFE_ASRC3_CON5,
	AFE_ASRC3_CON6,
	AFE_ASRC3_CON7,
	AFE_ASRC3_CON8,
	AFE_ASRC3_CON9,
	AFE_ASRC3_CON10,
	AFE_ASRC3_CON11,
	AFE_ASRC3_CON12,
	AFE_ASRC3_CON13,
	AFE_ASRC3_CON14,
	AFE_ADDA4_TOP_CON0,
	AFE_ADDA4_UL_SRC_CON0,
	AFE_ADDA4_UL_SRC_CON1,
	AFE_ADDA4_NEWIF_CFG0,
	AFE_ADDA4_NEWIF_CFG1,
	AFE_ADDA4_ULCF_CFG_02_01,
	AFE_ADDA4_ULCF_CFG_04_03,
	AFE_ADDA4_ULCF_CFG_06_05,
	AFE_ADDA4_ULCF_CFG_08_07,
	AFE_ADDA4_ULCF_CFG_10_09,
	AFE_ADDA4_ULCF_CFG_12_11,
	AFE_ADDA4_ULCF_CFG_14_13,
	AFE_ADDA4_ULCF_CFG_16_15,
	AFE_ADDA4_ULCF_CFG_18_17,
	AFE_ADDA4_ULCF_CFG_20_19,
	AFE_ADDA4_ULCF_CFG_22_21,
	AFE_ADDA4_ULCF_CFG_24_23,
	AFE_ADDA4_ULCF_CFG_26_25,
	AFE_ADDA4_ULCF_CFG_28_27,
	AFE_ADDA4_ULCF_CFG_30_29,
};

int mt8163_general_rate_transform(unsigned int rate)
{
	switch (rate) {
	case 8000:
		return MT8163_AFE_RATE_8K;
	case 11025:
		return MT8163_AFE_RATE_11K;
	case 12000:
		return MT8163_AFE_RATE_12K;
	case 16000:
		return MT8163_AFE_RATE_16K;
	case 22050:
		return MT8163_AFE_RATE_22K;
	case 24000:
		return MT8163_AFE_RATE_24K;
	case 32000:
		return MT8163_AFE_RATE_32K;
	case 44100:
		return MT8163_AFE_RATE_44K;
	case 48000:
		return MT8163_AFE_RATE_48K;
	case 88000:
		return MT8163_AFE_RATE_88K;
	case 96000:
		return MT8163_AFE_RATE_96K;
	case 174000:
		return MT8163_AFE_RATE_174K;
	case 192000:
		return MT8163_AFE_RATE_192K;
	case 260000:
		return MT8163_AFE_RATE_260K;
	default:
		return -EINVAL;
	}
}

static const struct snd_pcm_hardware mt8163_afe_hardware = {
	.info = SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_MMAP_VALID,
	.formats = SNDRV_PCM_FMTBIT_S16_LE |
		   SNDRV_PCM_FMTBIT_S24_LE |
		   SNDRV_PCM_FMTBIT_S32_LE,
	.period_bytes_min = 256,
	.period_bytes_max = 4 * 48 * 1024,
	.periods_min = 2,
	.periods_max = 256,
	.buffer_bytes_max = 8 * 48 * 1024,
	.fifo_size = 0,
};

static int mt8163_memif_fs(struct snd_pcm_substream *substream,
			   unsigned int rate)
{
	return mt8163_general_rate_transform(rate);
}

static int mt8163_irq_fs(struct snd_pcm_substream *substream, unsigned int rate)
{
	return mt8163_general_rate_transform(rate);
}

#define MT8163_DAI_MEMIF_DL(_name) {			\
	.name = #_name,					\
	.id = MT8163_AFE_MEMIF_##_name,			\
	.playback = {					\
		.stream_name = #_name,			\
		.channels_min = 1,			\
		.channels_max = 2,			\
		.rates = MTK_PCM_RATES,			\
		.formats = MTK_PCM_FORMATS		\
	},						\
	.ops = &mtk_afe_fe_ops,				\
}

#define MT8163_DAI_MEMIF_UL(_name) {			\
	.name = #_name,					\
	.id = MT8163_AFE_MEMIF_##_name,			\
	.capture = {					\
		.stream_name = #_name,			\
		.channels_min = 1,			\
		.channels_max = 2,			\
		.rates = MTK_PCM_RATES,			\
		.formats = MTK_PCM_FORMATS		\
	},						\
	.ops = &mtk_afe_fe_ops,				\
}

static struct snd_soc_dai_driver mt8163_memif_dai_driver[] = {
	/* FE DAIs: memory intefaces to CPU */
	MT8163_DAI_MEMIF_DL(DL1),
	MT8163_DAI_MEMIF_DL(DL2),
	MT8163_DAI_MEMIF_UL(VUL),
	MT8163_DAI_MEMIF_UL(AWB),
};

/* dma widget & routes*/
static const struct snd_kcontrol_new memif_vul_ch1_mix[] = {
	/* I00 - O09 */
	SOC_DAPM_SINGLE_AUTODISABLE("I2S0_CH1", AFE_CONN5, 8, 1, 0),
	/* I03 - O09 */
	SOC_DAPM_SINGLE_AUTODISABLE("I2S2_CH1", AFE_CONN3, 0, 1, 0),
};

static const struct snd_kcontrol_new memif_vul_ch2_mix[] = {
	/* I01 - O10 */
	SOC_DAPM_SINGLE_AUTODISABLE("I2S0_CH2", AFE_CONN5, 13, 1, 0),
	/* I04 - O10 */
	SOC_DAPM_SINGLE_AUTODISABLE("I2S2_CH2", AFE_CONN3, 3, 1, 0),
};

static const struct snd_kcontrol_new memif_awb_ch1_mix[] = {
	/* I00 - O05 */
	SOC_DAPM_SINGLE_AUTODISABLE("I2S0_CH1", AFE_CONN2, 16, 1, 0),
	/* I05 - O05 */
	SOC_DAPM_SINGLE_AUTODISABLE("DL1_CH1", AFE_CONN2, 19, 1, 0),
	/* I15 - O05 */
	SOC_DAPM_SINGLE_AUTODISABLE("MRG_CH1", AFE_CONN4, 6, 1, 0),
};

static const struct snd_kcontrol_new memif_awb_ch2_mix[] = {
	/* I01 - O06 */
	SOC_DAPM_SINGLE_AUTODISABLE("I2S0_CH2", AFE_CONN2, 22, 1, 0),
	/* I06 - O06 */
	SOC_DAPM_SINGLE_AUTODISABLE("DL1_CH2", AFE_CONN2, 24, 1, 0),
	/* I16 - O06 */
	SOC_DAPM_SINGLE_AUTODISABLE("MRG_CH2", AFE_CONN4, 8, 1, 0),
};

static const struct snd_soc_dapm_widget mt8163_memif_widgets[] = {
	/* memif */
	SND_SOC_DAPM_MIXER("VUL_CH1", SND_SOC_NOPM, 0, 0,
			   memif_vul_ch1_mix, ARRAY_SIZE(memif_vul_ch1_mix)),
	SND_SOC_DAPM_MIXER("VUL_CH2", SND_SOC_NOPM, 0, 0,
			   memif_vul_ch2_mix, ARRAY_SIZE(memif_vul_ch2_mix)),
	SND_SOC_DAPM_MIXER("AWB_CH1", SND_SOC_NOPM, 0, 0,
			   memif_awb_ch1_mix, ARRAY_SIZE(memif_awb_ch1_mix)),
	SND_SOC_DAPM_MIXER("AWB_CH2", SND_SOC_NOPM, 0, 0,
			   memif_awb_ch2_mix, ARRAY_SIZE(memif_awb_ch2_mix)),
};

static const struct snd_soc_dapm_route mt8163_memif_routes[] = {
	/* capture */
	{"AWB", NULL, "AWB_CH1"},
	{"AWB", NULL, "AWB_CH2"},
	{"AWB_CH1", "I2S0_CH1", "I2S0"},
	{"AWB_CH2", "I2S0_CH2", "I2S0"},

	{"VUL", NULL, "VUL_CH1"},
	{"VUL", NULL, "VUL_CH2"},
	{"VUL_CH1", "I2S2_CH1", "I2S2"},
	{"VUL_CH2", "I2S2_CH2", "I2S2"},
};

static const struct snd_soc_component_driver mt8163_afe_pcm_dai_component = {
	.name = "mt8163-afe-pcm-dai",
};

/* Dummy register / field definitions */
#define NO_ENABLE			-1
#define NO_MODE				-1
#define NO_MONO				-1
#define MOD_DAI_DATA_SHIFT		-1
#define DAI_DATA_SHIFT			-1
#define MOD_DAI_PBUF_SIZE_SHIFT		0
#define MOD_DAI_PBUF_SIZE_MASK		0
#define DAI_PBUF_SIZE_SHIFT		0
#define DAI_PBUF_SIZE_MASK		0
#define AWB_PBUF_SIZE_SHIFT		0
#define AWB_PBUF_SIZE_MASK		0
#define VUL_PBUF_SIZE_SHIFT		0
#define VUL_PBUF_SIZE_MASK		0

#define MT8163_MEMIF_BASE(_id, _en_reg, _fs_reg, _mono_reg,		\
			  _pbuf_reg)					\
	[MT8163_AFE_MEMIF_##_id] = {					\
		.name = #_id,						\
		.id = MT8163_AFE_MEMIF_##_id,				\
		.reg_ofs_base = AFE_##_id##_BASE,			\
		.reg_ofs_cur = AFE_##_id##_CUR,				\
		.reg_ofs_end = AFE_##_id##_END,				\
		.fs_reg = _fs_reg,					\
		.fs_shift = _id##_MODE_SHIFT,				\
		.fs_maskbit = _id##_MODE_MASK,				\
		.mono_reg = _mono_reg,					\
		.mono_shift = _id##_DATA_SHIFT,				\
		.enable_reg = _en_reg,					\
		.enable_shift = _id##_ON_SHIFT,				\
		.hd_reg = _pbuf_reg,					\
		.hd_align_reg = _pbuf_reg,				\
		.hd_shift = _id##_HD_SHIFT,				\
		.hd_align_mshift = _id##_HD_ALIGN_SHIFT,		\
		.agent_disable_reg = -1,				\
		.agent_disable_shift = -1,				\
		.msb_reg = AFE_MEMIF_MSB,				\
		.msb_shift = _id##_MSB_SHIFT,				\
		.pbuf_reg = _pbuf_reg,					\
		.pbuf_mask = _id##_PBUF_SIZE_MASK,			\
		.pbuf_shift = _id##_PBUF_SIZE_SHIFT,			\
	}

#define MT8163_MEMIF(_id, _fs_reg, _mono_reg, _pbuf_reg)	\
	MT8163_MEMIF_BASE(_id, AFE_DAC_CON0, _fs_reg, _mono_reg, _pbuf_reg)

static const struct mtk_base_memif_data memif_data[MT8163_AFE_MEMIF_NUM] = {
	MT8163_MEMIF(DL1, AFE_DAC_CON1, AFE_DAC_CON1, AFE_MEMIF_PBUF_SIZE),
	MT8163_MEMIF(DL2, AFE_DAC_CON1, AFE_DAC_CON1, AFE_MEMIF_PBUF_SIZE),
	MT8163_MEMIF(VUL, AFE_DAC_CON1, AFE_DAC_CON1, AFE_MEMIF_PBUF_SIZE),
	MT8163_MEMIF(AWB, AFE_DAC_CON1, AFE_DAC_CON1, AFE_MEMIF_PBUF_SIZE),
};

#define MT8163_AFE_IRQ(_id, _cnt_reg, _fs_reg, _fs_shift,		\
		       _en_shift, _clr_shift)				\
	[MT8163_AFE_##_id] = {						\
		.id = MT8163_AFE_##_id,					\
		.irq_cnt_reg = _cnt_reg,				\
		.irq_cnt_shift = 0,					\
		.irq_cnt_maskbit = (_cnt_reg < 0 ? -1 : 0x3ffff),	\
		.irq_fs_reg = _fs_reg,					\
		.irq_fs_shift = _fs_shift,				\
		.irq_fs_maskbit = (_fs_reg < 0 ? -1 : 0xf),		\
		.irq_en_reg = AFE_IRQ_MCU_CON,				\
		.irq_en_shift = _en_shift,				\
		.irq_clr_reg = AFE_IRQ_MCU_CLR,				\
		.irq_clr_shift = _clr_shift,				\
	}

static const struct mtk_base_irq_data irq_data[MT8163_AFE_IRQ_NUM] = {
	MT8163_AFE_IRQ(IRQ1_MCU, AFE_IRQ_MCU_CNT1, AFE_IRQ_MCU_CON, 4, 0, 0),
	MT8163_AFE_IRQ(IRQ2_MCU, AFE_IRQ_MCU_CNT2, AFE_IRQ_MCU_CON, 8, 1, 1),
	MT8163_AFE_IRQ(IRQ5_MCU, AFE_IRQ_MCU_CNT5, -1, -1, 12, 4),
	MT8163_AFE_IRQ(IRQ7_MCU, AFE_IRQ_MCU_CNT7, AFE_IRQ_MCU_CON, 24, 14, 6),
};

/* Maps MEMIF to IRQ_MCU usage */
static const int memif_irq_usage[MT8163_AFE_MEMIF_NUM] = {
	[MT8163_AFE_MEMIF_DL1] = MT8163_AFE_IRQ1_MCU,
	[MT8163_AFE_MEMIF_DL2] = MT8163_AFE_IRQ7_MCU,
	[MT8163_AFE_MEMIF_VUL] = MT8163_AFE_IRQ2_MCU,
	[MT8163_AFE_MEMIF_AWB] = MT8163_AFE_IRQ2_MCU,
};

static const struct regmap_config mt8163_afe_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.max_register = AFE_MAXLENGTH,
	.cache_type = REGCACHE_NONE,
};

static irqreturn_t mt8163_afe_irq_handler(int irq_id, void *dev)
{
	struct mtk_base_afe *afe = dev;
	struct mtk_base_afe_irq *irq;
	unsigned int status;
	unsigned int status_mcu;
	unsigned int mcu_en;
	int ret;
	int i;
	irqreturn_t irq_ret = IRQ_HANDLED;

	/* Get the IRQs that are sent to MCU */
	ret = regmap_read(afe->regmap, AFE_IRQ_MCU_EN, &mcu_en);
	if (ret) {
		dev_err(afe->dev, "Failed to read AFE_IRQ_MCU_EN register: %d.\n", ret);
		status_mcu = AFE_IRQ_MCU_STATUS_BITS;
		goto err_irq;
	}

	ret = regmap_read(afe->regmap, AFE_IRQ_MCU_STATUS, &status);
	if (ret) {
		dev_err(afe->dev, "Failed to read AFE_IRQ_MCU_STATUS register: %d.\n", ret);
		status_mcu = AFE_IRQ_MCU_STATUS_BITS;
		goto err_irq;
	}

	/* Mask off irrelevant bits - we only care about status */
	status_mcu = status & AFE_IRQ_MCU_STATUS_BITS;

	if (ret || status_mcu == 0) {
		dev_err(afe->dev, "%s(), irq status err, ret %d, status 0x%x, mcu_en 0x%x\n",
			__func__, ret, status, mcu_en);

		irq_ret = IRQ_NONE;
		goto err_irq;
	}

	for (i = 0; i < MT8163_AFE_MEMIF_NUM; i++) {
		struct mtk_base_afe_memif *memif = &afe->memif[i];

		if (!memif->substream)
			continue;

		if (memif->irq_usage < 0)
			continue;

		irq = &afe->irqs[memif->irq_usage];

		/*
		 * Check if the IRQ isn't enabled and if so clear the bits off the
		 * status, to avoid clearing non-MCU interrupts. We need to do it this
		 * way because the control and status bits for each IRQ between the status
		 * and enable register are different, so we can't just use the value of the
		 * AFE_IRQ_MCU_EN register as mask like other SoCs do.
		 */
		if (!(mcu_en & BIT(irq->irq_data->irq_en_shift))) {
			status_mcu &= ~BIT(irq->irq_data->irq_clr_shift);
			continue;
		}

		if (status_mcu & BIT(irq->irq_data->irq_clr_shift))
			snd_pcm_period_elapsed(memif->substream);
	}

err_irq:
	/* clear irq */
	regmap_write(afe->regmap,
		     AFE_IRQ_MCU_CLR,
		     status_mcu);

	return irq_ret;
}

static int mt8163_afe_runtime_suspend(struct device *dev)
{
	struct mtk_base_afe *afe = dev_get_drvdata(dev);

	regmap_clear_bits(afe->regmap, AFE_DAC_CON0, AFE_ON);
	mt8163_afe_disable_clock(afe);
	return 0;
}

static int mt8163_afe_runtime_resume(struct device *dev)
{
	struct mtk_base_afe *afe = dev_get_drvdata(dev);
	int ret;

	ret = mt8163_afe_enable_clock(afe);
	if (ret)
		return ret;

	/* set APB 3.0 */
	regmap_set_bits(afe->regmap, AUDIO_TOP_CON0, APB3_SEL);

	/* enable AFE */
	regmap_set_bits(afe->regmap, AFE_DAC_CON0, AFE_ON);

	return 0;
}

static int mt8163_dai_memif_register(struct mtk_base_afe *afe)
{
	struct mtk_base_afe_dai *dai;

	dai = devm_kzalloc(afe->dev, sizeof(*dai), GFP_KERNEL);
	if (!dai)
		return -ENOMEM;

	list_add(&dai->list, &afe->sub_dais);

	dai->dai_drivers = mt8163_memif_dai_driver;
	dai->num_dai_drivers = ARRAY_SIZE(mt8163_memif_dai_driver);

	dai->dapm_widgets = mt8163_memif_widgets;
	dai->num_dapm_widgets = ARRAY_SIZE(mt8163_memif_widgets);
	dai->dapm_routes = mt8163_memif_routes;
	dai->num_dapm_routes = ARRAY_SIZE(mt8163_memif_routes);
	return 0;
}

typedef int (*dai_register_cb)(struct mtk_base_afe *);
static const dai_register_cb dai_register_cbs[] = {
	mt8163_dai_i2s_register,
	mt8163_dai_memif_register,
};

static int mt8163_afe_pcm_dev_probe(struct platform_device *pdev)
{
	struct mtk_base_afe *afe;
	struct mt8163_afe_private *afe_priv;
	struct device *dev = &pdev->dev;
	int i, irq_id, ret;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	afe = devm_kzalloc(dev, sizeof(*afe), GFP_KERNEL);
	if (!afe)
		return -ENOMEM;
	platform_set_drvdata(pdev, afe);

	afe->platform_priv = devm_kzalloc(dev, sizeof(*afe_priv), GFP_KERNEL);
	if (!afe->platform_priv)
		return -ENOMEM;

	afe_priv = afe->platform_priv;
	afe->dev = dev;

	ret = of_reserved_mem_device_init(dev);
	if (ret) {
		dev_info(dev, "no reserved memory found, pre-allocating buffers instead\n");
		afe->preallocate_buffers = true;
	}

	/* initial audio related clock */
	ret = mt8163_init_clock(afe);
	if (ret) {
		dev_err(dev, "init clock error\n");
		return ret;
	}

	/* regmap init */
	afe->regmap = syscon_node_to_regmap(dev->parent->of_node);
	if (IS_ERR(afe->regmap)) {
		dev_err(dev, "could not get regmap from parent\n");
		ret = PTR_ERR(afe->regmap);
		goto err_pm_disable;
	}

	ret = regmap_attach_dev(dev, afe->regmap, &mt8163_afe_regmap_config);
	if (ret) {
		dev_warn(dev, "regmap_attach_dev fail, ret %d\n", ret);
		goto err_pm_disable;
	}

	/* init memif */
	afe->memif_size = MT8163_AFE_MEMIF_NUM;
	afe->memif = devm_kcalloc(dev, afe->memif_size, sizeof(*afe->memif),
				  GFP_KERNEL);
	if (!afe->memif) {
		ret = -ENOMEM;
		goto err_pm_disable;
	}

	for (i = 0; i < afe->memif_size; i++) {
		afe->memif[i].data = &memif_data[i];
		afe->memif[i].irq_usage = memif_irq_usage[i];
		afe->memif[i].const_irq = 1;
	}

	mutex_init(&afe->irq_alloc_lock);

	/* irq initialize */
	afe->irqs_size = MT8163_AFE_IRQ_NUM;
	afe->irqs = devm_kcalloc(dev, afe->irqs_size, sizeof(*afe->irqs),
				 GFP_KERNEL);
	if (!afe->irqs) {
		ret = -ENOMEM;
		goto err_pm_disable;
	}

	for (i = 0; i < afe->irqs_size; i++)
		afe->irqs[i].irq_data = &irq_data[i];

	/* request irq */
	irq_id = platform_get_irq(pdev, 0);
	if (irq_id < 0) {
		ret = irq_id;
		goto err_pm_disable;
	}

	ret = devm_request_irq(dev, irq_id, mt8163_afe_irq_handler,
			       IRQF_TRIGGER_NONE, "asys-isr", (void *)afe);
	if (ret) {
		dev_err(dev, "could not request_irq for asys-isr\n");
		goto err_pm_disable;
	}

	/* init sub_dais */
	INIT_LIST_HEAD(&afe->sub_dais);

	for (i = 0; i < ARRAY_SIZE(dai_register_cbs); i++) {
		ret = dai_register_cbs[i](afe);
		if (ret) {
			dev_warn(dev, "dai register i %d fail, ret %d\n",
				 i, ret);
			goto err_pm_disable;
		}
	}

	/* init dai_driver and component_driver */
	ret = mtk_afe_combine_sub_dai(afe);
	if (ret) {
		dev_warn(dev, "mtk_afe_combine_sub_dai fail, ret %d\n", ret);
		goto err_pm_disable;
	}

	afe->mtk_afe_hardware = &mt8163_afe_hardware;
	afe->memif_fs = mt8163_memif_fs;
	afe->irq_fs = mt8163_irq_fs;

	pm_runtime_enable(dev);
	if (!pm_runtime_enabled(dev)) {
		ret = mt8163_afe_runtime_resume(dev);
		if (ret)
			goto err_pm_disable;
	}

	afe->reg_back_up_list = mt8163_afe_backup_list;
	afe->reg_back_up_list_num = ARRAY_SIZE(mt8163_afe_backup_list);

	afe->runtime_resume = mt8163_afe_runtime_resume;
	afe->runtime_suspend = mt8163_afe_runtime_suspend;

	/* register component */
	ret = devm_snd_soc_register_component(dev, &mtk_afe_pcm_platform,
					      NULL, 0);
	if (ret) {
		dev_warn(dev, "err_platform\n");
		goto err_pm_disable;
	}

	ret = devm_snd_soc_register_component(dev, &mt8163_afe_pcm_dai_component,
					      afe->dai_drivers,
					      afe->num_dai_drivers);
	if (ret) {
		dev_warn(dev, "err_dai_component\n");
		goto err_pm_disable;
	}

	return ret;

err_pm_disable:
	pm_runtime_disable(dev);
	return ret;
}

static void mt8163_afe_pcm_dev_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;

	snd_soc_unregister_component(dev);

	pm_runtime_disable(dev);
	if (!pm_runtime_status_suspended(dev))
		mt8163_afe_runtime_suspend(dev);
}

static const struct of_device_id mt8163_afe_pcm_dt_match[] = {
	{ .compatible = "mediatek,mt8163-afe-pcm", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mt8163_afe_pcm_dt_match);

static const struct dev_pm_ops mt8163_afe_pm_ops = {
	RUNTIME_PM_OPS(mt8163_afe_runtime_suspend,
		       mt8163_afe_runtime_resume, NULL)
};

static struct platform_driver mt8163_afe_pcm_driver = {
	.driver = {
		   .name = "mt8163-afe-pcm",
		   .of_match_table = mt8163_afe_pcm_dt_match,
		   .pm = pm_ptr(&mt8163_afe_pm_ops),
	},
	.probe = mt8163_afe_pcm_dev_probe,
	.remove = mt8163_afe_pcm_dev_remove,
};

module_platform_driver(mt8163_afe_pcm_driver);

MODULE_DESCRIPTION("Mediatek ALSA SoC AFE platform driver");
MODULE_AUTHOR("Ben Grisdale <bengris32@protonmail.ch>");
MODULE_LICENSE("GPL v2");
