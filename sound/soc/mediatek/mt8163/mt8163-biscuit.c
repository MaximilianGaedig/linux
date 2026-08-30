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
