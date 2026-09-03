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

static struct snd_soc_jack biscuit_hp_jack;
static struct snd_soc_jack_gpio biscuit_hp_gpio = {
	.name = "hp-det",
	.report = SND_JACK_LINEOUT,
	.debounce_time = 200,
	/*
	 * ACTIVE_LOW in DT means "inserted = pin low"; invert so the ASoC
	 * jack reports LINEOUT present when the plug is in.
	 */
	.invert = 1,
};

static const struct snd_soc_dapm_widget biscuit_widgets[] = {
	SND_SOC_DAPM_SPK("Ext Spk", NULL),
	SND_SOC_DAPM_MIC("Mic Array", NULL),
	/*
	 * The external amplifier's enable line (GPIO 122), as a supply widget
	 * so DAPM turns it on with the speaker path and off again after.
	 * Holding it on continuously makes the speaker hiss; stock enables it
	 * only while a speaker stream is open and refcounts it.
	 */
	SND_SOC_DAPM_REGULATOR_SUPPLY("ext-spk-amp", 0, 0),
	/*
	 * Second amplifier enable, GPIO 124 - the pin the shipped OTA device
	 * tree actually uses for extamp (see ext_spk_amp2 in the DTS). Driven
	 * the same DAPM-gated way as ext-spk-amp so whichever pin really gates
	 * the amp on this board is asserted during playback.
	 */
	SND_SOC_DAPM_REGULATOR_SUPPLY("ext-spk-amp2", 0, 0),
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
 * to close one; declaring every input on each part lets DAPM complete a
 * path for whichever one this board actually wires without needing the
 * schematic first.
 *
 * The differential inputs are separate widgets from the single-ended ones -
 * "DIF_1L_1R Capture Switch" is fed by DIFL_1L_1R and DIFR_1L_1R, not by
 * IN_1L/IN_1R - so listing only IN_* left the differential pins with no
 * source. Amazon's audio_init.sh selects exactly the differential pair
 * (DIF1), which meant the one input this board actually uses was the one
 * input DAPM could not power: the switch closed, the path still dead-ended,
 * and ADC_DIGITAL stayed 0x00 with both ADCs off, no PLL, and no I2S clock
 * for the FPGA to count frames from.
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
	 * The speaker hangs off the HEADPHONE outputs, not the line outputs.
	 *
	 * Amazon's own /etc/audio_init.sh closes exactly two playback
	 * switches - "HPL Output Mixer L_DAC Switch" and "HPR Output Mixer
	 * R_DAC Switch" - and nothing on the LO path, so HPL/HPR is what
	 * reaches the amplifier on this board. Without a route from them to
	 * an endpoint the HP drivers can never power up (OUTPWRCTL keeps its
	 * HP bits clear and the widgets read "Off in 1 out 0"), which is why
	 * driving the LO path produced a perfectly configured, completely
	 * silent codec.
	 *
	 * The LO routes stay as well: they cost nothing, and keeping both
	 * described means whichever pair is populated can be driven.
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

/*
 * Which ADC drives the bus. Amazon always uses the first, and so do we; it is
 * a module parameter only because it separates two otherwise identical
 * symptoms - a broken slave path from a dead analogue front end on a
 * particular part. Whichever ADC is master is the one that has been observed
 * to convert.
 */
static int biscuit_mic_master;
module_param_named(mic_master, biscuit_mic_master, int, 0644);
MODULE_PARM_DESC(mic_master, "index of the mic ADC that drives BCLK/WCLK");

/* 0x03, 0x0c, 0x30, 0x40: two channels each except the last, seven in all. */
#define BISCUIT_MIC_RX_MASK(i)	((i) == BISCUIT_N_MICS - 1 ? \
				 0x1 << (2 * (i)) : 0x3 << (2 * (i)))

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
				   (i == biscuit_mic_master
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
		/*
		 * Seven microphones across four stereo parts, so the last one
		 * supplies a single channel. Amazon expresses this as one
		 * global rx_mask of 0x7f handed to every codec, each of which
		 * knows its own index; mainline's driver has no such index, so
		 * the same thing is said per-codec here - 0x03, 0x0c, 0x30,
		 * 0x40 - and the driver derives both its slot offset and which
		 * of its own two channels to leave alone from that.
		 *
		 * The eighth channel matters: the FPGA puts its DAC loopback
		 * reference there, so an ADC that keeps driving it collides
		 * with the reference.
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
	 * The ADC input routing and PGA gain are deliberately NOT set here.
	 *
	 * They are DAPM controls, and calling a DAPM control's .put from
	 * inside hw_params deadlocks: the put ends up in
	 * snd_soc_dpcm_runtime_update(), which wants the card mutex that this
	 * callback is already holding. The process wedges in D state and the
	 * whole card follows, because every later snd_pcm_open() blocks behind
	 * it. Amazon sets these from userspace instead, in audio_init.sh, once
	 * at boot and outside any stream - see the equivalent script in the
	 * initrd, which is where this belongs.
	 */
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
/*
 * Close every mixer the playback chain needs, once.
 *
 * These cannot be set from the card's late_probe: a CODEC's DAPM mixer
 * controls are only created later, in snd_soc_dapm_new_widgets(), so the
 * lookups there fail with "no control" for exactly the ones that matter.
 * By the time a stream configures its back end they all exist.
 *
 * All of them default to off, and each one silently breaks the chain:
 *  - the AFE's DL1 -> I2S1 routing mixers: without them there is no DAPM
 *    path from the front end to the back end at all, so dpcm_path_get()
 *    finds no back end and opening the PCM fails with a bare -EINVAL and
 *    no kernel message whatsoever.
 *  - the CODEC's DAC -> output-mixer switches: with these open the DAC has
 *    no outgoing path, DAPM leaves the chain powered down, and because the
 *    I2S port is only enabled as part of that power-up AFE_I2S_CON1 keeps
 *    its enable bit clear (0xa08 instead of 0xa09) - a perfectly
 *    configured codec that never receives a single clock edge.
 *
 * The speaker is on the HEADPHONE outputs: Amazon's own /etc/audio_init.sh
 * closes "HPL/HPR Output Mixer L_DAC/R_DAC Switch" and nothing on the LO
 * path. The LO switches are closed too, harmlessly, so either wiring works.
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
		dev_info(card->dev, "biscuit: closed '%s'\n", mixers[i]);
	}
	snd_soc_dapm_sync(card->dapm);
}

static int biscuit_be_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);
	/*
	 * 9.6MHz, not rate*256: the codec's clock is the CMMCLK pad shared
	 * with the mic ADCs, which the SENINF divider drives at a fixed
	 * 9.6MHz. Amazon passes the same constant (AIC31XX_FREQ_9600000) to
	 * this codec and to the ADCs.
	 */
	/*
	 * 9.6MHz - the rate stock actually uses (AIC31XX_FREQ_9600000), from
	 * the CMMCLK pad shared with the mic ADCs. An earlier -EINVAL here was
	 * misattributed to the codec being unable to use 9.6MHz; it came from
	 * asking the AFE to *generate* that rate, which it cannot because
	 * mtk_dai_i2s_set_sysclk() requires an exact APLL divisor. That call
	 * is gone, and the codec's own PLL handles 9.6MHz fine.
	 */
	unsigned int mclk = 9600000;
	int ret;

	/*
	 * Deliberately no set_sysclk on the AFE side.
	 *
	 * The codec's master clock does not come from the AFE at all - it is
	 * the CMMCLK pad, driven by the camera SENINF divider (the same clock
	 * the mic ADCs and the FPGA use). Asking the AFE for it fails anyway:
	 * mtk_dai_i2s_set_sysclk() rejects any rate that does not divide the
	 * APLL exactly, so 12MHz returns -EINVAL and takes hw_params, and
	 * therefore the whole stream, down with it. The AFE still supplies
	 * BCK and LRCK as the I2S clock provider; only MCLK is external.
	 */

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

	/*
	 * Unmute the external amplifier.
	 *
	 * Its mute line is not a SoC GPIO - it hangs off the CODEC's MFP2 pin,
	 * driven through AIC32X4_DOUTCTL (register 53). Amazon's own aic32x4
	 * driver writes MFP2_GPIO_ENABLE|MFP2_GPIO_HI (0x05) at probe to mute
	 * the amp, and MFP2_GPIO_ENABLE alone (0x04) to unmute it. Mainline's
	 * driver never touches that register at all, so nothing ever unmutes
	 * the amplifier and the board stays silent no matter how correct the
	 * digital path is.
	 */
	{
		struct snd_soc_component *cmp = snd_soc_rtd_to_codec(rtd, 0)->component;
		int wret = snd_soc_component_write(cmp, 53, 0x04);

		dev_info(rtd->dev, "biscuit: amp unmute via MFP2 (reg53=0x04) -> %d\n",
			 wret);
	}

	return 0;
}

/*
 * Dump the AFE's I2S1 control registers once the port should be live.
 *
 * hw_params is too early - DAPM only enables the port at trigger - so a
 * zero here is meaningful where a zero in hw_params is not. If playback is
 * inaudible while AFE_I2S_CON1 reads 0, no clock or data is leaving the SoC
 * and the codec is not the problem.
 */
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

/*
 * Force the mic-array capture chain on once the card exists.
 *
 * The register readback proved the four ADCs get correctly configured
 * (mic0 becomes TDM master, the PLL divider is written) but their PLL and
 * ADC-digital blocks never power up - power on this codec is entirely
 * DAPM-gated, and the capture stream was not pulling the AIF_OUT ->
 * ADC -> PGA -> Input chain up. The mic array is an always-listening
 * device with no user-facing routing choice, so pin every input and every
 * ADC path on explicitly and sync, which powers PLL_CLK/ADC_CLK and the
 * ADCs and gets the codec clocking the FPGA.
 */
static int biscuit_late_probe(struct snd_soc_card *card)
{
	struct snd_soc_dapm_context *dapm = card->dapm;
	/*
	 * The differential input widgets, not the single-ended IN_2 pins.
	 *
	 * Amazon selects what it calls DIF1, which is page 1 register 52 bit 7
	 * on the left and register 55 bit 7 on the right. Mainline names those
	 * same two bits after the pins they actually connect - DIF_2L_3L and
	 * DIF_2R_3R - and uses the name DIF_1L_1R for a different register
	 * entirely. Forcing IN_2L/IN_2R on instead left the differential
	 * widgets unpowered, so the switch could be closed and still carry
	 * nothing.
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
	 * Close the AFE's DL1 -> I2S1 routing mixers.
	 *
	 * MediaTek's AFE connects a memif to an I2S port through DAPM mixer
	 * controls ("I2S1_CH1 DL1_CH1"), and they default to off. With them
	 * open there is no DAPM path from the DL1 front end to the I2S1 back
	 * end at all, so dpcm_path_get() finds no back end and opening the
	 * playback device fails with a bare -EINVAL and no kernel message -
	 * which is exactly how this presented. There is only one playback
	 * route on this board, so close them here rather than making every
	 * user discover the mixer names.
	 */
	dev_info(card->dev, "biscuit: forced mic capture pins on\n");

	/*
	 * The AFE routing mixers have to be closed here, not at hw_params:
	 * dpcm_path_get() walks DAPM at open() time, so without them the very
	 * first open fails with -EINVAL and hw_params never runs at all.
	 * These controls exist by now; the CODEC's own DAPM mixers do not,
	 * which is why they are handled later.
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
			else
				dev_info(card->dev, "biscuit: line-out jack detect on\n");
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
	 * No amplifier gain-select pins are driven here.
	 *
	 * biscuit.dtsi does define aud_pins_extamp_gain0..3 on GPIO 28/29, but
	 * the &audgpio node's pinctrl-names never lists those states - not in
	 * biscuit.dtsi and not in either EVT override - so stock's
	 * pinctrl_lookup_state() fails for them, gpio_prepare stays false and
	 * AudDrv_GPIO_EXTAMP_Gain_Set() is a no-op returning -1 on this board.
	 * They are MediaTek reference-board leftovers. Worse, stock uses
	 * <&pio 29 0> as the enable-gpio of the lp55231 LED controller
	 * (biscuit.dtsi:180), so driving it from here fights the LED ring.
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
	for_each_card_prelinks(card, i, link)
		dev_info(&pdev->dev,
			 "biscuit-link[%d] %-8s cpu=%s codec=%s plat=%s dyn=%d no_pcm=%d play=%d cap=%d\n",
			 i, link->name,
			 link->cpus ? (link->cpus->dai_name ?: "?") : "-",
			 link->codecs ? (link->codecs->dai_name ?: "?") : "-",
			 link->platforms ? (link->platforms->name ?: "of") : "-",
			 link->dynamic, link->no_pcm,
			 link->playback_only, link->capture_only);

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
