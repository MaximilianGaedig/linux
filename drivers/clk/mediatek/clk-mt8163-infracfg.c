// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2015 MediaTek Inc.
 *         James Liao <jamesjj.liao@mediatek.com>
 * Copyright (c) 2025-2026 Ben Grisdale <bengris32@protonmail.ch>
 */

#include <linux/clk-provider.h>
#include <linux/platform_device.h>

#include "clk-cpumux.h"
#include "clk-gate.h"
#include "clk-mtk.h"

#include <dt-bindings/clock/mediatek,mt8163-infracfg.h>
#include <dt-bindings/reset/mediatek,mt8163-infracfg.h>

#define RST_NR_PER_BANK			32

static DEFINE_SPINLOCK(mt8163_peri_clk_lock);

static const struct mtk_gate_regs infra0_cg_regs = {
	.set_ofs = 0x0080,
	.clr_ofs = 0x0084,
	.sta_ofs = 0x0090,
};

static const struct mtk_gate_regs infra1_cg_regs = {
	.set_ofs = 0x0088,
	.clr_ofs = 0x008c,
	.sta_ofs = 0x0094,
};

#define GATE_ICG0(_id, _name, _parent, _shift)								\
	GATE_MTK(_id, _name, _parent, &infra0_cg_regs, _shift, &mtk_clk_gate_ops_setclr)

#define GATE_ICG0_FLAGS(_id, _name, _parent, _shift, _flags)				\
	GATE_MTK_FLAGS(_id, _name, _parent, &infra0_cg_regs, _shift,			\
		       &mtk_clk_gate_ops_setclr, _flags)

#define GATE_ICG1(_id, _name, _parent, _shift)								\
	GATE_MTK(_id, _name, _parent, &infra1_cg_regs, _shift, &mtk_clk_gate_ops_setclr)

#define GATE_ICG1_FLAGS(_id, _name, _parent, _shift, _flags)				\
	GATE_MTK_FLAGS(_id, _name, _parent, &infra1_cg_regs, _shift,			\
		       &mtk_clk_gate_ops_setclr, _flags)

static const struct mtk_gate infracfg_gates[] = {
	/* INFRA0 */
	GATE_ICG0(CLK_INFRA_PMIC_TMR, "infra_pmic_tmr", "clk26m", 0),
	GATE_ICG0(CLK_INFRA_PMIC_AP, "infra_pmic_ap", "clk26m", 1),
	GATE_ICG0(CLK_INFRA_PMIC_MD, "infra_pmic_md", "clk26m", 2),
	GATE_ICG0(CLK_INFRA_PMIC_CONN, "infra_pmic_conn", "clk26m", 3),
	GATE_ICG0(CLK_INFRA_SCPSYS, "infra_scpsys", "scp_sel", 4),
	GATE_ICG0(CLK_INFRA_SEJ, "infra_sej", "axi_sel", 5),
	GATE_ICG0_FLAGS(CLK_INFRA_APXGPT, "infra_apxgpt", "axi_sel",
		  6, CLK_IS_CRITICAL),
	GATE_ICG0(CLK_INFRA_USB, "infra_usb", "axi_sel", 7),
	GATE_ICG0(CLK_INFRA_ICUSB, "infra_icusb", "axi_sel", 8),
	GATE_ICG0(CLK_INFRA_GCE, "infra_gce", "axi_sel", 9),
	GATE_ICG0(CLK_INFRA_THERM, "infra_therm", "axi_sel", 10),
	GATE_ICG0(CLK_INFRA_I2C0, "infra_i2c0", "axi_sel", 11),
	GATE_ICG0(CLK_INFRA_I2C1, "infra_i2c1", "axi_sel", 12),
	GATE_ICG0(CLK_INFRA_I2C2, "infra_i2c2", "axi_sel", 13),
	GATE_ICG0(CLK_INFRA_PWM_HCLK, "infra_pwm_hclk", "axi_sel", 15),
	GATE_ICG0(CLK_INFRA_PWM1, "infra_pwm1", "axi_sel", 16),
	GATE_ICG0(CLK_INFRA_PWM2, "infra_pwm2", "axi_sel", 17),
	GATE_ICG0(CLK_INFRA_PWM3, "infra_pwm3", "axi_sel", 18),
	GATE_ICG0(CLK_INFRA_PWM, "infra_pwm", "axi_sel", 21),
	GATE_ICG0(CLK_INFRA_UART0, "infra_uart0", "uart_sel", 22),
	GATE_ICG0(CLK_INFRA_UART1, "infra_uart1", "uart_sel", 23),
	GATE_ICG0(CLK_INFRA_UART2, "infra_uart2", "uart_sel", 24),
	GATE_ICG0(CLK_INFRA_UART3, "infra_uart3", "uart_sel", 25),
	GATE_ICG0(CLK_INFRA_USB_MCU, "infra_usb_mcu", "axi_sel", 26),
	GATE_ICG0(CLK_INFRA_NFI_ECC_66M, "infra_nfi_ecc66", "axi_sel", 27),
	GATE_ICG0(CLK_INFRA_NFI_66M, "infra_nfi_66m", "axi_sel", 28),
	GATE_ICG0(CLK_INFRA_BTIF, "infra_btif", "axi_sel", 31),

	/* INFRA1 */
	GATE_ICG1(CLK_INFRA_SPI, "infra_spi", "spi_sel", 1),
	GATE_ICG1(CLK_INFRA_MSDC3, "infra_msdc3", "msdc50_3_sel", 2),
	GATE_ICG1(CLK_INFRA_MSDC1, "infra_msdc1", "msdc30_1_sel", 4),
	GATE_ICG1(CLK_INFRA_MSDC2, "infra_msdc2", "msdc30_2_sel", 5),
	GATE_ICG1(CLK_INFRA_MSDC0, "infra_msdc0", "msdc30_0_sel", 6),
	GATE_ICG1(CLK_INFRA_GCPU, "infra_gcpu", "axi_sel", 8),
	GATE_ICG1(CLK_INFRA_TRNG, "infra_trng", "axi_sel", 9),
	GATE_ICG1(CLK_INFRA_AUXADC, "infra_auxadc", "clk26m", 10),
	GATE_ICG1(CLK_INFRA_CPUM, "infra_cpum", "axi_sel", 11),
	GATE_ICG1(CLK_INFRA_IRRX, "infra_irrx", "rtc_sel", 12),
	GATE_ICG1(CLK_INFRA_UFO, "infra_ufo", "axi_sel", 13),
	GATE_ICG1(CLK_INFRA_CEC, "infra_cec", "rtc_sel", 14),
	GATE_ICG1(CLK_INFRA_CEC_26M, "infra_cec_26m", "clk26m", 15),
	GATE_ICG1(CLK_INFRA_NFI_BCLK, "infra_nfi_bclk", "spi_sel", 16),
	GATE_ICG1(CLK_INFRA_NFI_ECC, "infra_nfi_ecc", "spi_sel", 17),
	GATE_ICG1(CLK_INFRA_AP_DMA, "infra_ap_dma", "axi_sel", 18),
	GATE_ICG1(CLK_INFRA_XIU, "infra_xiu", "axi_sel", 19),
	GATE_ICG1(CLK_INFRA_DEVICE_APC, "infra_devapc", "axi_sel", 20),
	GATE_ICG1(CLK_INFRA_XIU2AHB, "infra_xiu2ahb", "axi_sel", 21),
	GATE_ICG1(CLK_INFRA_L2C_SRAM, "infra_l2c_sram", "axi_sel", 22),
	GATE_ICG1(CLK_INFRA_ETH_50M, "infra_eth_50m", "eth_sel", 23),
	GATE_ICG1(CLK_INFRA_DEBUGSYS, "infra_debugsys", "axi_sel", 24),
	GATE_ICG1(CLK_INFRA_AUDIO, "infra_audio", "axi_sel", 25),
	GATE_ICG1(CLK_INFRA_ETH_25M, "infra_eth_25m", "eth_sel", 26),
	GATE_ICG1(CLK_INFRA_NFI, "infra_nfi", "axi_sel", 27),
	GATE_ICG1(CLK_INFRA_ONFI, "infra_onfi", "onfi_sel", 28),
	GATE_ICG1(CLK_INFRA_SNFI, "infra_snfi", "snfi_sel", 29),
	GATE_ICG1(CLK_INFRA_ETH, "infra_eth", "axi_sel", 30),
	GATE_ICG1_FLAGS(CLK_INFRA_DRAMC_F26M, "infra_dramc_26m", "clk26m",
		  31, CLK_IS_CRITICAL),
};

static const struct mtk_fixed_factor infra_divs[] = {
	FACTOR(CLK_INFRA_OSC, "osc_ck", "clk26m", 8, 1),
	FACTOR(CLK_INFRA_OSC_D8, "osc_d8", "osc_ck", 1, 8),
	FACTOR(CLK_INFRA_OSC_D16, "osc_d16", "osc_ck", 1, 16),
	FACTOR(CLK_INFRA_CLK26M_D8, "clk26m_d8", "clk26m", 1, 8),
	FACTOR(CLK_INFRA_ETH_D2, "eth_d2", "eth_sel", 1, 2),
	FACTOR(CLK_INFRA_ONFI_D2, "onfi_d2", "onfi_sel", 1, 2),
};

static const char * const infra_uart_parents[] = {
	"clk26m",
	"uart_sel"
};

static const char * const infra_spi_parents[] = {
	"axi_sel",
	"spi_sel"
};

static const char * const infra_dramc_parents[] = {
	"clk26m",
	"clk26m_d8"
};

static const char * const infra_ulp_parents[] = {
	"osc_d8",
	"osc_d16"
};

static const char * const infra_eth_parents[] = {
	"eth_d2",
	"eth_sel"
};

static const char * const infra_nfi_parents[] = {
	"axi_sel",
	"onfi_d2"
};

static const char * const infra_ca53_parents[] = {
	"clk26m",
	"armpll",
	"arm_div_pll1_en",
	"arm_div_pll2_en",
};

static const struct mtk_composite infracfg_muxes[] = {
	MUX(CLK_INFRA_UART0_SEL, "infra_uart0_sel", infra_uart_parents, 0x098, 0, 1),
	MUX(CLK_INFRA_UART1_SEL, "infra_uart1_sel", infra_uart_parents, 0x098, 1, 1),
	MUX(CLK_INFRA_UART2_SEL, "infra_uart2_sel", infra_uart_parents, 0x098, 2, 1),
	MUX(CLK_INFRA_UART3_SEL, "infra_uart3_sel", infra_uart_parents, 0x098, 3, 1),
	MUX(CLK_INFRA_SPI_SEL, "infra_spi_sel", infra_spi_parents, 0x098, 4, 1),
	MUX(CLK_INFRA_DRAMC_SEL, "infra_dramc_sel", infra_dramc_parents, 0x098, 7, 1),
	MUX(CLK_INFRA_ULPOSC_SEL, "infra_ulp_sel", infra_ulp_parents, 0x098, 8, 1),
	MUX(CLK_INFRA_ETH_25M_SEL, "infra_eth_sel", infra_eth_parents, 0x098, 9, 1),
	MUX(CLK_INFRA_NFI_SEL, "infra_nfi_sel", infra_nfi_parents, 0x098, 10, 1),
};

static const struct mtk_composite cpu_muxes[] = {
	MUX(CLK_INFRA_CA53SEL, "infra_ca53_sel", infra_ca53_parents, 0x0000, 0, 2),
};

static u16 infracfg_rst_bank_ofs[] = { 0x120, 0x140 };

static u16 infracfg_rst_idx_map[] = {
	/* INFRA_GLOBALCON_RST_0 */
	[MT8163_INFRA_RST0_THERM_CTRL]		= 0 * RST_NR_PER_BANK + 0,
	[MT8163_INFRA_RST0_USB_TOP]			= 0 * RST_NR_PER_BANK + 1,
	[MT8163_INFRA_RST0_PERI_IOMMU]		= 0 * RST_NR_PER_BANK + 2,
	[MT8163_INFRA_RST0_MM_IOMMU]		= 0 * RST_NR_PER_BANK + 3,
	[MT8163_INFRA_RST0_MSDC3]			= 0 * RST_NR_PER_BANK + 4,
	[MT8163_INFRA_RST0_MSDC2]			= 0 * RST_NR_PER_BANK + 5,
	[MT8163_INFRA_RST0_MSDC1]			= 0 * RST_NR_PER_BANK + 6,
	[MT8163_INFRA_RST0_MSDC0]			= 0 * RST_NR_PER_BANK + 7,
	[MT8163_INFRA_RST0_DRAMC]			= 0 * RST_NR_PER_BANK + 8,
	[MT8163_INFRA_RST0_AP_DMA]			= 0 * RST_NR_PER_BANK + 9,
	[MT8163_INFRA_RST0_MIPI_D]			= 0 * RST_NR_PER_BANK + 10,
	[MT8163_INFRA_RST0_MIPI_C]			= 0 * RST_NR_PER_BANK + 11,
	[MT8163_INFRA_RST0_BTIF]			= 0 * RST_NR_PER_BANK + 12,

	/* INFRA_GLOBALCON_RST_1 */
	[MT8163_INFRA_RST1_PMIC_WRAP]		= 1 * RST_NR_PER_BANK + 0,
	[MT8163_INFRA_RST1_SPM]				= 1 * RST_NR_PER_BANK + 1,
	[MT8163_INFRA_RST1_USBSIF]			= 1 * RST_NR_PER_BANK + 2,
	[MT8163_INFRA_RST1_SCP]				= 1 * RST_NR_PER_BANK + 3,
	[MT8163_INFRA_RST1_CEC]				= 1 * RST_NR_PER_BANK + 4,
	[MT8163_INFRA_RST1_IRRX]			= 1 * RST_NR_PER_BANK + 5,
};

static const struct mtk_clk_rst_desc clk_rst_desc = {
	.version = MTK_RST_SET_CLR,
	.rst_bank_ofs = infracfg_rst_bank_ofs,
	.rst_bank_nr = ARRAY_SIZE(infracfg_rst_bank_ofs),
	.rst_idx_map = infracfg_rst_idx_map,
	.rst_idx_map_nr = ARRAY_SIZE(infracfg_rst_idx_map)
};

static int clk_mt8163_infracfg_probe(struct platform_device *pdev)
{
	struct clk_hw_onecell_data *clk_data;
	struct device_node *node = pdev->dev.of_node;
	void __iomem *base;
	int ret;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	clk_data = mtk_alloc_clk_data(CLK_INFRA_NR_CLK);
	if (!clk_data)
		return -ENOMEM;

	ret = mtk_register_reset_controller_with_dev(&pdev->dev, &clk_rst_desc);
	if (ret)
		goto free_clk_data;

	ret = mtk_clk_register_gates(&pdev->dev, node, infracfg_gates,
					  ARRAY_SIZE(infracfg_gates), clk_data);
	if (ret)
		goto free_clk_data;

	ret = mtk_clk_register_composites(&pdev->dev, infracfg_muxes,
					  ARRAY_SIZE(infracfg_muxes), base,
					  &mt8163_peri_clk_lock, clk_data);
	if (ret)
		goto unregister_gates;

	ret = mtk_clk_register_cpumuxes(&pdev->dev, node, cpu_muxes,
					  ARRAY_SIZE(cpu_muxes), clk_data);
	if (ret)
		goto unregister_composites;

	ret = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, clk_data);
	if (ret)
		goto unregister_cpumuxes;

	return 0;

unregister_cpumuxes:
	mtk_clk_unregister_cpumuxes(cpu_muxes, ARRAY_SIZE(cpu_muxes), clk_data);
unregister_composites:
	mtk_clk_unregister_composites(infracfg_muxes, ARRAY_SIZE(infracfg_muxes), clk_data);
unregister_gates:
	mtk_clk_unregister_gates(infracfg_gates, ARRAY_SIZE(infracfg_gates), clk_data);
free_clk_data:
	mtk_free_clk_data(clk_data);
	return ret;
}

static void clk_mt8163_infracfg_remove(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct clk_hw_onecell_data *clk_data = platform_get_drvdata(pdev);

	of_clk_del_provider(node);
	mtk_clk_unregister_cpumuxes(cpu_muxes, ARRAY_SIZE(cpu_muxes), clk_data);
	mtk_clk_unregister_composites(infracfg_muxes, ARRAY_SIZE(infracfg_muxes), clk_data);
	mtk_clk_unregister_gates(infracfg_gates, ARRAY_SIZE(infracfg_gates), clk_data);
	mtk_free_clk_data(clk_data);
}

static const struct of_device_id of_match_clk_mt8163_infracfg[] = {
	{ .compatible = "mediatek,mt8163-infracfg" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt8163_infracfg);

static struct platform_driver clk_mt8163_infracfg_drv = {
	.driver = {
		.name = "clk-mt8163-infracfg",
		.of_match_table = of_match_clk_mt8163_infracfg,
	},
	.probe = clk_mt8163_infracfg_probe,
	.remove = clk_mt8163_infracfg_remove,
};
module_platform_driver(clk_mt8163_infracfg_drv);

MODULE_AUTHOR("James Liao <jamesjj.liao@mediatek.com>");
MODULE_AUTHOR("Ben Grisdale <bengris32@protonmail.ch>");
MODULE_DESCRIPTION("MediaTek MT8163 infracfg clock driver");
MODULE_LICENSE("GPL");
