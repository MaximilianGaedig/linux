// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT8163 camera sensor interface (SENINF) master clock
 *
 * Timing generator 1 of SENINF divides the CAMTG clock and drives the result
 * onto the CMMCLK pad, which a board can use as a general purpose clock
 * output when no camera sensor is fitted. The register and field names follow
 * the MediaTek vendor kernel.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#define SENINF_TOP_CTRL			0x000
#define   SENINF12_PCLK_MASK		GENMASK(11, 10)
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

#define SENINF_TG1_DIV_MAX		(FIELD_MAX(SENINF_TG1_CLKCNT) + 1)

struct mt8163_seninf {
	struct clk_hw hw;
	struct device *dev;
	void __iomem *base;
	struct clk_bulk_data *clks;
	int num_clks;
	/* division ratio, applied to the hardware whenever the clock is prepared */
	unsigned int div;
};

#define to_mt8163_seninf(_hw) container_of(_hw, struct mt8163_seninf, hw)

static void seninf_clrset(struct mt8163_seninf *s, u32 off, u32 clr, u32 set)
{
	writel((readl(s->base + off) & ~clr) | set, s->base + off);
}

/*
 * TG1 counts CLKCNT + 1 source periods, raising the output at count CLKRS and
 * dropping it at count CLKFL. For an odd ratio CLKFL_POL moves the falling
 * edge half a source period later, which keeps the duty cycle at 50%.
 */
static void mt8163_seninf_program(struct mt8163_seninf *s)
{
	unsigned int cnt = s->div - 1;
	unsigned int fl = cnt ? (cnt + 1) / 2 : 1;

	seninf_clrset(s, SENINF_TOP_CTRL, SENINF12_PCLK_MASK, SENINF12_PCLK_VAL);

	/* The divider only takes effect once TG1_PH_CNT is written again. */
	seninf_clrset(s, SENINF_TG1_SEN_CK,
		      SENINF_TG1_CLKFL | SENINF_TG1_CLKRS | SENINF_TG1_CLKCNT,
		      FIELD_PREP(SENINF_TG1_CLKFL, fl) |
		      FIELD_PREP(SENINF_TG1_CLKCNT, cnt));
	seninf_clrset(s, SENINF_TG1_PH_CNT,
		      SENINF_TG1_TGCLK_SEL | SENINF_TG1_CLKFL_POL |
		      SENINF_TG1_PADCLK_INV | SENINF_TG1_CLK_POL,
		      FIELD_PREP(SENINF_TG1_TGCLK_SEL, SENINF_TG1_TGCLK_SEL_CAMTG) |
		      (cnt % 2 ? 0 : SENINF_TG1_CLKFL_POL));
	seninf_clrset(s, SENINF_TG1_PH_CNT, 0,
		      SENINF_TG1_MCLK_EN | SENINF_TG1_PCEN);

	seninf_clrset(s, SENINF1_MUX_CTRL, 0, SENINF1_MUX_EN);
	seninf_clrset(s, SENINF1_CTRL, 0, SENINF1_EN);
}

static int mt8163_seninf_prepare(struct clk_hw *hw)
{
	struct mt8163_seninf *s = to_mt8163_seninf(hw);
	int ret;

	/* The registers are only accessible with the ISP domain and gates on. */
	ret = pm_runtime_resume_and_get(s->dev);
	if (ret)
		return ret;

	ret = clk_bulk_prepare_enable(s->num_clks, s->clks);
	if (ret) {
		pm_runtime_put(s->dev);
		return ret;
	}

	mt8163_seninf_program(s);

	return 0;
}

static void mt8163_seninf_unprepare(struct clk_hw *hw)
{
	struct mt8163_seninf *s = to_mt8163_seninf(hw);

	seninf_clrset(s, SENINF_TG1_PH_CNT,
		      SENINF_TG1_MCLK_EN | SENINF_TG1_PCEN, 0);

	clk_bulk_disable_unprepare(s->num_clks, s->clks);
	pm_runtime_put(s->dev);
}

static unsigned long mt8163_seninf_recalc_rate(struct clk_hw *hw,
					       unsigned long parent_rate)
{
	return parent_rate / to_mt8163_seninf(hw)->div;
}

static int mt8163_seninf_determine_rate(struct clk_hw *hw,
					struct clk_rate_request *req)
{
	unsigned int div;

	if (!req->rate)
		return -EINVAL;

	div = clamp(DIV_ROUND_CLOSEST(req->best_parent_rate, req->rate),
		    1UL, SENINF_TG1_DIV_MAX);
	req->rate = req->best_parent_rate / div;

	return 0;
}

static int mt8163_seninf_set_rate(struct clk_hw *hw, unsigned long rate,
				  unsigned long parent_rate)
{
	struct mt8163_seninf *s = to_mt8163_seninf(hw);

	s->div = clamp(DIV_ROUND_CLOSEST(parent_rate, rate),
		       1UL, SENINF_TG1_DIV_MAX);

	if (clk_hw_is_prepared(hw))
		mt8163_seninf_program(s);

	return 0;
}

static const struct clk_ops mt8163_seninf_ops = {
	.prepare = mt8163_seninf_prepare,
	.unprepare = mt8163_seninf_unprepare,
	.recalc_rate = mt8163_seninf_recalc_rate,
	.determine_rate = mt8163_seninf_determine_rate,
	.set_rate = mt8163_seninf_set_rate,
};

static int mt8163_seninf_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct clk_init_data init = {};
	struct mt8163_seninf *s;
	const char *name;
	int ret;

	s = devm_kzalloc(dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;

	s->dev = dev;
	s->div = 1;

	s->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(s->base))
		return PTR_ERR(s->base);

	ret = devm_clk_bulk_get_all(dev, &s->clks);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to get clocks\n");
	s->num_clks = ret;

	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;

	name = dev_name(dev);
	of_property_read_string(dev->of_node, "clock-output-names", &name);

	init.name = name;
	init.ops = &mt8163_seninf_ops;
	init.parent_data = &(const struct clk_parent_data) { .fw_name = "camtg" };
	init.num_parents = 1;
	s->hw.init = &init;

	ret = devm_clk_hw_register(dev, &s->hw);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register clock\n");

	return devm_of_clk_add_hw_provider(dev, of_clk_hw_simple_get, &s->hw);
}

static const struct of_device_id mt8163_seninf_of_match[] = {
	{ .compatible = "mediatek,mt8163-seninf" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mt8163_seninf_of_match);

static struct platform_driver mt8163_seninf_driver = {
	.probe = mt8163_seninf_probe,
	.driver = {
		.name = "clk-mt8163-seninf",
		.of_match_table = mt8163_seninf_of_match,
	},
};
module_platform_driver(mt8163_seninf_driver);

MODULE_DESCRIPTION("MediaTek MT8163 SENINF master clock driver");
MODULE_LICENSE("GPL");
