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
 * FPGA reference-clock (CMMCLK pad) bring-up via the camera SENINF timing
 * generator, ported 1:1 from Amazon's stock mt_amzn_mclk.c. The SENINF block
 * lives in the ISP register space: ispsys reg[0] = 0x15004000, and the SENINF
 * registers are at ISP_ADDR + 0x4000, i.e. physical 0x15008000. Map that.
 */
#define SENINF_PHYS_BASE	0x15008000
#define SENINF_MAP_SIZE		0x210
/*
 * iCE40 configuration timings, from the same source as the sequence in
 * probe(): >200ns with CRESET_B and SS_B both low, then >=1200us for the
 * FPGA to clear its configuration memory before it will accept a bitstream.
 */
static int biscuit_fpga_tpg;
module_param(biscuit_fpga_tpg, int, 0644);
MODULE_PARM_DESC(biscuit_fpga_tpg, "put the FPGA in test-pattern mode instead of I2S capture");

#define FPGA_CRESET_DELAY_US		1
#define FPGA_HOUSEKEEPING_DELAY_US	1200

#define SENINF_TOP_CTRL		0x000
#define SENINF1_CTRL		0x100
#define SENINF1_MUX_CTRL	0x120
#define SENINF_TG1_PH_CNT	0x200
#define SENINF_TG1_SEN_CK	0x204
#define FPGA_MCLK_SRC_HZ	48000000	/* CAMTG_SEL -> UNIVPLL_D26 */
#define FPGA_MCLK_OUT_HZ	9600000	/* SENINF divider output, matches stock */
#define SENINF_TG1_MCLK_EN	0x20000000	/* mclk_enable_reg bit */

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
	/*
	 * Test-pattern generator. The FPGA synthesises frames on its own,
	 * with no I2S input needed, so this separates "the FPGA never frames"
	 * from "the microphones are not feeding it": if capture works in TPG
	 * mode then the SPI path, the frame layout and the ALSA plumbing are
	 * all correct and only the mic-side I2S is at fault.
	 */
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
	unsigned int dbg_reads;
	/* have we armed the FPGA while its I2S input was actually clocking? */
	bool armed_live;
	unsigned int dbg_ptr;
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

/* val = rd; val &= ~mask; val |= (v << shift); wr  (matches Amazon isp_wr32_mask) */
static void seninf_wr_mask(void __iomem *base, u32 off, u32 mask, u32 shift, u32 v)
{
	u32 t = readl(base + off);

	t &= ~mask;
	t |= (v << shift);
	writel(t, base + off);
}

/*
 * Program the camera SENINF timing generator to emit FPGA_MCLK_OUT_HZ on the
 * CMMCLK pad, ported 1:1 from Amazon's mt_amzn_mclk.c (mclk_enable_reg +
 * mclk_divider_reg). Requires the ISP power domain on and the imgsys clocks
 * enabled (register space is otherwise inaccessible), and CAMTG_SEL at 48 MHz.
 */
static void biscuit_fpga_seninf_program(struct biscuit_spi_pcm *priv)
{
	void __iomem *b = priv->seninf;
	u32 clkcnt, clkf_pol, clkf_edge, v;

	/* mclk_enable_reg(1): TG1_PH_CNT |= 0x20000000 */
	v = readl(b + SENINF_TG1_PH_CNT);
	v |= SENINF_TG1_MCLK_EN;
	writel(v, b + SENINF_TG1_PH_CNT);

	/*
	 * mclk_divider_reg: clkcnt = 48M/out - 1, so 12MHz gives 3.
	 *
	 * Amazon runs this pad at 9.6MHz, but this clock is shared with the
	 * two audio codecs and mainline's aic32x4 cannot use 9.6MHz at all:
	 * its PLL solver (clk_aic32x4_pll_calc_muldiv) finds no valid p/r for
	 * that input, so hw_params fails with -EINVAL at every sample rate.
	 * 12MHz is an exact divide of the same 48MHz source, is a standard
	 * MCLK the aic32x4 handles, and is already present in the ADC3101
	 * divider table for the 16kHz the mic array runs at.
	 */
	clkcnt = (FPGA_MCLK_SRC_HZ / FPGA_MCLK_OUT_HZ) - 1;
	clkf_pol = !(clkcnt & 0x1);
	clkf_edge = clkcnt > 1 ? ((clkcnt + 1) >> 1) : 1;

	/* PCEN = 1 */
	seninf_wr_mask(b, SENINF_TG1_PH_CNT, (1u << 31), 31, 1);
	/* clear top clock gating (clear 0xc00, set 0x300) */
	seninf_wr_mask(b, SENINF_TOP_CTRL, 0xc00, 0, 0x300);
	/* SEN_CK divider: CLKFL=clkf_edge, CLKRS=0, CLKCNT=clkcnt */
	seninf_wr_mask(b, SENINF_TG1_SEN_CK, 0x3f, 0, clkf_edge);
	seninf_wr_mask(b, SENINF_TG1_SEN_CK, 0x3f00, 8, 0);
	seninf_wr_mask(b, SENINF_TG1_SEN_CK, 0x3f0000, 16, clkcnt);
	/* PH_CNT: TGCLK_SEL=1, CLKFL_POL, PADCLK_INV=0, CLK_POL=0 */
	seninf_wr_mask(b, SENINF_TG1_PH_CNT, 0x3, 0, 1);
	seninf_wr_mask(b, SENINF_TG1_PH_CNT, 0x4, 2, clkf_pol);
	seninf_wr_mask(b, SENINF_TG1_PH_CNT, (1u << 6), 6, 0);
	seninf_wr_mask(b, SENINF_TG1_PH_CNT, (1u << 28), 28, 0);
	/* enable SENINF1 mux + block */
	seninf_wr_mask(b, SENINF1_MUX_CTRL, (1u << 31), 31, 1);
	seninf_wr_mask(b, SENINF1_CTRL, 0x1, 0, 1);

	dev_err(&priv->spi->dev,
		"biscuit-dbg: seninf programmed clkcnt=%u pol=%u edge=%u TG1_PH_CNT=0x%08x SEN_CK=0x%08x\n",
		clkcnt, clkf_pol, clkf_edge,
		readl(b + SENINF_TG1_PH_CNT), readl(b + SENINF_TG1_SEN_CK));
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

		/*
		 * Idle until trigger says go.  The thread now outlives a
		 * single start/stop so that trigger - which ASoC calls with
		 * the PCM group lock held and interrupts disabled - never has
		 * to create or destroy it.
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
		 * Arm the part once its I2S input is genuinely clocking.
		 *
		 * Both existing arming points are too early: probe runs before
		 * any codec is configured, and .open runs before hw_params, so
		 * in both the status frame still reports i2s_inactive=1. The
		 * part latches "no I2S" when the command arrives and never
		 * starts framing, which is how it ends up reporting frames
		 * ready while handing back an entirely zero payload. The first
		 * read where i2s_inactive clears is the earliest moment the
		 * command can mean anything, so send it there.
		 */
		if (!priv->armed_live && !priv->rx_buf->dsf.i2s_inactive) {
			u8 cmd[32] = { biscuit_fpga_tpg ? DOUGH_FW_TPG
						       : DOUGH_FW_I2S };

			priv->armed_live = true;
			dev_err(&priv->spi->dev,
				"biscuit-dbg: FPGA %s mode armed with I2S live -> %d\n",
				biscuit_fpga_tpg ? "TPG" : "I2S",
				dough_spi_txrx(priv->spi, cmd, NULL, sizeof(cmd)));
			usleep_range(SPI_READ_WAIT_MIN_USEC, SPI_READ_WAIT_MAX_USEC);
			continue;
		}

		n_bytes = min_t(u16, le16_to_cpu(priv->rx_buf->dsf.num_audio_frames),
				 DOUGH_AUDIO_FRAME_BUF) * DOUGH_FRAME_BYTES;

		/*
		 * Report what the FPGA is handing us for the first few reads of
		 * each stream. Without this it is impossible to tell apart "the
		 * reader thread never ran", "the FPGA has no frames" and "the
		 * frames arrive but never reach ALSA".
		 */
		if (priv->dbg_reads < 8) {
			/*
			 * Find the last byte the controller actually filled.
			 * The status header arrives intact while the audio
			 * payload behind it reads as zeros, which is what a
			 * silently truncated transfer looks like - so measure
			 * where the data stops rather than assuming the whole
			 * buffer came back.
			 */
			const u8 *raw = (const u8 *)priv->rx_buf;
			size_t last_nz = 0, k;

			for (k = 0; k < sizeof(*priv->rx_buf); k++)
				if (raw[k])
					last_nz = k;

			priv->dbg_reads++;
			dev_err(&priv->spi->dev,
				"biscuit-xfer[%u]: asked %zu bytes, last nonzero at %zu\n",
				priv->dbg_reads, sizeof(*priv->rx_buf), last_nz);
			/*
			 * The status frame's own field comments number its
			 * bytes backwards - fpga_rev is called byte 0 but sits
			 * at offset 26 - so the wire order is reversed against
			 * the struct. If the audio payload does not begin
			 * where daf[] says it does, a correct header and an
			 * empty payload is exactly what we would see. Dump the
			 * bytes either side of the boundary and look.
			 */
			print_hex_dump(KERN_ERR, "biscuit-raw 0..63: ",
				       DUMP_PREFIX_OFFSET, 16, 1,
				       raw, 64, false);
			dev_err(&priv->spi->dev,
				"biscuit-rd[%u]: rev=%u mode=%u nframes=%u i2s_inact=%u dac_inact=%u overrun=%u -> %zu bytes\n",
				priv->dbg_reads, priv->rx_buf->dsf.fpga_rev,
				priv->rx_buf->dsf.mode,
				le16_to_cpu(priv->rx_buf->dsf.num_audio_frames),
				priv->rx_buf->dsf.i2s_inactive,
				priv->rx_buf->dsf.dac_inactive,
				priv->rx_buf->dsf.overrun, n_bytes);
		}

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

	dev_err(&priv->spi->dev, "biscuit-dbg: pcm open, stream=%d\n",
		substream->stream);
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
	priv->dbg_reads = 0;
	priv->dbg_ptr = 0;
	priv->armed_live = false;
	spin_lock_init(&priv->lock);

	/*
	 * Start the SPI reader here rather than in trigger().
	 *
	 * trigger() is called from snd_pcm_action_lock_irq() with the stream
	 * group lock held and interrupts disabled, so it may not sleep - and
	 * kthread_run() does.  Creating the thread there produced a
	 * "sleeping function called from invalid context" splat, the creation
	 * failed, and the very first read returned -EIO with zero frames
	 * captured.  open() is allowed to sleep, so the thread is created
	 * once here and simply parked until trigger flips priv->running.
	 */
	/*
	 * Re-issue the I2S-mode command now, at stream start.
	 *
	 * probe() sends this once, but at that point nothing is clocking the
	 * FPGA's I2S inputs - its own status frame reports i2s_inactive=1 and
	 * dac_inactive=1 there. Once the ADCs and the playback path are
	 * running those both clear, yet the part still hands back
	 * num_audio_frames=0: it latched "no I2S" when the command arrived and
	 * never starts framing. Telling it again once the clocks are live is
	 * what actually arms capture.
	 */
	{
		u8 i2s_cmd[32] = { biscuit_fpga_tpg ? DOUGH_FW_TPG
						   : DOUGH_FW_I2S };
		int cret = dough_spi_txrx(priv->spi, i2s_cmd, NULL,
					  sizeof(i2s_cmd));

		dev_err(&priv->spi->dev,
			"biscuit-dbg: FPGA %s mode armed at stream start (%d)\n",
			biscuit_fpga_tpg ? "TPG" : "I2S", cret);
	}

	priv->capture_thread = kthread_run(biscuit_spi_capture_thread, priv,
					   "biscuit-spi-pcm");
	dev_err(&priv->spi->dev, "biscuit-dbg: kthread_run -> %ld\n",
		IS_ERR(priv->capture_thread) ? PTR_ERR(priv->capture_thread) : 0L);
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

static int biscuit_spi_pcm_prepare(struct snd_soc_component *component,
				   struct snd_pcm_substream *substream)
{
	struct biscuit_spi_pcm *priv = snd_soc_component_get_drvdata(component);

	dev_err(&priv->spi->dev, "biscuit-dbg: pcm prepare, state=%d\n",
		(int)substream->runtime->state);
	return 0;
}

static int biscuit_spi_pcm_trigger(struct snd_soc_component *component,
				    struct snd_pcm_substream *substream, int cmd)
{
	struct biscuit_spi_pcm *priv = snd_soc_component_get_drvdata(component);

	/* Atomic context: flip a flag and wake the reader, nothing more. */
	pr_err("biscuit-dbg: trigger cmd=%d thread=%p\n", cmd, priv->capture_thread);
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

	if (priv->dbg_ptr < 6) {
		priv->dbg_ptr++;
		dev_err(&priv->spi->dev,
			"biscuit-ptr[%u]: off=%zu frames=%lu dma_area=%p dma_bytes=%zu state=%d\n",
			priv->dbg_ptr, priv->write_offset, (unsigned long)frames,
			substream->runtime->dma_area,
			(size_t)substream->runtime->dma_bytes,
			(int)substream->runtime->state);
	}

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
	.close		= biscuit_spi_pcm_close,
	.prepare	= biscuit_spi_pcm_prepare,
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
	dev_err(dev, "biscuit-dbg: fw size=%zu padded=%zu head=%02x %02x %02x %02x\n",
		fw->size, bytes, ((u8 *)fw_buf)[0], ((u8 *)fw_buf)[1],
		((u8 *)fw_buf)[2], ((u8 *)fw_buf)[3]);
	release_firmware(fw);

	ret = dough_spi_txrx(priv->spi, fw_buf, NULL, bytes);
	kfree(fw_buf);
	dev_err(dev, "biscuit-dbg: bitstream SPI send ret=%d (%zu bytes)\n", ret, bytes);
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
	 * Put the FPGA into slave-SPI configuration mode.
	 *
	 * This part is a Lattice iCE40UL1K-SWG16 - the bitstream says so in
	 * plain ASCII ("Part: iCE40UL1K-SWG16", iCEcube2, May 2016) ahead of
	 * the 7E AA 99 7E preamble at offset 106.  An iCE40 samples its
	 * configuration mode from SPI_SS_B at the moment CRESET_B is released:
	 * SS_B low selects slave SPI, where the host clocks the bitstream in;
	 * SS_B high selects master SPI, where the FPGA tries to boot itself
	 * from an external configuration flash.  There is no such flash on this
	 * board.
	 *
	 * What this code did before was toggle CRESET_B on its own.  The SPI
	 * core only asserts chip-select around an actual transfer, so SS_B was
	 * high for the entire reset pulse and the FPGA came up every time in
	 * master mode, looking for a flash that does not exist.  It then
	 * ignored the bitstream we clocked at it and answered every register
	 * read with zeros - exactly the "revision 0" we have been chasing,
	 * with a bitstream that was correct all along.
	 *
	 * The sequence below is the one mainline's own iCE40 FPGA manager uses
	 * (drivers/fpga/ice40-spi.c): take the bus, drop CRESET_B, run a
	 * zero-length transfer whose cs_change keeps SS_B asserted afterwards,
	 * release CRESET_B while SS_B is still low, then a second transfer
	 * whose completion releases SS_B once the FPGA has finished clearing
	 * its configuration memory.
	 */
	dev_err(dev, "biscuit-dbg: pre-reset gpio logical=%d\n",
		gpiod_get_value_cansleep(priv->reset_gpio));
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

		/* Release CRESET_B with SS_B still asserted - this is the bit
		 * that selects slave SPI rather than master SPI.
		 */
		gpiod_set_value_cansleep(priv->reset_gpio, 0);

		if (!cret) {
			spi_message_init(&msg);
			spi_message_add_tail(&housekeeping_delay_then_release_cs,
					     &msg);
			cret = spi_sync_locked(spi, &msg);
		}

		spi_bus_unlock(spi->controller);

		dev_err(dev, "biscuit-dbg: slave-SPI entry ret=%d, gpio logical=%d\n",
			cret, gpiod_get_value_cansleep(priv->reset_gpio));
	}
	msleep(FPGA_RESET_MS);

	/*
	 * biscuit-dbg: is the FPGA alive BEFORE any bitstream load? If it boots
	 * a base design from NVCM it should answer here; all-zero means it needs
	 * an external config. Also re-read SENINF to confirm the clock config
	 * survived the reset.
	 */
	ret = dough_spi_txrx(spi, tx_df, rx_df, sizeof(*rx_df));
	dev_err(dev, "biscuit-dbg: PRE-config revread ret=%d rev=%u; SENINF TG1_PH_CNT=0x%08x SEN_CK=0x%08x\n",
		ret, rx_df->dsf.fpga_rev,
		readl(priv->seninf + SENINF_TG1_PH_CNT),
		readl(priv->seninf + SENINF_TG1_SEN_CK));
	print_hex_dump(KERN_ERR, "biscuit-dbg pre raw: ", DUMP_PREFIX_NONE, 16, 1, rx_df, 32, false);

	/*
	 * Firmware-missing must NOT be fatal. This FPGA only carries the
	 * mic-array capture path; the speaker playback path (AFE DL1 -> I2S0 ->
	 * aic32x4) is entirely independent of it. If we returned an error here
	 * when /lib/firmware/i2s_to_spi_v34.bin is absent, this component's DAI
	 * would never register, the simple-audio-card that references it would
	 * fail to parse, and the whole sound card - speaker included - would
	 * disappear. So on firmware failure we warn, skip the FPGA bring-up
	 * (revision read / I2S mode), and still register the component; the mic
	 * capture DAI then exists but is non-functional until the firmware is
	 * dropped in and the device re-probed.
	 */
	ret = biscuit_spi_pcm_load_firmware(priv);
	dev_err(dev, "biscuit-dbg: load_firmware ret=%d\n", ret);
	if (ret < 0) {
		dev_warn(dev,
			 "FPGA firmware unavailable (%d); registering component anyway - mic-array capture is non-functional until /lib/firmware/%s is present, speaker playback is unaffected\n",
			 ret, FPGA_FIRMWARE_NAME);
		goto register_component;
	}

	msleep(FPGA_RESET_MS);

	/* biscuit-dbg: retry the rev-read many times with full dumps to see if
	 * the FPGA ever responds, and whether MISO is consistently idle-0. */
	{
		int att;

		for (att = 0; att < 15; att++) {
			ret = dough_spi_txrx(spi, tx_df, rx_df, sizeof(*rx_df));
			dev_err(dev, "biscuit-dbg: revread att=%d ret=%d rev=%u nframes=%u\n",
				att, ret, rx_df->dsf.fpga_rev,
				le16_to_cpu(rx_df->dsf.num_audio_frames));
			print_hex_dump(KERN_ERR, "biscuit-dbg raw: ", DUMP_PREFIX_NONE,
				       16, 1, rx_df, 32, false);
			if (dough_rev_ok(rx_df->dsf.fpga_rev))
				break;
			msleep(25);
		}
	}

	/*
	 * Every FPGA bring-up step below is best-effort for the same reason as
	 * the firmware load above: a failure here only costs mic-array capture,
	 * so warn and still register the component rather than taking the whole
	 * sound card (speaker included) down with a fatal probe error. Observed
	 * on this board: the "dough" FPGA reads back revision 0 (no response on
	 * SPI0) even with the firmware present, so without this the card never
	 * forms.
	 */
	ret = dough_spi_txrx(spi, tx_df, rx_df, sizeof(*rx_df));
	if (ret < 0) {
		dev_warn(dev, "FPGA revision read failed (%d); mic capture non-functional\n", ret);
		goto register_component;
	}

	print_hex_dump(KERN_INFO, "biscuit-spi-pcm: rev-read raw: ", DUMP_PREFIX_NONE,
		       16, 1, rx_df, 32, false);

	if (!dough_rev_ok(rx_df->dsf.fpga_rev)) {
		dev_warn(dev, "unrecognized FPGA revision %u (mic capture non-functional); speaker unaffected\n",
			 rx_df->dsf.fpga_rev);
		goto register_component;
	}

	dev_info(dev, "FPGA revision %u\n", rx_df->dsf.fpga_rev);

	/*
	 * MCLK comes up only NOW, matching Amazon's stock probe order exactly
	 * (amzn-mt-spi-pcm.c): the off command, reset pulse, bitstream load and
	 * revision read all happen with the CMMCLK pad still unmuxed, and only
	 * after the revision verifies does the stock driver call
	 * AudDrv_GPIO_MCLK_Select() / restore I2S. The FPGA therefore does NOT
	 * need its reference clock to answer on SPI - that clock is for the I2S
	 * audio path. Bringing it up earlier (as this driver used to) deviates
	 * from stock, so do it here.
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
	 * Full FPGA reference-clock bring-up, matching Amazon's stock
	 * mt_amzn_mclk.c. The CMMCLK pad is driven by the camera SENINF timing
	 * generator, not CAMTG_SEL alone: enable the imgsys sensor clocks, set
	 * CAMTG_SEL to 48 MHz (UNIVPLL_D26), then program the SENINF divider to
	 * emit 9.6 MHz. Without this the FPGA has no clock and reads revision 0.
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

	dev_err(dev, "biscuit-dbg: rates mclk=%lu sen_tg=%lu sen_cam=%lu larb2=%lu cam_smi=%lu\n",
		clk_get_rate(priv->mclk), clk_get_rate(priv->sen_tg),
		clk_get_rate(priv->sen_cam), clk_get_rate(priv->larb2_smi),
		clk_get_rate(priv->cam_smi));

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

	/* biscuit-dbg: does the FPGA answer once MCLK + I2S are up? */
	{
		int a;

		for (a = 0; a < 5; a++) {
			msleep(20);
			ret = dough_spi_txrx(spi, tx_df, rx_df, sizeof(*rx_df));
			dev_err(dev, "biscuit-dbg: POST-mclk revread a=%d ret=%d rev=%u mode=%u nframes=%u\n",
				a, ret, rx_df->dsf.fpga_rev, rx_df->dsf.mode,
				le16_to_cpu(rx_df->dsf.num_audio_frames));
			print_hex_dump(KERN_ERR, "biscuit-dbg post raw: ", DUMP_PREFIX_NONE,
				       16, 1, rx_df, 32, false);
			if (dough_rev_ok(rx_df->dsf.fpga_rev))
				break;
		}
	}

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
