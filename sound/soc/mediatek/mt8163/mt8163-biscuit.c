// SPDX-License-Identifier: GPL-2.0
/*
 * ASoC machine driver for the Amazon Echo Dot 2nd Gen (biscuit).
 *
 * Why this exists instead of simple-audio-card: the MT8163 AFE splits the
 * playback path into a front-end memory interface (DL1) and a separate I2S
 * back-end DAI, and only a DPCM card can bind the two. simple-audio-card can
 * only build a plain FE<->codec link, which configures the memif but never the
 * I2S back-end, so samples are DMA'd into the AFE and never clocked out of the
 * SoC pins - the codec plays silence no matter how its mixers are set.
 *
 * Note MT8163's I2S DAIs are unidirectional: I2S0/I2S2 are capture only and
 * I2S1/I2S3 are playback, so the speaker back-end must be an odd-numbered one.
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

static const struct snd_soc_dapm_widget biscuit_widgets[] = {
	SND_SOC_DAPM_SPK("Ext Spk", NULL),
};

/* The aic32x4's line outputs drive the on-board amplifier. */
static const struct snd_soc_dapm_route biscuit_routes[] = {
	{ "Ext Spk", NULL, "Speaker LOL" },
	{ "Ext Spk", NULL, "Speaker LOR" },
};

SND_SOC_DAILINK_DEFS(playback_fe,
	DAILINK_COMP_ARRAY(COMP_EMPTY()),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(playback_be,
	DAILINK_COMP_ARRAY(COMP_EMPTY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()),
	DAILINK_COMP_ARRAY(COMP_DUMMY()));

/*
 * Give the codec a master clock.
 *
 * Nothing was doing this, and the omission is silent in the worst way: the
 * AFE only remembers an MCLK rate if somebody calls set_sysclk on the I2S
 * DAI (mtk_dai_i2s_set_sysclk() is the sole writer of i2s_priv->mclk_rate).
 * With no caller the rate stays 0, so mtk_mclk_en_event() asks for
 * mt8163_mck_enable(afe, id, 0) and the MCLK widget's connect check -
 * "return (i2s_priv->mclk_rate > 0) ? 1 : 0" - reports the path as
 * disconnected.  The result is an I2S port that clocks BCK and LRCK and
 * shifts data out perfectly while pin 15 (I2S1_MCK) stays dead.
 *
 * The aic32x4 needs that pin: it has no crystal of its own on this board,
 * it derives everything from MCLK, and its own device node in the device
 * tree claims MCLK is a fixed 26MHz from clk26m - which is not what the
 * pin is wired to since the AFE took it over.  So the codec was being told
 * one rate and handed none at all, which is consistent with a chain that
 * verifies correct end to end and still makes no sound.
 *
 * 256 * fs is the conventional MTK/aic32x4 ratio and divides both APLLs
 * exactly, which mtk_dai_i2s_set_sysclk() insists on (it rejects any freq
 * that does not divide the APLL rate).
 */
static int biscuit_be_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);
	unsigned int mclk = params_rate(params) * 256;
	int ret;

	ret = snd_soc_dai_set_sysclk(cpu_dai, 0, mclk, SND_SOC_CLOCK_OUT);
	if (ret && ret != -ENOTSUPP) {
		dev_err(rtd->dev, "biscuit: AFE set_sysclk(%u) failed: %d\n",
			mclk, ret);
		return ret;
	}

	/*
	 * The codec side is best-effort on purpose.  aic32x4_set_dai_sysclk()
	 * resolves its "pll" clock and calls clk_set_rate() on that clock's
	 * parent; with the parent declared as clk26m - a fixed-rate clock -
	 * that call cannot succeed.  The codec still works out its PLL
	 * dividers from the rate it believes MCLK to be, so a failure here is
	 * worth reporting but must not take the stream down with it.
	 */
	ret = snd_soc_dai_set_sysclk(codec_dai, 0, mclk, SND_SOC_CLOCK_IN);
	if (ret && ret != -ENOTSUPP)
		dev_warn(rtd->dev, "biscuit: codec set_sysclk(%u): %d\n",
			 mclk, ret);

	dev_info(rtd->dev, "biscuit: BE hw_params rate=%u mclk=%u\n",
		 params_rate(params), mclk);
	return 0;
}

static const struct snd_soc_ops biscuit_be_ops = {
	.hw_params = biscuit_be_hw_params,
};

static struct snd_soc_dai_link biscuit_dai_links[] = {
	/* Front end: the DL1 memory interface owns the PCM device. */
	{
		.name = "DL1_FE",
		.stream_name = "Playback",
		.dynamic = 1,
		.playback_only = 1,
		.trigger = { SND_SOC_DPCM_TRIGGER_POST,
			     SND_SOC_DPCM_TRIGGER_POST },
		SND_SOC_DAILINK_REG(playback_fe),
	},
	/* Back end: the I2S port that actually drives the codec. */
	{
		.name = "I2S_BE",
		.no_pcm = 1,
		.playback_only = 1,
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
			   SND_SOC_DAIFMT_CBC_CFC,
		.ops = &biscuit_be_ops,
		SND_SOC_DAILINK_REG(playback_be),
	},
};

static struct snd_soc_card biscuit_card = {
	.name		= "biscuit-audio",
	.owner		= THIS_MODULE,
	.dai_link	= biscuit_dai_links,
	.num_links	= ARRAY_SIZE(biscuit_dai_links),
	.dapm_widgets	= biscuit_widgets,
	.num_dapm_widgets = ARRAY_SIZE(biscuit_widgets),
	.dapm_routes	= biscuit_routes,
	.num_dapm_routes = ARRAY_SIZE(biscuit_routes),
	.fully_routed	= true,
};

static int biscuit_snd_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card = &biscuit_card;
	struct device_node *platform, *codec, *fe_cpu, *be_cpu;
	struct snd_soc_dai_link *link;
	int i;

	card->dev = &pdev->dev;

	platform = of_parse_phandle(pdev->dev.of_node, "mediatek,platform", 0);
	if (!platform)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "missing mediatek,platform\n");

	codec = of_parse_phandle(pdev->dev.of_node, "mediatek,audio-codec", 0);
	if (!codec)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "need mediatek,audio-codec\n");

	/*
	 * Bind the AFE DAIs by name rather than by a "<&afe N>" phandle: a
	 * phandle parsed with of_parse_phandle() drops its argument cell, so
	 * both links would resolve to the bare AFE node with no DAI selected
	 * and DPCM would find no back end ("no backend DAIs enabled").
	 * DL1 is the playback front-end memif; I2S1 is the playback back-end
	 * (I2S0/I2S2 are capture-only on this SoC).
	 */
	fe_cpu = platform;
	be_cpu = platform;

	for_each_card_prelinks(card, i, link) {
		if (!strcmp(link->name, "DL1_FE")) {
			link->cpus->of_node = fe_cpu;
			link->cpus->dai_name = "DL1";
			link->platforms->of_node = platform;
		} else {
			link->cpus->of_node = be_cpu;
			link->cpus->dai_name = "I2S1";
			link->codecs->of_node = codec;
			link->codecs->dai_name = "tlv320aic32x4-hifi";
		}
	}

	dev_info(&pdev->dev, "biscuit-audio: FE=DL1 BE=I2S1 platform=%pOF codec=%pOF\n",
		 platform, codec);

	return devm_snd_soc_register_card(&pdev->dev, card);
}

static const struct of_device_id biscuit_snd_of_match[] = {
	{ .compatible = "amazon,biscuit-audio" },
	{ }
};
MODULE_DEVICE_TABLE(of, biscuit_snd_of_match);

static struct platform_driver biscuit_snd_driver = {
	.driver = {
		.name = "biscuit-audio",
		.of_match_table = biscuit_snd_of_match,
	},
	.probe = biscuit_snd_probe,
};
module_platform_driver(biscuit_snd_driver);

MODULE_DESCRIPTION("Amazon Echo Dot 2 (biscuit) ASoC machine driver");
MODULE_LICENSE("GPL");
