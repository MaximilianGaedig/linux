// SPDX-License-Identifier: GPL-2.0-only
/*
 * Mic-array capture for the Amazon Echo Dot 2nd Gen (biscuit).
 *
 * The four aic3x mic codecs are NOT wired to the SoC's audio front end.
 * Their I2S output feeds a small FPGA ("dough" in Amazon's own source) that
 * mixes/packages all channels and streams finished frames back to the SoC
 * over SPI0 instead. This driver ports that transport: reset the FPGA, load
 * its bitstream over SPI, then poll it for audio frames on a real-time
 * thread and hand them to ALSA as a capture-only PCM device.
 *
 * Ported from Amazon's downstream amzn-spi-pcm driver (GPL-2.0,
 * "Amazon Lab126 2016") to the modern snd_soc_component_driver API and
 * gpiod/clk. Frame geometry (dough_status_frame / dough_frame) and the
 * FPGA control sequence (off -> reset pulse -> load firmware -> verify
 * revision -> i2s mode) are unchanged, since they describe a fixed wire
 * protocol on real silicon, not something the OS gets to redesign.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/sched.h>
#include <linux/spi/spi.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#define FPGA_FIRMWARE_NAME	"i2s_to_spi_v34.bin"
#define FIRMWARE_MAX_BYTES	(10 * 4096)

#define SPI_SPEED_HZ		50000000
#define SPI_READ_WAIT_MIN_USEC	6000
#define SPI_READ_WAIT_MAX_USEC	7000
#define FPGA_RESET_MS		100

#define SAMPLING_RATE		16000
#define DOUGH_N_CHANNELS	9
#define DOUGH_SAMPLE_BYTES	3
#define DOUGH_FRAME_BYTES	(DOUGH_N_CHANNELS * DOUGH_SAMPLE_BYTES) /* 27 */
#define DOUGH_AUDIO_FRAME_BUF	255

#define DOUGH_FPGA_REV_MIN	30
#define DOUGH_FPGA_REV_MAX	251

#define SPI_BYTES_PER_PERIOD	(DOUGH_FRAME_BYTES * (DOUGH_AUDIO_FRAME_BUF + 1)) /* 6912 */
#define SPI_N_PERIODS		4
#define SPI_BUFFER_BYTES	(SPI_BYTES_PER_PERIOD * SPI_N_PERIODS)

enum dough_fw_cmd {
	DOUGH_FW_OFF	= 0x80,
	DOUGH_FW_I2S	= 0x81,
};

struct __packed dough_status_frame {
	u8 rsvd0[15];
	__le32 timestamp_48mhz;
	__le16 num_audio_frames;
	u8 rsvd1;
	u8 mode;
	u8 dac_inactive;
	u8 i2s_inactive;
	u8 overrun;
	u8 fpga_rev;
};

struct __packed dough_frame {
	struct dough_status_frame dsf;
	u8 daf[DOUGH_AUDIO_FRAME_BUF][DOUGH_FRAME_BYTES];
};

struct biscuit_spi_pcm {
	struct spi_device *spi;
	struct gpio_desc *reset_gpio;
	struct clk *mclk;
	struct pinctrl *pinctrl;
	struct pinctrl_state *state_idle;
	struct pinctrl_state *state_active;
	struct pinctrl_state *state_mclk;

	struct snd_pcm_substream *substream;
	struct task_struct *capture_thread;
	spinlock_t lock;
	size_t write_offset;
	size_t elapsed_since_period;

	struct dough_frame *rx_buf;
};

static bool dough_rev_ok(u8 rev)
{
	return rev >= DOUGH_FPGA_REV_MIN && rev <= DOUGH_FPGA_REV_MAX;
}

static int dough_spi_txrx(struct spi_device *spi, void *tx, void *rx, size_t len)
{
	struct spi_transfer xfer = {
		.tx_buf = tx,
		.rx_buf = rx,
		.len = len,
		.bits_per_word = 8,
		.speed_hz = SPI_SPEED_HZ,
	};

	return spi_sync_transfer(spi, &xfer, 1);
}

static int biscuit_spi_capture_thread(void *data)
{
	struct biscuit_spi_pcm *priv = data;
	struct snd_pcm_substream *ss = priv->substream;
	struct sched_param param = { .sched_priority = MAX_RT_PRIO - 2 };
	struct dough_frame *tx_df;
	unsigned long flags;

	sched_setscheduler(current, SCHED_FIFO, &param);

	tx_df = kzalloc(sizeof(*tx_df), GFP_KERNEL);
	if (!tx_df)
		return -ENOMEM;

	while (!kthread_should_stop()) {
		size_t n_bytes, copied = 0;
		int ret;

		ret = dough_spi_txrx(priv->spi, tx_df, priv->rx_buf, sizeof(*priv->rx_buf));
		if (ret < 0) {
			dev_err(&priv->spi->dev, "SPI capture xfer failed: %d\n", ret);
			usleep_range(SPI_READ_WAIT_MIN_USEC, SPI_READ_WAIT_MAX_USEC);
			continue;
		}

		if (!dough_rev_ok(priv->rx_buf->dsf.fpga_rev)) {
			usleep_range(SPI_READ_WAIT_MIN_USEC, SPI_READ_WAIT_MAX_USEC);
			continue;
		}

		n_bytes = min_t(u16, le16_to_cpu(priv->rx_buf->dsf.num_audio_frames),
				 DOUGH_AUDIO_FRAME_BUF) * DOUGH_FRAME_BYTES;

		while (n_bytes > 0) {
			size_t bytes = min_t(size_t, ss->runtime->dma_bytes - priv->write_offset,
					      n_bytes);
			void *dst = ss->runtime->dma_area + priv->write_offset;

			memcpy(dst, (u8 *)priv->rx_buf->daf + copied, bytes);

			spin_lock_irqsave(&priv->lock, flags);
			priv->write_offset = (priv->write_offset + bytes) % ss->runtime->dma_bytes;
			spin_unlock_irqrestore(&priv->lock, flags);

			n_bytes -= bytes;
			copied += bytes;
		}

		priv->elapsed_since_period += copied;
		if (priv->elapsed_since_period >= SPI_BYTES_PER_PERIOD) {
			priv->elapsed_since_period %= SPI_BYTES_PER_PERIOD;
			snd_pcm_period_elapsed(ss);
		}

		usleep_range(SPI_READ_WAIT_MIN_USEC, SPI_READ_WAIT_MAX_USEC);
	}

	kfree(tx_df);
	return 0;
}

static const unsigned int biscuit_spi_pcm_rates[] = { SAMPLING_RATE };
static const struct snd_pcm_hw_constraint_list biscuit_spi_pcm_rate_constraints = {
	.count = ARRAY_SIZE(biscuit_spi_pcm_rates),
	.list = biscuit_spi_pcm_rates,
};

static const struct snd_pcm_hardware biscuit_spi_pcm_hw = {
	.info = SNDRV_PCM_INFO_INTERLEAVED,
	.formats = SNDRV_PCM_FMTBIT_S24_3LE,
	.rates = SNDRV_PCM_RATE_16000,
	.rate_min = SAMPLING_RATE,
	.rate_max = SAMPLING_RATE,
	.channels_min = DOUGH_N_CHANNELS,
	.channels_max = DOUGH_N_CHANNELS,
	.buffer_bytes_max = SPI_BUFFER_BYTES,
	.period_bytes_max = SPI_BYTES_PER_PERIOD,
	.period_bytes_min = SPI_BYTES_PER_PERIOD,
	.periods_min = SPI_N_PERIODS,
	.periods_max = SPI_N_PERIODS,
};

static int biscuit_spi_pcm_open(struct snd_soc_component *component,
				 struct snd_pcm_substream *substream)
{
	struct biscuit_spi_pcm *priv = snd_soc_component_get_drvdata(component);
	int ret;

	if (substream->stream != SNDRV_PCM_STREAM_CAPTURE)
		return -EINVAL;

	substream->runtime->hw = biscuit_spi_pcm_hw;
	ret = snd_pcm_hw_constraint_list(substream->runtime, 0, SNDRV_PCM_HW_PARAM_RATE,
					  &biscuit_spi_pcm_rate_constraints);
	if (ret < 0)
		return ret;

	priv->substream = substream;
	priv->write_offset = 0;
	priv->elapsed_since_period = 0;
	spin_lock_init(&priv->lock);

	return 0;
}

static int biscuit_spi_pcm_trigger(struct snd_soc_component *component,
				    struct snd_pcm_substream *substream, int cmd)
{
	struct biscuit_spi_pcm *priv = snd_soc_component_get_drvdata(component);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		priv->capture_thread = kthread_run(biscuit_spi_capture_thread, priv,
						    "biscuit-spi-pcm");
		if (IS_ERR(priv->capture_thread)) {
			int ret = PTR_ERR(priv->capture_thread);

			priv->capture_thread = NULL;
			return ret;
		}
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
		if (priv->capture_thread) {
			kthread_stop(priv->capture_thread);
			priv->capture_thread = NULL;
		}
		return 0;
	default:
		return -EINVAL;
	}
}

static snd_pcm_uframes_t biscuit_spi_pcm_pointer(struct snd_soc_component *component,
						   struct snd_pcm_substream *substream)
{
	struct biscuit_spi_pcm *priv = snd_soc_component_get_drvdata(component);
	unsigned long flags;
	snd_pcm_uframes_t frames;

	spin_lock_irqsave(&priv->lock, flags);
	frames = bytes_to_frames(substream->runtime, priv->write_offset);
	spin_unlock_irqrestore(&priv->lock, flags);

	return frames;
}

static int biscuit_spi_pcm_copy(struct snd_soc_component *component,
				 struct snd_pcm_substream *substream, int channel,
				 unsigned long pos, struct iov_iter *iter, unsigned long bytes)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	void *src = runtime->dma_area + pos;

	if (copy_to_iter(src, bytes, iter) != bytes)
		return -EFAULT;

	return 0;
}

static int biscuit_spi_pcm_construct(struct snd_soc_component *component,
				      struct snd_soc_pcm_runtime *rtd)
{
	return snd_pcm_set_managed_buffer_all(rtd->pcm, SNDRV_DMA_TYPE_CONTINUOUS,
					       NULL, SPI_BUFFER_BYTES, SPI_BUFFER_BYTES);
}

static const struct snd_soc_component_driver biscuit_spi_pcm_component = {
	.name		= "biscuit-spi-pcm",
	.open		= biscuit_spi_pcm_open,
	.trigger	= biscuit_spi_pcm_trigger,
	.pointer	= biscuit_spi_pcm_pointer,
	.copy		= biscuit_spi_pcm_copy,
	.pcm_construct	= biscuit_spi_pcm_construct,
};

static struct snd_soc_dai_driver biscuit_spi_pcm_dai = {
	.name = "biscuit-spi-pcm",
	.capture = {
		.stream_name = "Mic Capture",
		.rates = SNDRV_PCM_RATE_16000,
		.formats = SNDRV_PCM_FMTBIT_S24_3LE,
		.channels_min = DOUGH_N_CHANNELS,
		.channels_max = DOUGH_N_CHANNELS,
	},
};

static int biscuit_spi_pcm_load_firmware(struct biscuit_spi_pcm *priv)
{
	struct device *dev = &priv->spi->dev;
	const struct firmware *fw;
	void *fw_buf;
	size_t bytes;
	int ret;

	ret = request_firmware(&fw, FPGA_FIRMWARE_NAME, dev);
	if (ret) {
		dev_err(dev, "couldn't request %s: %d (extract it from the stock Fire OS image and drop it in /lib/firmware)\n",
			FPGA_FIRMWARE_NAME, ret);
		return ret;
	}

	/* Extra padding beyond the bitstream flushes the FPGA's loader. */
	bytes = roundup(fw->size, 1024) + 1024;
	if (bytes > FIRMWARE_MAX_BYTES) {
		dev_err(dev, "FPGA firmware too large (%zu bytes)\n", bytes);
		release_firmware(fw);
		return -EFBIG;
	}

	fw_buf = kzalloc(bytes, GFP_KERNEL);
	if (!fw_buf) {
		release_firmware(fw);
		return -ENOMEM;
	}

	memcpy(fw_buf, fw->data, fw->size);
	release_firmware(fw);

	ret = dough_spi_txrx(priv->spi, fw_buf, NULL, bytes);
	kfree(fw_buf);
	if (ret < 0)
		dev_err(dev, "FPGA firmware SPI load failed: %d\n", ret);

	return ret;
}

static int biscuit_spi_pcm_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct biscuit_spi_pcm *priv;
	struct dough_frame *tx_df, *rx_df;
	u8 off_cmd[32] = { DOUGH_FW_OFF };
	u8 i2s_cmd[32] = { DOUGH_FW_I2S };
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->spi = spi;
	spi_set_drvdata(spi, priv);

	priv->rx_buf = devm_kzalloc(dev, sizeof(*priv->rx_buf), GFP_KERNEL);
	tx_df = devm_kzalloc(dev, sizeof(*tx_df), GFP_KERNEL);
	rx_df = devm_kzalloc(dev, sizeof(*rx_df), GFP_KERNEL);
	if (!priv->rx_buf || !tx_df || !rx_df)
		return -ENOMEM;

	/*
	 * GPIOD_OUT_HIGH here means "assert" (active), which - since
	 * reset-gpios is ACTIVE_LOW in DT - drives the line physically low
	 * immediately on request, holding the FPGA in reset from the start.
	 * Matches Amazon's stock driver, which requests this gpio with
	 * GPIOF_INIT_LOW (also physical low) rather than leaving it
	 * deasserted until the explicit reset pulse below.
	 */
	priv->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(priv->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(priv->reset_gpio),
				      "failed to get reset gpio\n");

	priv->pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(priv->pinctrl))
		return dev_err_probe(dev, PTR_ERR(priv->pinctrl), "failed to get pinctrl\n");

	priv->state_idle = pinctrl_lookup_state(priv->pinctrl, "idle");
	priv->state_active = pinctrl_lookup_state(priv->pinctrl, "active");
	priv->state_mclk = pinctrl_lookup_state(priv->pinctrl, "mclk");
	if (IS_ERR(priv->state_idle) || IS_ERR(priv->state_active) || IS_ERR(priv->state_mclk))
		return dev_err_probe(dev, -EINVAL, "missing idle/active/mclk pinctrl state\n");

	priv->mclk = devm_clk_get(dev, "mclk");
	if (IS_ERR(priv->mclk))
		return dev_err_probe(dev, PTR_ERR(priv->mclk), "failed to get mclk\n");

	spi->mode = SPI_MODE_3;
	spi->bits_per_word = 8;
	spi->max_speed_hz = SPI_SPEED_HZ;
	ret = spi_setup(spi);
	if (ret < 0)
		return dev_err_probe(dev, ret, "spi_setup failed\n");

	/* I2S1 idle (GPIO, not muxed to the I2S block) while the FPGA resets/loads. */
	ret = pinctrl_select_state(priv->pinctrl, priv->state_idle);
	if (ret)
		return dev_err_probe(dev, ret, "failed to select idle pinctrl state\n");

	ret = dough_spi_txrx(spi, off_cmd, NULL, sizeof(off_cmd));
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to send FPGA off command\n");

	/* Reset pulse: assert (physical low, since reset-gpios is ACTIVE_LOW), then release. */
	gpiod_set_value_cansleep(priv->reset_gpio, 1);
	msleep(FPGA_RESET_MS);
	gpiod_set_value_cansleep(priv->reset_gpio, 0);
	msleep(FPGA_RESET_MS);

	ret = biscuit_spi_pcm_load_firmware(priv);
	if (ret < 0)
		return ret;

	msleep(FPGA_RESET_MS);

	ret = dough_spi_txrx(spi, tx_df, rx_df, sizeof(*rx_df));
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to read FPGA revision\n");

	print_hex_dump(KERN_INFO, "biscuit-spi-pcm: rev-read raw: ", DUMP_PREFIX_NONE,
		       16, 1, rx_df, 32, false);

	if (!dough_rev_ok(rx_df->dsf.fpga_rev))
		return dev_err_probe(dev, -EINVAL, "unrecognized FPGA revision %u\n",
				      rx_df->dsf.fpga_rev);

	dev_info(dev, "FPGA revision %u\n", rx_df->dsf.fpga_rev);

	ret = clk_prepare_enable(priv->mclk);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable mclk\n");

	ret = pinctrl_select_state(priv->pinctrl, priv->state_mclk);
	if (ret) {
		clk_disable_unprepare(priv->mclk);
		return dev_err_probe(dev, ret, "failed to select mclk pinctrl state\n");
	}

	/* Restore I2S1 to its FPGA-facing function now the codecs can drive it. */
	ret = pinctrl_select_state(priv->pinctrl, priv->state_active);
	if (ret) {
		clk_disable_unprepare(priv->mclk);
		return dev_err_probe(dev, ret, "failed to select active pinctrl state\n");
	}

	ret = dough_spi_txrx(spi, i2s_cmd, NULL, sizeof(i2s_cmd));
	if (ret < 0) {
		clk_disable_unprepare(priv->mclk);
		return dev_err_probe(dev, ret, "failed to put FPGA in I2S mode\n");
	}

	ret = devm_snd_soc_register_component(dev, &biscuit_spi_pcm_component,
					       &biscuit_spi_pcm_dai, 1);
	if (ret) {
		clk_disable_unprepare(priv->mclk);
		return dev_err_probe(dev, ret, "failed to register ASoC component\n");
	}

	return 0;
}

static void biscuit_spi_pcm_remove(struct spi_device *spi)
{
	struct biscuit_spi_pcm *priv = spi_get_drvdata(spi);

	clk_disable_unprepare(priv->mclk);
}

static const struct of_device_id biscuit_spi_pcm_of_match[] = {
	{ .compatible = "amazon,biscuit-spi-pcm" },
	{ }
};
MODULE_DEVICE_TABLE(of, biscuit_spi_pcm_of_match);

static struct spi_driver biscuit_spi_pcm_driver = {
	.driver = {
		.name = "biscuit-spi-pcm",
		.of_match_table = biscuit_spi_pcm_of_match,
	},
	.probe = biscuit_spi_pcm_probe,
	.remove = biscuit_spi_pcm_remove,
};
module_spi_driver(biscuit_spi_pcm_driver);

MODULE_FIRMWARE(FPGA_FIRMWARE_NAME);
MODULE_DESCRIPTION("Amazon Echo Dot 2 (biscuit) mic-array FPGA SPI PCM driver");
MODULE_LICENSE("GPL");
