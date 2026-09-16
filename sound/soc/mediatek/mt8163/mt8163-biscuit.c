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
#include <linux/io.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-card.h>
#include <sound/jack.h>
#include <linux/gpio/consumer.h>

#include "../../codecs/tlv320aic32x4.h"

static struct snd_soc_jack biscuit_hp_jack;
static struct snd_soc_jack_gpio biscuit_hp_gpio = {
	.name = "hp-det",
	.report = SND_JACK_LINEOUT,
	.debounce_time = 200,
	/* Polarity comes from the DT flags: the detect pin reads high when plugged. */
};

static const struct snd_soc_dapm_widget biscuit_widgets[] = {
	SND_SOC_DAPM_SPK("Ext Spk", NULL),
	SND_SOC_DAPM_MIC("Mic Array", NULL),
	/*
	 * External amplifier enable (GPIO 122), as a supply so DAPM enables it
	 * only while the speaker path is powered; keeping it on makes the
	 * speaker hiss.
	 */
	SND_SOC_DAPM_REGULATOR_SUPPLY("ext-spk-amp", 0, 0),
	/*
	 * Second amplifier enable (GPIO 124, the extamp pin in the vendor
	 * device tree), gated the same way.
	 */
	SND_SOC_DAPM_REGULATOR_SUPPLY("ext-spk-amp2", 0, 0),
};

/*
 * Feed every ADC input from the mic-array widget. The card is fully
 * routed, so an input without a route to an endpoint is never powered.
 * The differential inputs are separate widgets from the single-ended
 * ones and need their own routes.
 */
#define BISCUIT_MIC_ROUTES(prefix)					\
	{ prefix " IN_1L", NULL, "Mic Array" },				\
	{ prefix " IN_1R", NULL, "Mic Array" },				\
	{ prefix " IN_2L", NULL, "Mic Array" },				\
	{ prefix " IN_2R", NULL, "Mic Array" },				\
	{ prefix " IN_3L", NULL, "Mic Array" },				\
	{ prefix " IN_3R", NULL, "Mic Array" },				\
	{ prefix " DIFL_1L_1R", NULL, "Mic Array" },			\
	{ prefix " DIFL_2L_3L", NULL, "Mic Array" },			\
	{ prefix " DIFL_2R_3R", NULL, "Mic Array" },			\
	{ prefix " DIFR_1L_1R", NULL, "Mic Array" },			\
	{ prefix " DIFR_2L_3L", NULL, "Mic Array" },			\
	{ prefix " DIFR_2R_3R", NULL, "Mic Array" }

static const struct snd_soc_dapm_route biscuit_routes[] = {
	/*
	 * The amplifier is driven from the HPL/HPR outputs (the vendor
	 * configuration only enables the HP output mixers). The LOL/LOR routes
	 * are kept so the line outputs can be driven as well.
	 */
	{ "Ext Spk", NULL, "ext-spk-amp" },
	{ "Ext Spk", NULL, "ext-spk-amp2" },
	{ "Ext Spk", NULL, "Speaker HPL" },
	{ "Ext Spk", NULL, "Speaker HPR" },
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

/* 0x03, 0x0c, 0x30, 0x40: two channels each except the last, seven in all. */
#define BISCUIT_MIC_RX_MASK(i)	((i) == BISCUIT_N_MICS - 1 ? \
				 0x1 << (2 * (i)) : 0x3 << (2 * (i)))

/*
 * All four ADCs sit on the TDM bus and are part of the link. With a single
 * CODEC, ASoC would also intersect the CPU DAI's 9 channels with the ADC's
 * 2 and reject every configuration.
 */
SND_SOC_DAILINK_DEFS(mic_capture,
	DAILINK_COMP_ARRAY(COMP_EMPTY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY(), COMP_EMPTY(),
			   COMP_EMPTY(), COMP_EMPTY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

/*
 * The mic ADCs are clocked from the 9.6MHz CMMCLK pad. The first ADC is
 * the bit- and frame-clock master of the DSP_B TDM bus, which also clocks
 * the FPGA's I2S input. The tlv320adc3xxx driver only accepts DSP_B with
 * IB_NF, which gives the wire polarity the vendor driver's NB_NF does.
 */
#define BISCUIT_MIC_MCLK_HZ	9600000

static int biscuit_mic_hw_params(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *codec_dai;
	int i, ret;

	/*
	 * Exactly one ADC may drive the bus clocks, which .dai_fmt cannot
	 * express since it applies to every CODEC on the link, so set the
	 * format per CODEC: the first is clock provider, the rest consume.
	 */
	for_each_rtd_codec_dais(rtd, i, codec_dai) {
		unsigned int fmt = SND_SOC_DAIFMT_DSP_B |
				   SND_SOC_DAIFMT_IB_NF |
				   (i == 0
					   ? SND_SOC_DAIFMT_CBP_CFP
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
		 * Give each ADC its slots on the shared bus. Seven microphones
		 * across four stereo parts: 0x03, 0x0c, 0x30, 0x40. The eighth
		 * slot carries the FPGA's DAC loopback reference and must not be
		 * driven by an ADC. The masks also drive ASoC's per-CODEC
		 * channel fixup on this multi-CODEC link.
		 */
		ret = snd_soc_dai_set_tdm_slot(codec_dai, 0,
					       BISCUIT_MIC_RX_MASK(i),
					       params_channels(params),
					       snd_pcm_format_width(params_format(params)));
		if (ret && ret != -ENOTSUPP) {
			dev_err(rtd->dev, "biscuit: mic%d set_tdm_slot failed: %d\n",
				i, ret);
			return ret;
		}
	}

	/*
	 * Input routing and PGA gain are DAPM controls and cannot be changed
	 * from hw_params (the card mutex is already held); userspace sets
	 * them.
	 */
	return 0;
}

static const struct snd_soc_ops biscuit_mic_ops = {
	.hw_params = biscuit_mic_hw_params,
};

SND_SOC_DAILINK_DEFS(playback_be,
	DAILINK_COMP_ARRAY(COMP_EMPTY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()),
	DAILINK_COMP_ARRAY(COMP_DUMMY()));

/*
 * Switch on the given card mixer controls (both channels) and resync
 * DAPM.
 */
static void biscuit_close_mixers(struct snd_soc_card *card,
				 const char * const *mixers, int n)
{
	struct snd_ctl_elem_value uval;
	struct snd_kcontrol *kctl;
	int i;

	for (i = 0; i < n; i++) {
		kctl = snd_soc_card_get_kcontrol(card, mixers[i]);
		if (!kctl || !kctl->put) {
			dev_warn(card->dev, "biscuit: no control '%s'\n",
				 mixers[i]);
			continue;
		}
		memset(&uval, 0, sizeof(uval));
		uval.value.integer.value[0] = 1;
		uval.value.integer.value[1] = 1;
		kctl->put(kctl, &uval);
	}
	snd_soc_dapm_sync(card->dapm);
}

/*
 * The AFE clocks every I2S sample in a 32-bit slot, whatever the sample
 * width, so BCLK is 64 * fs.
 */
#define BISCUIT_SPK_BCLK_FS	64

static int biscuit_be_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);
	unsigned int bclk = params_rate(params) * BISCUIT_SPK_BCLK_FS;
	int ret;

	/*
	 * The speaker codec has no usable MCLK on this board: run its PLL from
	 * BCLK. No set_sysclk on the AFE side, which only provides BCK and LRCK
	 * here.
	 */
	ret = snd_soc_dai_set_sysclk(codec_dai, AIC32X4_PLL_CLKIN_BCKL, bclk,
				     SND_SOC_CLOCK_IN);
	if (ret) {
		dev_err(rtd->dev, "biscuit: codec PLL from BCLK (%u Hz): %d\n",
			bclk, ret);
		return ret;
	}

	/*
	 * Unmute the external amplifier. Its mute line is the codec's MFP2
	 * pin, used as a GPIO output (register 53): 0x05 mutes, 0x04 unmutes.
	 */
	snd_soc_component_write(codec_dai->component, 53, 0x04);

	return 0;
}

/*
 * Codec output stage setup, done in prepare so it overrides what the codec
 * driver programmed in hw_params: power the HPL/HPR drivers, route both
 * DACs to them and unmute them.
 *
 * Register numbers are the flat aic32x4 map (page * 128 + offset).
 */
static int biscuit_be_prepare(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_component *comp = snd_soc_rtd_to_codec(rtd, 0)->component;

	/* Analog output: power the drivers, route both DACs, lift the mutes. */
	snd_soc_component_write(comp, 128 + 9, 0x33);	/* OUTPWRCTL: LOL/LOR + HPL/HPR on */
	snd_soc_component_write(comp, 128 + 12, 0x08);	/* HPLROUTE: L_DAC */
	snd_soc_component_write(comp, 128 + 13, 0x08);	/* HPRROUTE: R_DAC */
	snd_soc_component_write(comp, 128 + 14, 0x08);	/* LOLROUTE: L_DAC */
	snd_soc_component_write(comp, 128 + 15, 0x08);	/* LORROUTE: R_DAC */
	snd_soc_component_write(comp, 128 + 16, 0x04);	/* HPLGAIN: unmuted */
	snd_soc_component_write(comp, 128 + 17, 0x04);	/* HPRGAIN: unmuted */
	snd_soc_component_write(comp, 128 + 18, 0x04);	/* LOLGAIN: unmuted */
	snd_soc_component_write(comp, 128 + 19, 0x04);	/* LORGAIN: unmuted */

	return 0;
}

static const struct snd_soc_ops biscuit_be_ops = {
	.hw_params = biscuit_be_hw_params,
	.prepare = biscuit_be_prepare,
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
	 * Mic array. The ADCs feed the FPGA rather than the AFE; the FPGA
	 * driver is both CPU DAI and PCM platform of this link.
	 */
	{
		.name = "MIC_SPI",
		.stream_name = "Mic Capture",
		.capture_only = 1,
		.ops = &biscuit_mic_ops,
		SND_SOC_DAILINK_REG(mic_capture),
	},
};

/*
 * The mic array is always listening and has no routing choice, so force
 * its input pins on; this powers the ADC clocks and the ADCs, which in
 * turn clock the FPGA.
 */
static int biscuit_late_probe(struct snd_soc_card *card)
{
	struct snd_soc_dapm_context *dapm = card->dapm;
	/*
	 * The vendor "DIF1" input is page 1 register 52/55 bit 7, which the
	 * tlv320adc3xxx driver names DIF_2L_3L / DIF_2R_3R.
	 */
	static const char * const pins[] = {
		"Mic Array",
		"Mic0 DIFL_2L_3L", "Mic0 DIFR_2R_3R",
		"Mic1 DIFL_2L_3L", "Mic1 DIFR_2R_3R",
		"Mic2 DIFL_2L_3L", "Mic2 DIFR_2R_3R",
		"Mic3 DIFL_2L_3L", "Mic3 DIFR_2R_3R",
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(pins); i++)
		snd_soc_dapm_force_enable_pin(dapm, pins[i]);
	snd_soc_dapm_sync(dapm);

	/*
	 * Close the AFE's DL1 -> I2S1 routing mixers. They default to off,
	 * and without them dpcm_path_get() finds no back end when the
	 * playback PCM is opened.
	 */
	{
		static const char * const afe_routes[] = {
			"I2S1_CH1 DL1_CH1", "I2S1_CH2 DL1_CH2",
		};
		biscuit_close_mixers(card, afe_routes, ARRAY_SIZE(afe_routes));
	}

	/*
	 * 3.5mm line-out presence. The detect line is a GPIO the sound node
	 * names as amazon,hp-det-gpios; hand it to the ASoC jack-gpio helper
	 * so plug in/out shows up as SND_JACK_LINEOUT on a "Line Out Jack"
	 * input device. Best-effort: a board without the pin populated should
	 * still bring the card up.
	 */
	biscuit_hp_gpio.gpiod_dev = card->dev;
	biscuit_hp_gpio.idx = 0;
	if (of_property_present(card->dev->of_node, "amazon,hp-det-gpios")) {
		int ret = snd_soc_card_jack_new(card, "Line Out Jack",
						SND_JACK_LINEOUT, &biscuit_hp_jack);
		if (ret) {
			dev_warn(card->dev, "biscuit: jack_new failed: %d\n", ret);
		} else {
			/* the DT property is "amazon,hp-det-gpios" */
			biscuit_hp_gpio.name = "amazon,hp-det";
			ret = snd_soc_jack_add_gpios(&biscuit_hp_jack, 1,
						     &biscuit_hp_gpio);
			if (ret)
				dev_warn(card->dev, "biscuit: add jack gpio: %d\n", ret);
		}
	}
	return 0;
}

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
	.late_probe	= biscuit_late_probe,
};

static int biscuit_snd_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card = &biscuit_card;
	struct device_node *platform, *codec, *fe_cpu, *be_cpu, *mic;
	struct device_node *mic_codec[BISCUIT_N_MICS];
	struct snd_soc_dai_link *link;
	int i, j;

	card->dev = &pdev->dev;

	/*
	 * The amplifier gain-select pins of the vendor device tree are unused
	 * on this board; GPIO 29 is the LED controller's enable line.
	 */

	platform = of_parse_phandle(pdev->dev.of_node, "mediatek,platform", 0);
	if (!platform)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "missing mediatek,platform\n");

	codec = of_parse_phandle(pdev->dev.of_node, "mediatek,audio-codec", 0);
	if (!codec)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "need mediatek,audio-codec\n");

	/*
	 * Bind the AFE DAIs by name: DL1 is the playback front end and I2S1
	 * the playback back end (I2S0/I2S2 are capture-only on this SoC).
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
