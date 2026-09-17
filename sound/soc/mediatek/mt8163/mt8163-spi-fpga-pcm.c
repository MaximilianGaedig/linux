// SPDX-License-Identifier: GPL-2.0-only
/*
 * Mic-array capture for the Amazon Echo Dot 2nd Gen (biscuit).
 *
 * The four TLV320ADC3101 mic ADCs are not wired to the SoC's audio front end.
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

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/io.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pm_runtime.h>
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

/*
 * iCE40 configuration timings, from the same source as the sequence in
 * probe(): >200ns with CRESET_B and SS_B both low, then >=1200us for the
 * FPGA to clear its configuration memory before it will accept a bitstream.
 */
#define FPGA_CRESET_DELAY_US		1
#define FPGA_HOUSEKEEPING_DELAY_US	1200

/*
 * FPGA and mic ADC master clock (the CMMCLK pad).
 *
 * The pad is driven by the timing generator (TG1) of the camera sensor
 * interface (SENINF), ported from Amazon's mt_amzn_mclk.c, whose register
 * and field names are used below. SENINF lives in the ISP register space:
 * the vendor "mediatek,mt8163-ispsys" node has reg[0] = 0x15004000 and the
 * SENINF registers start at +0x4000 from it. mt8163.dtsi describes no ISP
 * or SENINF node to take this from, so the address is fixed here.
 *
 * TG1 is clocked from CAMTG_SEL, which the driver sets to UNIVPLL_D26
 * (48MHz). TG1 divides that by CLKCNT + 1, so 48MHz / 9.6MHz = 5 gives
 * CLKCNT = 4. Within each period the output rises at count CLKRS = 0 and
 * falls at count CLKFL = (CLKCNT + 1) / 2 = 2. For an odd division ratio
 * (even CLKCNT) the vendor driver also sets CLKFL_POL, which by its name
 * takes the falling edge on the opposite source-clock edge, i.e. half a
 * source period later, for a duty cycle of 2.5/5 = 50%. TGCLK_SEL = 1
 * selects CAMTG as the TG1 clock, PADCLK_INV = CLK_POL = 0 leave the pad
 * clock uninverted, MCLK_EN gates the output onto the pad and PCEN enables
 * the timing generator.
 */
#define SENINF_PHYS_BASE		0x15008000
#define SENINF_MAP_SIZE			0x210

#define SENINF_TOP_CTRL			0x000
#define   SENINF12_PCLK_MASK		0xc00
#define   SENINF12_PCLK_VAL		0x300
#define SENINF1_CTRL			0x100
#define   SENINF1_EN			BIT(0)
#define SENINF1_MUX_CTRL		0x120
#define   SENINF1_MUX_EN		BIT(31)
#define SENINF_TG1_PH_CNT		0x200
#define   SENINF_TG1_TGCLK_SEL		GENMASK(1, 0)
#define   SENINF_TG1_TGCLK_SEL_CAMTG	1
#define   SENINF_TG1_CLKFL_POL		BIT(2)
#define   SENINF_TG1_PADCLK_INV		BIT(6)
#define   SENINF_TG1_CLK_POL		BIT(28)
#define   SENINF_TG1_MCLK_EN		BIT(29)
#define   SENINF_TG1_PCEN		BIT(31)
#define SENINF_TG1_SEN_CK		0x204
#define   SENINF_TG1_CLKFL		GENMASK(5, 0)
#define   SENINF_TG1_CLKRS		GENMASK(13, 8)
#define   SENINF_TG1_CLKCNT		GENMASK(21, 16)

#define FPGA_MCLK_SRC_HZ		48000000	/* CAMTG_SEL = UNIVPLL_D26 */
#define FPGA_MCLK_OUT_HZ		9600000		/* CMMCLK pad rate */

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
	/* Test-pattern generator: frames are synthesised without I2S input. */
	DOUGH_FW_TPG	= 0x83,
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
	struct clk *sen_tg;
	struct clk *sen_cam;
	struct clk *larb2_smi;
	struct clk *cam_smi;
	void __iomem *seninf;
	struct pinctrl *pinctrl;
	struct pinctrl_state *state_idle;
	struct pinctrl_state *state_active;
	struct pinctrl_state *state_mclk;

	struct snd_pcm_substream *substream;
	struct task_struct *capture_thread;
	bool running;
	/* have we armed the FPGA while its I2S input was actually clocking? */
	bool armed_live;
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

/* Clear @clr, then set @set, in a SENINF register (vendor isp_wr32_mask). */
static void seninf_clrset(void __iomem *base, u32 off, u32 clr, u32 set)
{
	u32 t = readl(base + off);

	t &= ~clr;
	t |= set;
	writel(t, base + off);
}

/*
 * Program the SENINF timing generator to emit FPGA_MCLK_OUT_HZ on the CMMCLK
 * pad (vendor mclk_divider_reg() + mclk_enable_reg()). Requires the ISP power
 * domain on and the imgsys clocks enabled (register space is otherwise
 * inaccessible), and CAMTG_SEL at FPGA_MCLK_SRC_HZ.
 */
static void biscuit_fpga_seninf_program(struct biscuit_spi_pcm *priv)
{
	void __iomem *b = priv->seninf;
	u32 clkcnt, clkf_pol, clkf_edge;

	clkcnt = (FPGA_MCLK_SRC_HZ / FPGA_MCLK_OUT_HZ) - 1;
	clkf_pol = !(clkcnt & 0x1);
	clkf_edge = clkcnt > 1 ? ((clkcnt + 1) >> 1) : 1;

	/*
	 * Program the divider before enabling the timing generator: a divider
	 * written while it is running only takes effect once TG1_PH_CNT is
	 * written again.
	 */
	seninf_clrset(b, SENINF_TOP_CTRL, SENINF12_PCLK_MASK, SENINF12_PCLK_VAL);

	seninf_clrset(b, SENINF_TG1_SEN_CK, SENINF_TG1_CLKFL,
		      FIELD_PREP(SENINF_TG1_CLKFL, clkf_edge));
	seninf_clrset(b, SENINF_TG1_SEN_CK, SENINF_TG1_CLKRS,
		      FIELD_PREP(SENINF_TG1_CLKRS, 0));
	seninf_clrset(b, SENINF_TG1_SEN_CK, SENINF_TG1_CLKCNT,
		      FIELD_PREP(SENINF_TG1_CLKCNT, clkcnt));

	seninf_clrset(b, SENINF_TG1_PH_CNT, SENINF_TG1_TGCLK_SEL,
		      FIELD_PREP(SENINF_TG1_TGCLK_SEL, SENINF_TG1_TGCLK_SEL_CAMTG));
	seninf_clrset(b, SENINF_TG1_PH_CNT, SENINF_TG1_CLKFL_POL,
		      clkf_pol ? SENINF_TG1_CLKFL_POL : 0);
	seninf_clrset(b, SENINF_TG1_PH_CNT, SENINF_TG1_PADCLK_INV, 0);
	seninf_clrset(b, SENINF_TG1_PH_CNT, SENINF_TG1_CLK_POL, 0);

	/* Gate the clock onto the pad and enable PCEN last. */
	seninf_clrset(b, SENINF_TG1_PH_CNT, 0,
		      SENINF_TG1_MCLK_EN | SENINF_TG1_PCEN);

	seninf_clrset(b, SENINF1_MUX_CTRL, SENINF1_MUX_EN, SENINF1_MUX_EN);
	seninf_clrset(b, SENINF1_CTRL, SENINF1_EN, SENINF1_EN);
}

static int biscuit_spi_capture_thread(void *data)
{
	struct biscuit_spi_pcm *priv = data;
	struct snd_pcm_substream *ss = priv->substream;
	struct dough_frame *tx_df;
	unsigned long flags;

	sched_set_fifo(current);

	tx_df = kzalloc(sizeof(*tx_df), GFP_KERNEL);
	if (!tx_df)
		return -ENOMEM;

	while (!kthread_should_stop()) {
		size_t n_bytes, copied = 0;
		int ret;

		/*
		 * Idle until trigger sets priv->running. The thread lives from
		 * open to close, so trigger (atomic context) never has to create
		 * or stop it.
		 */
		if (!READ_ONCE(priv->running)) {
			set_current_state(TASK_INTERRUPTIBLE);
			if (!READ_ONCE(priv->running) && !kthread_should_stop())
				schedule_timeout(msecs_to_jiffies(20));
			__set_current_state(TASK_RUNNING);
			continue;
		}

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

		/*
		 * (Re-)send the I2S mode command once the status frame reports
		 * the I2S input active. The FPGA only starts framing if the
		 * command arrives while its I2S input is clocked, which is not
		 * yet the case at probe or open time.
		 */
		if (!priv->armed_live && !priv->rx_buf->dsf.i2s_inactive) {
			u8 cmd[32] = { DOUGH_FW_I2S };

			priv->armed_live = true;
			dough_spi_txrx(priv->spi, cmd, NULL, sizeof(cmd));
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
	priv->running = false;
	priv->armed_live = false;
	spin_lock_init(&priv->lock);

	/*
	 * Start the SPI reader here rather than in trigger(), which runs in
	 * atomic context where kthread_run() cannot be called. The thread idles
	 * until trigger sets priv->running.
	 */
	/*
	 * Re-send the I2S mode command at stream start. The command sent at
	 * probe time arrives before the FPGA's I2S inputs are clocked.
	 */
	{
		u8 i2s_cmd[32] = { DOUGH_FW_I2S };

		dough_spi_txrx(priv->spi, i2s_cmd, NULL, sizeof(i2s_cmd));
	}

	priv->capture_thread = kthread_run(biscuit_spi_capture_thread, priv,
					   "biscuit-spi-pcm");
	if (IS_ERR(priv->capture_thread)) {
		int ret2 = PTR_ERR(priv->capture_thread);

		priv->capture_thread = NULL;
		return ret2;
	}

	return 0;
}

static int biscuit_spi_pcm_close(struct snd_soc_component *component,
				 struct snd_pcm_substream *substream)
{
	struct biscuit_spi_pcm *priv = snd_soc_component_get_drvdata(component);

	WRITE_ONCE(priv->running, false);
	if (priv->capture_thread) {
		kthread_stop(priv->capture_thread);
		priv->capture_thread = NULL;
	}
	priv->substream = NULL;

	return 0;
}

static int biscuit_spi_pcm_trigger(struct snd_soc_component *component,
				    struct snd_pcm_substream *substream, int cmd)
{
	struct biscuit_spi_pcm *priv = snd_soc_component_get_drvdata(component);

	/* Atomic context: flip a flag and wake the reader, nothing more. */
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		if (!priv->capture_thread)
			return -EIO;
		WRITE_ONCE(priv->running, true);
		wake_up_process(priv->capture_thread);
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		WRITE_ONCE(priv->running, false);
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

static int biscuit_spi_pcm_new(struct snd_soc_component *component,
			       struct snd_soc_pcm_runtime *rtd)
{
	return snd_pcm_set_managed_buffer_all(rtd->pcm, SNDRV_DMA_TYPE_CONTINUOUS,
					       NULL, SPI_BUFFER_BYTES, SPI_BUFFER_BYTES);
}

static const struct snd_soc_component_driver biscuit_spi_pcm_component = {
	.name		= "biscuit-spi-pcm",
	.open		= biscuit_spi_pcm_open,
	.close		= biscuit_spi_pcm_close,
	.trigger	= biscuit_spi_pcm_trigger,
	.pointer	= biscuit_spi_pcm_pointer,
	.copy		= biscuit_spi_pcm_copy,
	.pcm_new	= biscuit_spi_pcm_new,
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
	priv->sen_tg = devm_clk_get(dev, "sen_tg");
	if (IS_ERR(priv->sen_tg))
		return dev_err_probe(dev, PTR_ERR(priv->sen_tg), "failed to get sen_tg\n");
	priv->sen_cam = devm_clk_get(dev, "sen_cam");
	if (IS_ERR(priv->sen_cam))
		return dev_err_probe(dev, PTR_ERR(priv->sen_cam), "failed to get sen_cam\n");
	priv->larb2_smi = devm_clk_get(dev, "larb2_smi");
	if (IS_ERR(priv->larb2_smi))
		return dev_err_probe(dev, PTR_ERR(priv->larb2_smi), "failed to get larb2_smi\n");
	priv->cam_smi = devm_clk_get(dev, "cam_smi");
	if (IS_ERR(priv->cam_smi))
		return dev_err_probe(dev, PTR_ERR(priv->cam_smi), "failed to get cam_smi\n");

	priv->seninf = devm_ioremap(dev, SENINF_PHYS_BASE, SENINF_MAP_SIZE);
	if (!priv->seninf)
		return dev_err_probe(dev, -ENOMEM, "failed to map SENINF\n");

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
	if (ret < 0) {
		return dev_err_probe(dev, ret, "failed to send FPGA off command\n");
	}

	/*
	 * Put the iCE40 into slave-SPI configuration mode.
	 *
	 * The configuration mode is sampled from SPI_SS_B when CRESET_B is
	 * released: SS_B low selects slave SPI (host sends the bitstream), SS_B
	 * high selects master SPI (boot from external flash, which this board
	 * does not have). The SPI core only asserts SS_B around a transfer, so
	 * follow drivers/fpga/ice40-spi.c: lock the bus, drop CRESET_B, run a
	 * zero-length transfer with cs_change to keep SS_B asserted, release
	 * CRESET_B, then a second transfer whose completion releases SS_B once
	 * the FPGA has cleared its configuration memory.
	 */
	{
		struct spi_message msg;
		struct spi_transfer assert_cs_then_reset_delay = {
			.cs_change = 1,
			.delay = {
				.value = FPGA_CRESET_DELAY_US,
				.unit = SPI_DELAY_UNIT_USECS,
			},
		};
		struct spi_transfer housekeeping_delay_then_release_cs = {
			.delay = {
				.value = FPGA_HOUSEKEEPING_DELAY_US,
				.unit = SPI_DELAY_UNIT_USECS,
			},
		};
		int cret;

		spi_bus_lock(spi->controller);

		/* CRESET_B low (reset-gpios is ACTIVE_LOW, so logical 1). */
		gpiod_set_value_cansleep(priv->reset_gpio, 1);

		spi_message_init(&msg);
		spi_message_add_tail(&assert_cs_then_reset_delay, &msg);
		cret = spi_sync_locked(spi, &msg);

		/* Release CRESET_B with SS_B asserted: selects slave SPI. */
		gpiod_set_value_cansleep(priv->reset_gpio, 0);

		if (!cret) {
			spi_message_init(&msg);
			spi_message_add_tail(&housekeeping_delay_then_release_cs,
					     &msg);
			cret = spi_sync_locked(spi, &msg);
		}

		spi_bus_unlock(spi->controller);
	}
	msleep(FPGA_RESET_MS);

	/*
	 * A missing bitstream is not fatal: the FPGA only carries mic-array
	 * capture, while speaker playback is independent of it. Register the
	 * component anyway so the sound card still forms.
	 */
	ret = biscuit_spi_pcm_load_firmware(priv);
	if (ret < 0) {
		dev_warn(dev,
			 "FPGA firmware unavailable (%d); registering component anyway - mic-array capture is non-functional until /lib/firmware/%s is present, speaker playback is unaffected\n",
			 ret, FPGA_FIRMWARE_NAME);
		goto register_component;
	}

	msleep(FPGA_RESET_MS);

	/*
	 * Read one status frame to verify the loaded design. The remaining FPGA
	 * bring-up is best-effort for the same reason as above: a failure only
	 * costs mic-array capture.
	 */
	ret = dough_spi_txrx(spi, tx_df, rx_df, sizeof(*rx_df));
	if (ret < 0) {
		dev_warn(dev, "FPGA revision read failed (%d); mic capture non-functional\n", ret);
		goto register_component;
	}

	if (!dough_rev_ok(rx_df->dsf.fpga_rev)) {
		dev_warn(dev, "unrecognized FPGA revision %u (mic capture non-functional); speaker unaffected\n",
			 rx_df->dsf.fpga_rev);
		goto register_component;
	}

	dev_info(dev, "FPGA revision %u\n", rx_df->dsf.fpga_rev);

	/*
	 * The reference clock is only needed for the I2S audio path, not for SPI
	 * access, so bring it up after the revision has been verified, in the
	 * same order as the vendor driver.
	 */
	/*
	 * Power on the ISP power domain so the imgsys/SENINF register space is
	 * accessible (power-domains = ISP in DT; the SPI core attached genpd).
	 */
	pm_runtime_enable(dev);
	ret = pm_runtime_get_sync(dev);
	if (ret < 0) {
		pm_runtime_disable(dev);
		return dev_err_probe(dev, ret, "failed to power on ISP domain\n");
	}

	/*
	 * The CMMCLK pad is driven by the camera SENINF timing generator: enable
	 * the imgsys sensor clocks, set CAMTG_SEL to 48MHz (UNIVPLL_D26), then
	 * program the SENINF divider to emit 9.6MHz.
	 */
	ret = clk_prepare_enable(priv->sen_tg);
	if (ret)
		dev_warn(dev, "enable sen_tg failed: %d\n", ret);
	ret = clk_prepare_enable(priv->sen_cam);
	if (ret)
		dev_warn(dev, "enable sen_cam failed: %d\n", ret);
	ret = clk_prepare_enable(priv->larb2_smi);
	if (ret)
		dev_warn(dev, "enable larb2_smi failed: %d\n", ret);
	ret = clk_prepare_enable(priv->cam_smi);
	if (ret)
		dev_warn(dev, "enable cam_smi failed: %d\n", ret);

	ret = clk_set_rate(priv->mclk, FPGA_MCLK_SRC_HZ);
	if (ret)
		dev_warn(dev, "failed to set mclk to 48MHz: %d\n", ret);
	ret = clk_prepare_enable(priv->mclk);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable mclk\n");

	ret = pinctrl_select_state(priv->pinctrl, priv->state_mclk);
	if (ret) {
		clk_disable_unprepare(priv->mclk);
		return dev_err_probe(dev, ret, "failed to select mclk pinctrl state\n");
	}

	/* Route/divide the 48 MHz reference to 9.6 MHz out the CMMCLK pad. */
	biscuit_fpga_seninf_program(priv);

	/* Restore I2S1 to its FPGA-facing function now the codecs can drive it. */
	ret = pinctrl_select_state(priv->pinctrl, priv->state_active);
	if (ret) {
		dev_warn(dev, "failed to select active pinctrl state (%d); mic capture non-functional\n", ret);
		goto register_component;
	}

	ret = dough_spi_txrx(spi, i2s_cmd, NULL, sizeof(i2s_cmd));
	if (ret < 0)
		dev_warn(dev, "failed to put FPGA in I2S mode (%d); mic capture non-functional\n", ret);

register_component:
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

static const struct spi_device_id biscuit_spi_pcm_id[] = {
	{ "biscuit-spi-pcm" },
	{ }
};
MODULE_DEVICE_TABLE(spi, biscuit_spi_pcm_id);

static struct spi_driver biscuit_spi_pcm_driver = {
	.driver = {
		.name = "biscuit-spi-pcm",
		.of_match_table = biscuit_spi_pcm_of_match,
	},
	.id_table = biscuit_spi_pcm_id,
	.probe = biscuit_spi_pcm_probe,
	.remove = biscuit_spi_pcm_remove,
};
module_spi_driver(biscuit_spi_pcm_driver);

MODULE_FIRMWARE(FPGA_FIRMWARE_NAME);
MODULE_DESCRIPTION("Amazon Echo Dot 2 (biscuit) mic-array FPGA SPI PCM driver");
MODULE_LICENSE("GPL");
