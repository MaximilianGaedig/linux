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
	SND_SOC_DAPM_MIC("Mic Array", NULL),
};

/* The aic32x4's line outputs drive the on-board amplifier. */
/*
 * Feed every ADC input from the mic-array widget.
 *
 * The card is fully_routed, so a widget with no route to an endpoint stays
 * powered down forever - which is what kept the ADCs silent even once they
 * were bound to the right driver and their PLL locked. Inside each ADC the
 * path is IN_xL -> "IN_xL Capture Switch" -> Left Input -> Left PGA ->
 * Left ADC, and those mixer switches default to off, so userspace still has
 * to close one; declaring all six inputs on each part lets DAPM complete a
 * path for whichever one this board actually wires without needing the
 * schematic first.
 */
#define BISCUIT_MIC_ROUTES(prefix)					\
	{ prefix " IN_1L", NULL, "Mic Array" },				\
	{ prefix " IN_1R", NULL, "Mic Array" },				\
	{ prefix " IN_2L", NULL, "Mic Array" },				\
	{ prefix " IN_2R", NULL, "Mic Array" },				\
	{ prefix " IN_3L", NULL, "Mic Array" },				\
	{ prefix " IN_3R", NULL, "Mic Array" }

static const struct snd_soc_dapm_route biscuit_routes[] = {
	{ "Ext Spk", NULL, "Speaker LOL" },
	{ "Ext Spk", NULL, "Speaker LOR" },
	BISCUIT_MIC_ROUTES("Mic0"),
	BISCUIT_MIC_ROUTES("Mic1"),
	BISCUIT_MIC_ROUTES("Mic2"),
	BISCUIT_MIC_ROUTES("Mic3"),
};

SND_SOC_DAILINK_DEFS(playback_fe,
	DAILINK_COMP_ARRAY(COMP_EMPTY()),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

#define BISCUIT_N_MICS	4

/*
 * All four ADCs are named, not just the clock master.
 *
 * Naming only the master (which is what Amazon's own link does) makes ASoC
 * intersect the CPU DAI's 9 channels with one ADC's 2, producing an empty
 * range and -EINVAL from the very open() - snd_soc_runtime_calc_hw() only
 * defers to the CPU side once a link has more than one CODEC. Listing all
 * four is also simply truthful: they all sit on this TDM bus and they all
 * need configuring.
 */
SND_SOC_DAILINK_DEFS(mic_capture,
	DAILINK_COMP_ARRAY(COMP_EMPTY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY(), COMP_EMPTY(),
			   COMP_EMPTY(), COMP_EMPTY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

/*
 * Mic-array clocking, straight from Amazon's tlv3101_hw_params().
 *
 * The ADC is the bit- and frame-clock master and the bus is TDM (DSP_B),
 * not plain I2S - so the codec, not the FPGA, generates the clocks the
 * FPGA is waiting for. Until this ran, the FPGA sat with i2s_inactive=1.
 *
 * Note mainline's ADC3101 driver only accepts DSP_B together with IB_NF,
 * while Amazon passes NB_NF; the two drivers program the bit-clock
 * polarity bit from opposite defaults, so IB_NF here is the same wire
 * behaviour their NB_NF produced.
 */
#define BISCUIT_MIC_MCLK_HZ	9600000

static int biscuit_mic_hw_params(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *codec_dai;
	int i, ret;

	/*
	 * Exactly one ADC drives the bus.
	 *
	 * The link cannot express this through .dai_fmt, which is applied
	 * identically to every CODEC on it - and four bit-clock masters on one
	 * TDM bus is a short, not an audio path. So set the format per CODEC
	 * here: the first is the bit- and frame-clock provider, the rest are
	 * consumers, which is how Amazon wires the array (only
	 * tlv320aic3101.0-0018 is named in their link, as the master).
	 */
	for_each_rtd_codec_dais(rtd, i, codec_dai) {
		unsigned int fmt = SND_SOC_DAIFMT_DSP_B |
				   SND_SOC_DAIFMT_IB_NF |
				   (i == 0 ? SND_SOC_DAIFMT_CBP_CFP
					   : SND_SOC_DAIFMT_CBC_CFC);

		ret = snd_soc_dai_set_fmt(codec_dai, fmt);
		if (ret && ret != -ENOTSUPP) {
			dev_err(rtd->dev, "biscuit: mic%d set_fmt failed: %d\n",
				i, ret);
			return ret;
		}

		ret = snd_soc_dai_set_sysclk(codec_dai, 0, BISCUIT_MIC_MCLK_HZ,
					     SND_SOC_CLOCK_IN);
		if (ret && ret != -ENOTSUPP) {
			dev_err(rtd->dev, "biscuit: mic%d set_sysclk(%u) failed: %d\n",
				i, BISCUIT_MIC_MCLK_HZ, ret);
			return ret;
		}

		/*
		 * Give each ADC its own pair of slots on the shared bus.
		 *
		 * This is not optional bookkeeping: on a multi-CODEC link ASoC
		 * narrows the params it hands each CODEC with
		 * soc_pcm_codec_params_fixup(), driven by that DAI's rx_mask.
		 * With no mask set the mask is zero, the fixup asks the ADC for
		 * a zero-channel stream and hw_params comes straight back as
		 * -EINVAL - which is what the capture tool saw while the
		 * machine-level hw_params above appeared to succeed.
		 *
		 * snd_soc_dai_set_tdm_slot() stores the masks in the DAI before
		 * it dispatches to the CODEC op, so this does its job even
		 * though mainline's ADC3101 driver implements no .set_tdm_slot
		 * and answers -ENOTSUPP.
		 */
		ret = snd_soc_dai_set_tdm_slot(codec_dai, 0,
					       0x3 << (2 * i),
					       params_channels(params),
					       snd_pcm_format_width(params_format(params)));
		if (ret && ret != -ENOTSUPP) {
			dev_err(rtd->dev, "biscuit: mic%d set_tdm_slot failed: %d\n",
				i, ret);
			return ret;
		}
	}

	dev_info(rtd->dev, "biscuit: mic hw_params rate=%u ch=%u width=%d\n",
		 params_rate(params), params_channels(params),
		 snd_pcm_format_width(params_format(params)));
	return 0;
}

/*
 * Read back what actually landed in each ADC after the whole chain has been
 * configured and powered. INTERFACE_CTRL_1 bits 3/2 are BCLK/WCLK master;
 * PLL_PROG_PR bit7 is PLL power; ADC_DIGITAL bits 7/6 power the L/R ADCs.
 * If master is not set or the ADCs are unpowered, the part cannot be
 * clocking the FPGA - which is what "i2s_inactive=1, nframes=0" says.
 */
static int biscuit_mic_prepare(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *codec_dai;
	int i;

	for_each_rtd_codec_dais(rtd, i, codec_dai) {
		struct snd_soc_component *c = codec_dai->component;
		int ifc = snd_soc_component_read(c, 27);   /* INTERFACE_CTRL_1 */
		int pll = snd_soc_component_read(c, 5);     /* PLL_PROG_PR */
		int nadc = snd_soc_component_read(c, 18);   /* ADC_NADC */
		int madc = snd_soc_component_read(c, 19);   /* ADC_MADC */
		int adcd = snd_soc_component_read(c, 81);   /* ADC_DIGITAL */

		dev_info(rtd->dev,
			 "biscuit-micreg mic%d: IFACE1=0x%02x(master=%d) PLL_PR=0x%02x(on=%d) NADC=0x%02x MADC=0x%02x ADC_DIG=0x%02x(L=%d R=%d)\n",
			 i, ifc & 0xff, !!(ifc & 0x0c), pll & 0xff, !!(pll & 0x80),
			 nadc & 0xff, madc & 0xff, adcd & 0xff,
			 !!(adcd & 0x80), !!(adcd & 0x40));
	}
	return 0;
}

static const struct snd_soc_ops biscuit_mic_ops = {
	.hw_params = biscuit_mic_hw_params,
	.prepare = biscuit_mic_prepare,
};

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
	/*
	 * Mic array. This one does not touch the AFE at all.
	 *
	 * The four aic3x mic codecs feed I2S into the "dough" FPGA rather than
	 * into the SoC; the FPGA packs all channels and hands finished frames
	 * back over SPI0, so the capture path is a single self-contained
	 * component that is CPU DAI and PCM platform at once - which is
	 * exactly how Amazon's own machine driver wires it ("AMZN_SPI_Capture",
	 * cpu_dai_name = platform_name = AMZN_MT_SPI_PCM).
	 *
	 * This link used to be absent because the FPGA never configured and
	 * would have registered a capture device that could only ever return
	 * silence.  It configures now (it reports revision 34), so the link
	 * comes back.
	 */
	{
		.name = "MIC_SPI",
		.stream_name = "Mic Capture",
		.capture_only = 1,
		.ops = &biscuit_mic_ops,
		SND_SOC_DAILINK_REG(mic_capture),
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
	struct device_node *platform, *codec, *fe_cpu, *be_cpu, *mic;
	struct device_node *mic_codec[BISCUIT_N_MICS];
	struct snd_soc_dai_link *link;
	int i, j;

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

	/*
	 * The mic FPGA is optional: if its node is absent or its driver has
	 * not probed, drop that link rather than failing the whole card and
	 * taking the speaker down with it.
	 */
	mic = of_parse_phandle(pdev->dev.of_node, "amazon,mic-fpga", 0);
	for (i = 0; i < BISCUIT_N_MICS; i++)
		mic_codec[i] = of_parse_phandle(pdev->dev.of_node,
						"amazon,mic-codec", i);
	if (!mic || !mic_codec[0])
		card->num_links = ARRAY_SIZE(biscuit_dai_links) - 1;

	for_each_card_prelinks(card, i, link) {
		if (!strcmp(link->name, "MIC_SPI")) {
			link->cpus->of_node = mic;
			link->cpus->dai_name = "biscuit-spi-pcm";
			link->platforms->of_node = mic;
			for (j = 0; j < BISCUIT_N_MICS; j++) {
				link->codecs[j].of_node = mic_codec[j];
				link->codecs[j].dai_name = "tlv320adc3xxx-hifi";
			}
		} else if (!strcmp(link->name, "DL1_FE")) {
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

	dev_info(&pdev->dev, "biscuit-audio: FE=DL1 BE=I2S1 platform=%pOF codec=%pOF mic=%pOF\n",
		 platform, codec, mic);

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
