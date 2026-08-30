/*! \file
    \brief  Declaration of library functions

    Any definitions in this file will be shared among GLUE Layer and internal Driver Stack.
*/

/*******************************************************************************
*                         C O M P I L E R   F L A G S
********************************************************************************
*/

/*******************************************************************************
*                                 M A C R O S
********************************************************************************
*/

#ifdef DFT_TAG
#undef DFT_TAG
#endif
#define DFT_TAG "[WMT-CONSYS-HW]"

/*******************************************************************************
*                    E X T E R N A L   R E F E R E N C E S
********************************************************************************
*/
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/memblock.h>
#include <linux/pinctrl/consumer.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/regmap.h>
#include "osal_typedef.h"
#include "mtk_wcn_consys_hw.h"
#include <linux/mfd/mt6323/registers.h>
/*
 * <soc/mediatek/pmic_wrap.h> and pwrap_node_to_regmap() were a
 * MediaTek-downstream-only API (not in mainline; mainline's PMIC wrap
 * driver is drivers/soc/mediatek/mtk-pmic-wrap.c and does not expose an
 * of-node-to-regmap helper like this). This file uses pmic_regmap for a
 * handful of *raw* MT6323 PMIC register pokes (VCN28/VCN33 BT+WIFI
 * power sequencing, offsets 0x41C/0x416/0x418) as an alternative to the
 * standard regulator_*() calls used elsewhere in this same file for the
 * same rails (reg_VCN18/reg_VCN28/reg_VCN33_BT/reg_VCN33_WIFI, wired to
 * mt6323.dtsi's regulator-fixed nodes and already working). It is NOT
 * YET PORTED: pmic_regmap is left permanently NULL below (no
 * "mediatek,pwrap-regmap" phandle is wired in this tree's DT), and
 * every regmap_update_bits(pmic_regmap, ...) call site downstream is
 * now guarded to skip rather than NULL-deref. Whether these raw pokes
 * are actually load-bearing (vs. redundant with the regulator_*() calls
 * already present) is unverified -- needs checking against Amazon's
 * production behavior/logic analyzer before deciding whether to wire a
 * real mainline regmap accessor here or delete this path entirely.
 */
#include <linux/regmap.h>
#if CONSYS_EMI_MPU_SETTING
#include <emi_mpu.h>
#endif

#include <linux/regulator/consumer.h>
#ifdef CONFIG_MTK_HIBERNATION
#include <mtk_hibernate_dpm.h>
#endif

#include <linux/of_reserved_mem.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>

#if CONSYS_CLOCK_BUF_CTRL
#include <mt_clkbuf_ctl.h>
#endif

#include <linux/pm_runtime.h>
#include <linux/reset.h>

/*******************************************************************************
*                              C O N S T A N T S
********************************************************************************
*/

/*******************************************************************************
*                             D A T A   T Y P E S
********************************************************************************
*/

/*******************************************************************************
*                  F U N C T I O N   D E C L A R A T I O N S
********************************************************************************
*/
static INT32 mtk_wmt_probe(struct platform_device *pdev);
/* platform_driver.remove returns void since v6.11. */
static VOID mtk_wmt_remove(struct platform_device *pdev);


/*******************************************************************************
*                            P U B L I C   D A T A
********************************************************************************
*/

struct CONSYS_BASE_ADDRESS conn_reg;
/*
 * Not static: the WiFi driver needs it to verify that the firmware
 * sections whose destination lies in the CONNSYS EMI window
 * (0xf0000000 and up) really landed in this reserved DRAM region.
 * hif.h already declares it extern.
 */
phys_addr_t gConEmiPhyBase;
EXPORT_SYMBOL(gConEmiPhyBase);
static UINT8 __iomem *pEmibaseaddr;

/* 1 = run the 2MB AP write/readback test at consys power-on (slow) */
static int biscuit_emitest;
module_param(biscuit_emitest, int, 0644);
static struct clk *clk_infra_conn_main;	/*ctrl infra_connmcu_bus clk */
static struct platform_device *my_pdev;
static struct reset_control *rstc;
static struct regulator *reg_VCN18;
static struct regulator *reg_VCN28;
static struct regulator *reg_VCN33_BT;
static struct regulator *reg_VCN33_WIFI;
static struct pinctrl *consys_pinctrl;
static struct pinctrl *mt6625_spi_pinctrl;
static struct pinctrl_state *mt6625_spi_default;
static struct regmap *pmic_regmap;
#define DYNAMIC_DUMP_GROUP_NUM 5

static const struct of_device_id apwmt_of_ids[] = {
	{.compatible = "mediatek,mt8163-consys",},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, apwmt_of_ids);

static struct platform_driver mtk_wmt_dev_drv = {
	.probe = mtk_wmt_probe,
	.remove = mtk_wmt_remove,
	.driver = {
		   .name = "mt8163consys",
		   .owner = THIS_MODULE,
		   .of_match_table = of_match_ptr(apwmt_of_ids),
		   },
};

static INT32 mtk_wmt_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct device_node *node = NULL;

	pm_runtime_enable(&pdev->dev);
	my_pdev = pdev;
	mt6625_spi_pinctrl  = devm_pinctrl_get(&pdev->dev);
	if (IS_ERR(mt6625_spi_pinctrl)) {
		ret = PTR_ERR(mt6625_spi_pinctrl);
		WMT_PLAT_ERR_FUNC("Wmt cannot find pinctrl!\n");
		goto set_pin_exit;
	}
	mt6625_spi_default = pinctrl_lookup_state(mt6625_spi_pinctrl, "consys_pins_default");
	if (IS_ERR(mt6625_spi_default)) {
		ret = PTR_ERR(mt6625_spi_default);
		WMT_PLAT_ERR_FUNC("Wmt Cannot find pinctrl default!\n");
		/*
		 * This board's boot pipeline (amonet-patched LK reading the
		 * appended DTB) has been confirmed, via extensive testing, to
		 * hand the kernel a stale devicetree that never reflects
		 * source changes - including this very state definition,
		 * which is present in source/compiled .dtb/packaged boot.img
		 * but never reaches /sys/firmware/devicetree/base, even
		 * across a genuine cold power cycle. Root-caused to something
		 * in the bootloader's DTB-loading path, not fixable from
		 * kernel/DTS alone without UART access or reflashing LK
		 * (protected/high-risk). Image.gz itself IS reliably picked
		 * up every boot (unlike the DTB), so fall back to writing the
		 * exact register the pinctrl driver's own mtk_pmx_set_mode()
		 * would have written, computed from
		 * drivers/pinctrl/mediatek/pinctrl-mtk-common.c's formula
		 * (mt8163_pinctrl_data: mode_per_reg=5, port_shf=4,
		 * pinmux_offset=0x600, GPIO_MODE_BITS=3) for pin 85
		 * (MSDC2_CMD -> ANT_SEL0, mode 2) and pin 86 (MSDC2_CLK ->
		 * ANT_SEL1, mode 2), both pins 85/86 packed into the same
		 * register at GPIO base + 0x710:
		 *   bits[2:0] = pin85 mode, bits[5:3] = pin86 mode.
		 */
		{
			/*
			 * A plain ioremap() of 0x1000b000 reads back 0 even for
			 * registers we know are live (verified against pin 79's
			 * UART mux, which the console depends on) - this region
			 * is already exclusively owned by the "pio" pinctrl
			 * driver (visible in /proc/iomem), and a second,
			 * independent mapping to the same physical page doesn't
			 * coherently observe/apply writes made through the
			 * driver's own mapping (or vice versa). The pinctrl-mtk
			 * driver itself talks to this block via
			 * syscon_node_to_regmap() - a publicly exported,
			 * per-node-cached lookup - so calling it ourselves on
			 * the same device node gets the exact same regmap
			 * instance instead of a conflicting alias.
			 */
			struct device_node *pio_np =
				of_find_compatible_node(NULL, NULL, "mediatek,mt8163-pinctrl");
			struct device_node *regmap_np;
			struct regmap *pio_regmap;

			if (!pio_np) {
				WMT_PLAT_ERR_FUNC("ANT_SEL0/1 fallback: pinctrl node not found\n");
				goto set_pin_exit;
			}
			/*
			 * The pio node isn't itself a syscon provider - it
			 * points to the real one via its own
			 * "mediatek,pctl-regmap" phandle (matching how
			 * mtk_pctrl_init() in pinctrl-mtk-common.c obtains
			 * pctl->regmap1), which is what actually backs the
			 * register writes mtk_pmx_set_mode() performs.
			 */
			regmap_np = of_parse_phandle(pio_np, "mediatek,pctl-regmap", 0);
			of_node_put(pio_np);
			if (!regmap_np) {
				WMT_PLAT_ERR_FUNC("ANT_SEL0/1 fallback: pctl-regmap phandle not found\n");
				goto set_pin_exit;
			}
			pio_regmap = syscon_node_to_regmap(regmap_np);
			of_node_put(regmap_np);
			if (IS_ERR(pio_regmap)) {
				WMT_PLAT_ERR_FUNC("ANT_SEL0/1 fallback: syscon_node_to_regmap failed(%ld)\n",
						  PTR_ERR(pio_regmap));
				goto set_pin_exit;
			}
			regmap_update_bits(pio_regmap, 0x710, 0x3Fu, 0x12u);
			{
				u32 verify = 0;

				regmap_read(pio_regmap, 0x710, &verify);
				WMT_PLAT_ERR_FUNC("Applied ANT_SEL0/1 pinmux fallback via syscon regmap (0x710=0x%08x)\n",
						  verify);
			}
		}
		goto set_pin_exit;
	}
	pinctrl_select_state(mt6625_spi_pinctrl, mt6625_spi_default);

	/*
	 * Verify the ANT_SEL mux actually took, and program it if not.
	 *
	 * pinctrl_lookup_state() and pinctrl_select_state() both succeed here -
	 * no error is logged - yet the mux register reads back as function 0
	 * (plain GPIO) for pins 85/86 instead of function 2 (ANT_SEL0/ANT_SEL1).
	 * Measured on hardware: 0x10005710 == 0x00000000, where 0x12 is expected
	 * (pin 85 in bits[2:0], pin 86 in bits[5:3], mode 2 each).
	 *
	 * With those pins left as GPIO the chip's coex block cannot drive the
	 * 2.4GHz antenna switch, so WiFi is never granted that antenna. That
	 * matches the symptom exactly: 2.4GHz returns zero BSSes on every scan
	 * while 5GHz - which does not use this switch - works, and BT, which
	 * holds the default path, receives fine at -45 dBm.
	 *
	 * The existing fallback below does the same write but only when the
	 * pinctrl *lookup* fails, which is not what happens. Check the register
	 * itself rather than trusting the return codes, and do it here so the
	 * mux is correct before the chip is powered on and latches its antenna
	 * configuration.
	 */
	{
		struct device_node *pio_np =
			of_find_compatible_node(NULL, NULL, "mediatek,mt8163-pinctrl");
		struct device_node *regmap_np;
		struct regmap *pio_regmap;
		u32 val = 0;

		if (pio_np) {
			regmap_np = of_parse_phandle(pio_np, "mediatek,pctl-regmap", 0);
			if (regmap_np) {
				pio_regmap = syscon_node_to_regmap(regmap_np);
				of_node_put(regmap_np);
				if (!IS_ERR(pio_regmap)) {
					regmap_read(pio_regmap, 0x710, &val);
					if ((val & 0x3Fu) != 0x12u) {
						regmap_update_bits(pio_regmap, 0x710, 0x3Fu, 0x12u);
						regmap_read(pio_regmap, 0x710, &val);
						WMT_PLAT_ERR_FUNC("biscuit-antsel: mux was wrong, programmed ANT_SEL0/1 (0x710=0x%08x)\n",
								  val);
					} else {
						WMT_PLAT_INFO_FUNC("biscuit-antsel: ANT_SEL0/1 mux already correct (0x%08x)\n",
								   val);
					}
				}
			}
			of_node_put(pio_np);
		}
	}
set_pin_exit:

	/*
	 * Get a regmap for the MT6323 PMIC.
	 *
	 * The vendor original used pwrap_node_to_regmap(), a
	 * MediaTek-downstream-only helper that does not exist upstream. The
	 * mainline equivalent: the PMIC wrapper driver
	 * (drivers/soc/mediatek/mtk-pmic-wrap.c) registers a regmap on its
	 * own device, and mt6397-core fetches it with dev_get_regmap() on the
	 * parent - so we can do exactly the same by looking the pwrap device
	 * up from its DT node.
	 *
	 * This matters: Amazon's driver calls upmu_set_vcn_1v8_lp_mode_set(0)
	 * to take the connsys 1.8V rail out of low-power mode before the MCU
	 * clock ramp. There is no mainline regulator API for that -
	 * regulator_set_mode() on this rail is rejected outright by the
	 * MT6323 regulator driver ("vcn18: mode operation not allowed" in
	 * dmesg), because MT6323 LDOs implement no .set_mode op. A direct
	 * register write through this regmap is the only way to reproduce it.
	 */
	node = of_find_compatible_node(NULL, NULL, "mediatek,mt8163-pwrap");
	if (node) {
		struct platform_device *pwrap_pdev = of_find_device_by_node(node);

		if (pwrap_pdev) {
			pmic_regmap = dev_get_regmap(&pwrap_pdev->dev, NULL);
			WMT_PLAT_ERR_FUNC("biscuit-pmic: regmap=%p (NULL means the raw MT6323 pokes are all skipped)\n",
					  pmic_regmap);
			if (!pmic_regmap)
				WMT_PLAT_ERR_FUNC("pwrap has no regmap yet\n");
			else
				WMT_PLAT_INFO_FUNC("got PMIC regmap via pwrap\n");
		} else {
			WMT_PLAT_ERR_FUNC("pwrap device not found for node\n");
		}
		of_node_put(node);
	} else {
		WMT_PLAT_ERR_FUNC("no mediatek,mt8163-pwrap node\n");
	}

	/*
	 * Amazon's own 3.18 consys node has no "clocks"/"clock-names"
	 * property at all - the connsys bus is gated by the CONN power
	 * domain (power-domains = <&spm MT8163_POWER_DOMAIN_CONN> above),
	 * not a separate CCF gate, and mt8163-infracfg.h exposes no
	 * "consysbus"-shaped clock to reference even if we wanted one. This
	 * devm_clk_get() call looks like it was carried over from a newer
	 * MTK SoC's consys driver during the port to modern CCF-based
	 * clocks. Treat it as optional rather than failing the whole probe:
	 * clk_prepare_enable()/clk_disable_unprepare() on a NULL clk are
	 * defined no-ops, so every later use of clk_infra_conn_main stays
	 * safe if this is genuinely absent on this SoC.
	 */
	clk_infra_conn_main = devm_clk_get_optional(&pdev->dev, "consysbus");
	if (IS_ERR(clk_infra_conn_main)) {
		WMT_PLAT_ERR_FUNC("[CCF]cannot get consysbus clock: %ld\n",
				  PTR_ERR(clk_infra_conn_main));
		return PTR_ERR(clk_infra_conn_main);
	}
	WMT_PLAT_DBG_FUNC("[CCF]clk_infra_conn_main=%p\n", clk_infra_conn_main);

	reg_VCN18 = devm_regulator_get(&pdev->dev, "vcn18");
	if (IS_ERR(reg_VCN18)) {
		ret = PTR_ERR(reg_VCN18);
		WMT_PLAT_ERR_FUNC("Regulator_get VCN_1V8 fail, ret=%d\n", ret);
	}
	reg_VCN28 = devm_regulator_get(&pdev->dev, "vcn28");
	if (IS_ERR(reg_VCN28)) {
		ret = PTR_ERR(reg_VCN28);
		WMT_PLAT_ERR_FUNC("Regulator_get VCN_2V8 fail, ret=%d\n", ret);
	}
	reg_VCN33_BT = devm_regulator_get(&pdev->dev, "vcn33_bt");
	if (IS_ERR(reg_VCN33_BT)) {
		ret = PTR_ERR(reg_VCN33_BT);
		WMT_PLAT_ERR_FUNC("Regulator_get VCN33_BT fail, ret=%d\n", ret);
	}
	reg_VCN33_WIFI = devm_regulator_get(&pdev->dev, "vcn33_wifi");
	if (IS_ERR(reg_VCN33_WIFI)) {
		ret = PTR_ERR(reg_VCN33_WIFI);
		WMT_PLAT_ERR_FUNC("Regulator_get VCN33_WIFI fail, ret=%d\n", ret);
	}

	rstc = devm_reset_control_get(&pdev->dev, "connsys");
	if (IS_ERR(rstc)) {
		ret = PTR_ERR(rstc);
		WMT_PLAT_ERR_FUNC("CanNot get consys reset. ret=%d\n", ret);
		return PTR_ERR(rstc);
	}

	consys_pinctrl = devm_pinctrl_get(&pdev->dev);
	if (IS_ERR(consys_pinctrl)) {
		ret = PTR_ERR(consys_pinctrl);
		WMT_PLAT_ERR_FUNC("CanNot find consys pinctrl. ret=%d\n", ret);
		return PTR_ERR(consys_pinctrl);
	}
	return 0;
}

static VOID mtk_wmt_remove(struct platform_device *pdev)
{
printk(KERN_ALERT "PMRT: mtk_wmt_remove() called! usage_count=%d disable_depth=%d runtime_status=%d\n",
	atomic_read(&pdev->dev.power.usage_count),
	pdev->dev.power.disable_depth,
	pdev->dev.power.runtime_status);
	pm_runtime_disable(&pdev->dev);
}

VOID mtk_wcn_consys_power_on(VOID)
{
	INT32 iRet = -1;
printk(KERN_ALERT "DEBUG: Passed %s %d \n",__FUNCTION__,__LINE__);
printk(KERN_ALERT "PMRT: pre get_sync usage_count=%d disable_depth=%d runtime_status=%d\n",
	atomic_read(&my_pdev->dev.power.usage_count),
	my_pdev->dev.power.disable_depth,
	my_pdev->dev.power.runtime_status);
	iRet = pm_runtime_get_sync(&my_pdev->dev);
printk(KERN_ALERT "PMRT: post get_sync ret=%d usage_count=%d runtime_status=%d\n",
	iRet, atomic_read(&my_pdev->dev.power.usage_count),
	my_pdev->dev.power.runtime_status);
printk(KERN_ALERT "DEBUG: Passed %s %d \n",__FUNCTION__,__LINE__);
	if (iRet)
		WMT_PLAT_ERR_FUNC("pm_runtime_get_sync() fail(%d)\n", iRet);
	else
		WMT_PLAT_INFO_FUNC("pm_runtime_get_sync() CONSYS ok\n");

	iRet = device_init_wakeup(&my_pdev->dev, true);
printk(KERN_ALERT "DEBUG: Passed %s %d \n",__FUNCTION__,__LINE__);
	if (iRet)
		WMT_PLAT_ERR_FUNC("device_init_wakeup(true) fail.\n");
	else
		WMT_PLAT_INFO_FUNC("device_init_wakeup(true) CONSYS ok\n");
}

VOID mtk_wcn_consys_power_off(VOID)
{
	INT32 iRet = -1;

printk(KERN_ALERT "PMRT: pre put_sync usage_count=%d disable_depth=%d runtime_status=%d\n",
	atomic_read(&my_pdev->dev.power.usage_count),
	my_pdev->dev.power.disable_depth,
	my_pdev->dev.power.runtime_status);
	iRet = pm_runtime_put_sync(&my_pdev->dev);
printk(KERN_ALERT "PMRT: post put_sync ret=%d usage_count=%d runtime_status=%d\n",
	iRet, atomic_read(&my_pdev->dev.power.usage_count),
	my_pdev->dev.power.runtime_status);
	if (iRet)
		WMT_PLAT_ERR_FUNC("pm_runtime_put_sync() fail.\n");
	else
		WMT_PLAT_INFO_FUNC("pm_runtime_put_sync() CONSYS ok\n");

	iRet = device_init_wakeup(&my_pdev->dev, false);
	if (iRet)
		WMT_PLAT_ERR_FUNC("device_init_wakeup(false) fail.\n");
	else
		WMT_PLAT_INFO_FUNC("device_init_wakeup(false) CONSYS ok\n");
}

INT32 mtk_wcn_consys_hw_reg_ctrl(UINT32 on, UINT32 co_clock_type)
{
	UINT32 retry = 10;
	UINT32 consysHwChipId = 0;

	WMT_PLAT_DBG_FUNC("CONSYS-HW-REG-CTRL(0x%08x),start\n", on);
	if (on) {
		WMT_PLAT_DBG_FUNC("++\n");
printk(KERN_ALERT "DEBUG: Passed %s %d \n",__FUNCTION__,__LINE__);
		/*need PMIC driver provide new API protocol */
		/*
		 * 1.AP power on VCN_1V8 LDO (with PMIC_WRAP API) VCN_1V8
		 *
		 * MUST be NORMAL, not STANDBY. Amazon's own 3.18 driver calls
		 * upmu_set_vcn_1v8_lp_mode_set(0) here, i.e. it *disables* the
		 * rail's low-power mode on the way up (that helper writes its
		 * argument straight into VCN_1V8_LP_MODE_SET, so 0 = LP off).
		 * The port translated that to REGULATOR_MODE_STANDBY, which is
		 * the *lowest*-drive mode in the Linux regulator API - exactly
		 * inverted.
		 *
		 * VCN18 is the connsys 1.8V rail, and the largest current step
		 * in the whole bring-up is the MCU ramp from 26MHz to
		 * 138.67MHz - which is precisely where this chip has been
		 * asserting. A rail pinned in low-power drive across that ramp
		 * is a textbook cause of a droop-triggered firmware assert.
		 */
		/*
		 * Equivalent of Amazon's upmu_set_vcn_1v8_lp_mode_set(0):
		 * clear VCN_1V8_LP_MODE_SET (DIGLDO_CON11 bit 1) so the rail is
		 * NOT in low-power mode while the connsys MCU ramps to
		 * 138.67MHz - the largest current step in the whole bring-up.
		 *
		 * regulator_set_mode() cannot express this: MT6323's regulator
		 * driver implements no .set_mode, so the call was rejected with
		 * "vcn18: mode operation not allowed" and silently did nothing.
		 */
		if (pmic_regmap) {
			int lpret = regmap_update_bits(pmic_regmap, MT6323_DIGLDO_CON11,
						       0x1 << 1, 0);
			if (lpret)
				WMT_PLAT_ERR_FUNC("VCN18 LP-mode clear failed (%d)\n", lpret);
			else
				WMT_PLAT_INFO_FUNC("VCN18 low-power mode disabled\n");
		} else {
			WMT_PLAT_ERR_FUNC("no PMIC regmap - VCN18 stays in whatever mode it booted in\n");
		}
printk(KERN_ALERT "DEBUG: Passed %s %d \n",__FUNCTION__,__LINE__);
		/* VOL_DEFAULT, VOL_1200, VOL_1300, VOL_1500, VOL_1800... */
		if (reg_VCN18) {
printk(KERN_ALERT "DEBUG: Passed %s %d \n",__FUNCTION__,__LINE__);
			regulator_set_voltage(reg_VCN18, 1800000, 1800000);
printk(KERN_ALERT "DEBUG: Passed %s %d \n",__FUNCTION__,__LINE__);
			if (regulator_enable(reg_VCN18))
				WMT_PLAT_ERR_FUNC("enable VCN18 fail\n");
			else
				WMT_PLAT_DBG_FUNC("enable VCN18 ok\n");
		}
		udelay(150);
printk(KERN_ALERT "DEBUG: Passed %s %d \n",__FUNCTION__,__LINE__);
		if (co_clock_type) {
			/*step0,clk buf ctrl */
			WMT_PLAT_INFO_FUNC("co clock type(%d),turn on clk buf\n", co_clock_type);
#if CONSYS_CLOCK_BUF_CTRL
			clk_buf_ctrl(CLK_BUF_CONN, 1);
#endif
			/*if co-clock mode: */
			/*2.set VCN28 to SW control mode (with PMIC_WRAP API) */
			/*turn on VCN28 LDO only when FMSYS is activated"  */
			if (pmic_regmap)
				regmap_update_bits(pmic_regmap, 0x41C, 0x1 << 14, 0x0 << 14);/*V28*/
		} else {
			/*if NOT co-clock: */
			/*2.1.switch VCN28 to HW control mode (with PMIC_WRAP API) */
			if (pmic_regmap)
				regmap_update_bits(pmic_regmap, 0x41C, 0x1 << 14, 0x1 << 14);/*V28*/
			/*2.2.turn on VCN28 LDO (with PMIC_WRAP API)" */
			/*fix vcn28 not balance warning */
			if (reg_VCN28) {
				regulator_set_voltage(reg_VCN28, 2800000, 2800000);
				if (regulator_enable(reg_VCN28))
					WMT_PLAT_ERR_FUNC("enable VCN_2V8 fail!\n");
				else
					WMT_PLAT_DBG_FUNC("enable VCN_2V8 ok\n");
			}
		}

		/*
		 * 3. Assert CONNSYS CPU SW reset and HOLD it across the whole
		 * power-up, deasserting only at the very end (step 16 below).
		 *
		 * This used to be reset_control_reset(), which on the TOPRGU
		 * controller is assert-immediately-followed-by-deassert with no
		 * delay (see toprgu_reset() in drivers/watchdog/mtk_wdt.c) - so
		 * the CONNSYS MCU was released from reset *before* its power
		 * domain came up, before the 26M settle delay, before the
		 * chip-id poll, and was never deasserted at the end at all.
		 * Amazon's driver brackets the entire sequence with the reset
		 * held, which is what the numbered step comments here always
		 * described.
		 */
printk(KERN_ALERT "DEBUG: Passed %s %d \n",__FUNCTION__,__LINE__);
		reset_control_assert(rstc);
printk(KERN_ALERT "DEBUG: Passed %s %d \n",__FUNCTION__,__LINE__);
		mtk_wcn_consys_power_on();
printk(KERN_ALERT "DEBUG: Passed %s %d \n",__FUNCTION__,__LINE__);

		/*
		 * Disable AXI bus protection for CONNSYS.
		 *
		 * This step was missing entirely from this port - the offsets
		 * were defined in the header but nothing ever wrote them.
		 *
		 * While these bits are set the bus fabric fences CONNSYS off
		 * its AXI master port, so the chip cannot reach DRAM. That is
		 * silent in exactly the way we have been chasing: firmware
		 * downloads whose destination is EMI (0xf0000000 and up - two
		 * of the four sections of WIFI_RAM_CODE_8163, and the second
		 * ROM patch, whose header carries an 0xf00e..... address) are
		 * acknowledged by the chip with status 0 and then land nowhere.
		 * Sweeping the whole 2MB reserved region afterwards finds no
		 * trace of them.
		 *
		 * Poll STA1 until the protection has actually been released,
		 * as the vendor driver does - the disable is not instantaneous.
		 */
		WMT_PLAT_DBG_FUNC("disabling CONNSYS AXI bus protection\n");
		CONSYS_REG_WRITE(conn_reg.topckgen_base + CONSYS_TOPAXI_PROT_EN_OFFSET,
				 CONSYS_REG_READ(conn_reg.topckgen_base + CONSYS_TOPAXI_PROT_EN_OFFSET) &
				 ~CONSYS_PROT_MASK);
		{
			UINT32 u4ProtRetry = 100000;

			while ((CONSYS_REG_READ(conn_reg.topckgen_base + CONSYS_TOPAXI_PROT_STA1_OFFSET)
				& CONSYS_PROT_MASK) && u4ProtRetry--)
				udelay(1);
			WMT_PLAT_INFO_FUNC("biscuit-topaxi: PROT_EN=0x%08x STA1=0x%08x (mask 0x%x)%s\n",
					   CONSYS_REG_READ(conn_reg.topckgen_base +
							   CONSYS_TOPAXI_PROT_EN_OFFSET),
					   CONSYS_REG_READ(conn_reg.topckgen_base +
							   CONSYS_TOPAXI_PROT_STA1_OFFSET),
					   (UINT32) CONSYS_PROT_MASK,
					   u4ProtRetry ? "" : " *** TIMED OUT ***");
		}

		/*
		 * EMI MPU region 12: let CONNSYS reach the 512KB of DRAM the
		 * WiFi firmware lives in.
		 *
		 * The chip can plainly READ that memory - once the sections are
		 * put there by hand the MCU leaves ROM and executes from
		 * 0xf000.... - but it never WRITES it: sweeping the whole 2MB
		 * after a download finds no trace of the two EMI-destined
		 * sections, even though the chip acknowledges each one with
		 * status 0. A read-allowed / write-denied asymmetry is exactly
		 * what the EMI MPU expresses, and Amazon configures region 12
		 * over precisely this range in their ahb_pdma.c, which mainline
		 * has no driver for and nothing else sets up.
		 *
		 * On MT8163 these registers are written directly - the SMC path
		 * in emi_reg_rw.c is a different implementation that this SoC
		 * does not use - so no secure call is needed. EMI is at
		 * 0x10203000; addresses are 64KB units relative to the base of
		 * DRAM, and each of the 8 domains gets 3 permission bits.
		 *
		 * Amazon forbids everything except domains 2 and 7. We keep
		 * domain 0 open as well: the AP has to be able to touch this
		 * region too, both for the host-side section fill and to read
		 * the area back when checking what landed.
		 */
		{
			void __iomem *pucEmiReg = ioremap(0x10203000, 0x1000);

			if (pucEmiReg) {
				UINT32 u4Start = (UINT32) ((gConEmiPhyBase - 0x40000000) >> 16);
				/*
				 * Cover the WHOLE 2MB the DT reserves, not just the
				 * 512KB the WiFi firmware image occupies.
				 *
				 * This was the bug.  512KB covers the image
				 * (sections land at +0x6000..+0x63010) but stops
				 * dead at +0x80000 - and +0x80000 is exactly where
                                 * the CONNSYS EXP_APMEM control block lives
				 * (CONSYS_EMI_FW_PHY_BASE 0xf0080000), with the
				 * paged dump at +0x88400 and the full dump at
				 * +0x90400 beyond it.
				 *
				 * So the chip could place and fetch its firmware
				 * but faulted the instant that firmware touched the
				 * shared block.  Measured, from the chip's own core
				 * dump once the stale copy was wiped:
				 *     ; exception type: ABT
				 *     R.S IPC 0xF0080024
				 * 0xF0080024 is EXP_APMEM_CTRL_CHIP_PRINT_BUFF_IDX,
				 * i.e. +0x80000 + 0x24 - 0x24 bytes past the end of
				 * the protected window.  The MCU took a bus abort,
				 * reset, and landed back in ROM, which is what
				 * "asser_type=4 / exp_main: maybe jump from RST"
				 * had been reporting all along.
				 *
				 * An earlier experiment "opened all eight domains
				 * and nothing changed" - that varied the PERMISSION
				 * bits but never this range, so it could not have
				 * found it.
				 */
				UINT32 u4End =
				    (UINT32) ((gConEmiPhyBase + 2 * 1024 * 1024 - 1 - 0x40000000) >> 16);
				/* d7..d0, 3 bits each; 0 = NO_PROTECTION, 5 = FORBIDDEN */
				/*
				 * Open this region to every domain.
				 *
				 * The bootloader already programs region 12
				 * over exactly this range, granting domains 0,
				 * 2 and 7 and forbidding the rest - and that
				 * was assumed to be sufficient because the chip
				 * demonstrably writes here: the download engine
				 * places the whole firmware image.
				 *
				 * But the engine that writes the image and the
				 * one that later executes it are different bus
				 * masters. The CONNSYS MCU does the download;
				 * the WiFi MAC is what runs the firmware, and
				 * ~92% of that firmware lives in this DRAM
				 * rather than inside the chip. If the MAC sits
				 * in one of the forbidden domains it can place
				 * its code and then be unable to fetch it,
				 * which is what we see: the firmware starts,
				 * writes "INIT" into its mailbox, and stops.
				 *
				 * Tried opening all eight domains (0 =
				 * NO_PROTECTION everywhere) on hardware: the
				 * registers read back 0 as asked and the
				 * firmware still stopped in the same place.
				 * That ruled out the permission bits but NOT
				 * the MPU - the address window above was still
				 * only 512KB, so every domain was being granted
				 * access to a region that ended 0x24 bytes
				 * before the address the firmware faults on.
				 * Keep Amazon's permissions; the range is the
				 * part that was wrong.
				 */
				/*
				 * EXPERIMENT (biscuit): open every domain.
				 *
				 * Re-run of a test that was previously called
				 * negative, but that verdict was made against
				 * STALE DRAM - the reserved region is no-map and
				 * survives warm reboots, so leftover firmware
				 * bytes from an earlier boot looked like a
				 * successful download. With the region wiped
				 * first, the real picture is that sections 2 and
				 * 3 land NOWHERE (0 bytes, 100% zero blocks),
				 * while the chip's core dump - which lands
				 * OUTSIDE the old 512KB protected window - writes
				 * fine. That asymmetry points straight at these
				 * permission bits, so measure it properly now
				 * that the measurement is trustworthy.
				 */
				UINT32 u4Low = (5 << 9) | (0 << 6) | (5 << 3) | 0;	/* d3,d2,d1,d0 */
				UINT32 u4High = (0 << 9) | (5 << 6) | (5 << 3) | 5;	/* d7,d6,d5,d4 */
				UINT32 u4Tmp, u4Tmp2;

				WMT_PLAT_INFO_FUNC("biscuit-emimpu: before: MPUE2=0x%08x MPUK2=0x%08x MPUK2_2ND=0x%08x\n",
						   readl(pucEmiReg + 0x0280),
						   readl(pucEmiReg + 0x02b0),
						   readl(pucEmiReg + 0x02b4));

				u4Tmp = readl(pucEmiReg + 0x02b0) & 0xFFFF0000;
				u4Tmp2 = readl(pucEmiReg + 0x02b4) & 0xFFFF0000;
				/* clear access rights before moving the address window */
				writel(0, pucEmiReg + 0x02b0);
				writel(0, pucEmiReg + 0x02b4);
				writel((u4Start << 16) | u4End, pucEmiReg + 0x0280);
				writel(u4Tmp | u4Low, pucEmiReg + 0x02b0);
				writel(u4Tmp2 | u4High, pucEmiReg + 0x02b4);

				WMT_PLAT_INFO_FUNC("biscuit-emimpu: after:  MPUE2=0x%08x MPUK2=0x%08x MPUK2_2ND=0x%08x (range 0x%x-0x%x)\n",
						   readl(pucEmiReg + 0x0280),
						   readl(pucEmiReg + 0x02b0),
						   readl(pucEmiReg + 0x02b4), u4Start, u4End);
				iounmap(pucEmiReg);
			} else {
				WMT_PLAT_ERR_FUNC("biscuit-emimpu: ioremap of EMI failed\n");
			}
		}

		/*
		 * biscuit: wipe the whole reserved region ONCE, here, before
		 * anything is downloaded into it.
		 *
		 * This DRAM is no-map and survives warm reboots, so bytes left
		 * by a previous boot are indistinguishable from bytes the chip
		 * just wrote.  That confound produced two wrong conclusions:
		 * the WiFi firmware sections were reported as 88%/50% resident
		 * when in fact they were leftovers from the long-retired
		 * BISCUIT_EMI_HOST_FILL experiment, and an "MPU permissions
		 * make no difference" verdict was reached against the same
		 * stale bytes.
		 *
		 * It has to be here rather than in wlanAdapterStart(): that
		 * runs long after the WMT ROM patches are downloaded, and ROM
		 * patch _1_1 goes to chip address 0xf00e0000 = +0x0e0000, so a
		 * late wipe erases a patch the chip needs at runtime.
		 *
		 * With this in place, anything non-zero in the region afterwards
		 * was demonstrably written by the chip on this boot.
		 */
		if (gConEmiPhyBase) {
			void __iomem *pucAll = ioremap(gConEmiPhyBase, 2 * 1024 * 1024);

			if (pucAll) {
				/*
				 * Before zeroing, prove the AP can write and read
				 * back this DRAM reliably.
				 *
				 * The chip's own core-dump text arrives with 32-byte
				 * holes punched through it, and those holes land in
				 * DIFFERENT places on every boot (14 of 32 blocks
				 * differ between two runs) - so writes into this
				 * region are being lost, not merely unwritten. That
				 * would also corrupt the firmware's own EMI data
				 * (it stores to 0xf0063xxx), which is reason enough
				 * to crash. This says whether the loss is on the AP
				 * side (DRAM / reservation / mapping) or the chip's.
				 */
				UINT32 u4Off, u4Bad = 0, u4FirstBad = 0xffffffff;

				/*
				 * RESULT (kept for the record): bad=0/524288 words
				 * over the full 2MB. The AP can write and read this
				 * DRAM perfectly, so the reservation, the mapping and
				 * the memory itself are all sound - the 32-byte holes
				 * in the chip's core dump are on the chip's side.
				 * They are almost certainly MCU cache lines that were
				 * never written back rather than lost transactions,
				 * since the firmware DOWNLOAD - also a chip write to
				 * EMI, but via the download engine - lands with zero
				 * holes (sec2: 0/8078 empty blocks).
				 *
				 * Off by default; set biscuit_emitest=1 to re-run.
				 */
				if (!biscuit_emitest)
					goto skip_emitest;

				for (u4Off = 0; u4Off < 2 * 1024 * 1024; u4Off += 4)
					writel(0xA5000000u | u4Off, pucAll + u4Off);
				/* make sure nothing is sitting in a write buffer */
				wmb();
				for (u4Off = 0; u4Off < 2 * 1024 * 1024; u4Off += 4) {
					if (readl(pucAll + u4Off) != (0xA5000000u | u4Off)) {
						if (u4FirstBad == 0xffffffff)
							u4FirstBad = u4Off;
						u4Bad++;
					}
				}
				WMT_PLAT_INFO_FUNC("biscuit-emitest: AP write/readback bad=%u/%u words, first bad +0x%x\n",
						   u4Bad, (UINT32)(2 * 1024 * 1024 / 4), u4FirstBad);
skip_emitest:

				memset_io(pucAll, 0, 2 * 1024 * 1024);
				iounmap(pucAll);
				WMT_PLAT_INFO_FUNC("biscuit-emiwipe: zeroed all 2MB at 0x%llx before download\n",
						   (unsigned long long)gConEmiPhyBase);
			} else {
				WMT_PLAT_ERR_FUNC("biscuit-emiwipe: ioremap failed\n");
			}
		}

		/*11.26M is ready now, delay 10us for mem_pd de-assert */
		udelay(10);
		/*enable AP bus clock : connmcu_bus_pd  API: enable_clock() ++?? */
		clk_prepare_enable(clk_infra_conn_main);
printk(KERN_ALERT "DEBUG: Passed %s %d \n",__FUNCTION__,__LINE__);
		WMT_PLAT_DBG_FUNC("[CCF]enable clk_infra_conn_main\n");
		/*12.poll CONNSYS CHIP ID until chipid is returned  0x18070008 */
		while (retry-- > 0) {
			/*
			 * No "-0xf6d" offset here, unlike the vendor original:
			 * on this board's silicon, this register already reads
			 * back the plain SoC ID (0x8163, matched below) - that
			 * offset was tuned for a different chip stepping and
			 * was turning a valid 0x8163 readback into a bogus
			 * 0x71f6 that never matched any accepted chip ID,
			 * confirmed by reversing the math on the resulting
			 * "Read CONSYS chipId(0x71f6)" failure log.
			 */
			consysHwChipId = CONSYS_REG_READ(conn_reg.mcu_base + CONSYS_CHIP_ID_OFFSET);

			if ((consysHwChipId == 0x0321) || (consysHwChipId == 0x0335) || (consysHwChipId == 0x0337)) {
				WMT_PLAT_INFO_FUNC("retry(%d)consys chipId(0x%08x)\n", retry, consysHwChipId);
				break;
			}
			if ((consysHwChipId == 0x8163) || (consysHwChipId == 0x8127) || (consysHwChipId == 0x7623)) {
				WMT_PLAT_INFO_FUNC("retry(%d)consys chipId(0x%08x)\n", retry, consysHwChipId);
				break;
			}

			WMT_PLAT_ERR_FUNC("Read CONSYS chipId(0x%08x)", consysHwChipId);
			msleep(20);
		}

		if ((0 == retry) || (0 == consysHwChipId))
			WMT_PLAT_ERR_FUNC("Maybe has a consys power on issue,(0x%08x)\n", consysHwChipId);

		/*
		 * 14. Write 1 to conn_mcu_cfg ACR[18] (0x18070110) before the
		 * MCU is released from reset.
		 *
		 * Per Amazon's own comment on this register: with this bit
		 * clear the hardware only runs its memory auto-test at the low
		 * (26MHz) CPU frequency; set, it covers the high (138.67MHz)
		 * frequency too. This is the one register in the whole
		 * sequence whose documented purpose is conditioning the CONNSYS
		 * MCU's memories for high-frequency operation - and this port
		 * never wrote it (the macros existed in the header with zero
		 * call sites), while failing precisely at the 138.67MHz switch.
		 */
		CONSYS_REG_WRITE(conn_reg.mcu_base + CONSYS_MCU_CFG_ACR_OFFSET,
				 CONSYS_REG_READ(conn_reg.mcu_base + CONSYS_MCU_CFG_ACR_OFFSET) |
				 CONSYS_MCU_CFG_ACR_MBIST_BIT);
		WMT_PLAT_DBG_FUNC("MCU_CFG ACR now 0x%08x\n",
				  CONSYS_REG_READ(conn_reg.mcu_base + CONSYS_MCU_CFG_ACR_OFFSET));

		/*16.deassert CONNSYS CPU SW reset - MCU starts running here */
		reset_control_deassert(rstc);
		msleep(20);

		msleep(40);

	} else {

		/*
		 * Put the CONNSYS MCU back into reset FIRST.
		 *
		 * The power-up path holds this reset asserted across the whole
		 * sequence and deasserts it at the very end (step 16), but the
		 * power-down path never asserted it again - so the MCU was left
		 * running while its bus clock was unprepared and its rails were
		 * dropped underneath it.  Stopping the core before removing what
		 * it runs on is the ordinary inverse of the power-up sequence,
		 * and it is what makes an off/on cycle repeatable.
		 */
		reset_control_assert(rstc);
		udelay(100);

		clk_disable_unprepare(clk_infra_conn_main);
		WMT_PLAT_DBG_FUNC("[CCF] clk_disable_unprepare(clk_infra_conn_main) calling\n");
		mtk_wcn_consys_power_off();

		if (co_clock_type) {
			/*VCN28 has been turned off by GPS OR FM */
#if CONSYS_CLOCK_BUF_CTRL
			clk_buf_ctrl(CLK_BUF_CONN, 0);
#endif
		} else {
			if (pmic_regmap)
				regmap_update_bits(pmic_regmap, 0x41C, 0x1 << 14, 0x0 << 14);/*V28*/
			/*turn off VCN28 LDO (with PMIC_WRAP API)" */
			if (reg_VCN28) {
				if (regulator_disable(reg_VCN28))
					WMT_PLAT_ERR_FUNC("disable VCN_2V8 fail!\n");
				else
					WMT_PLAT_DBG_FUNC("disable VCN_2V8 ok\n");
			}
		}

		/*
		 * AP power off MT6625L VCN_1V8 LDO.
		 *
		 * The regulator_set_mode(REGULATOR_MODE_STANDBY) that used to
		 * sit here is gone.  It dereferenced reg_VCN18 outside the NULL
		 * check immediately below it, MT6323's regulator driver has no
		 * .set_mode so the call could only ever fail, and STANDBY is the
		 * inverse of what the vendor driver does to this rail anyway
		 * (see the LP-mode comment in the power-up path above).
		 */
		if (reg_VCN18) {
			if (regulator_disable(reg_VCN18))
				WMT_PLAT_ERR_FUNC("disable VCN_1V8 fail!\n");
			else
				WMT_PLAT_DBG_FUNC("disable VCN_1V8 ok\n");
		}

		/*
		 * Report what the rails actually did, and give them time to
		 * discharge before anyone powers the chip back up.
		 *
		 * These two lines are the whole point of the change: while the
		 * rails carried "regulator-always-on" in the device tree, every
		 * regulator_disable() above returned success and left the rail
		 * up, so a "power cycle" was nothing of the sort.  Print the
		 * post-disable state so that is visible rather than assumed.
		 */
		WMT_PLAT_INFO_FUNC("biscuit-pwr: after off: VCN18=%d VCN28=%d VCN33_BT=%d VCN33_WIFI=%d (1 = still enabled)\n",
				   reg_VCN18 ? regulator_is_enabled(reg_VCN18) : -1,
				   reg_VCN28 ? regulator_is_enabled(reg_VCN28) : -1,
				   reg_VCN33_BT ? regulator_is_enabled(reg_VCN33_BT) : -1,
				   reg_VCN33_WIFI ? regulator_is_enabled(reg_VCN33_WIFI) : -1);
		msleep(100);

	}
	WMT_PLAT_DBG_FUNC("CONSYS-HW-REG-CTRL(0x%08x),finish\n", on);
	return 0;
}

INT32 mtk_wcn_consys_hw_gpio_ctrl(UINT32 on)
{
	INT32 iRet = 0;

	WMT_PLAT_DBG_FUNC("CONSYS-HW-GPIO-CTRL(0x%08x), start\n", on);

	if (on) {

		/* TODO: [FixMe][GeorgeKuo] double check if BGF_INT is implemented ok */
		/* iRet += wmt_plat_gpio_ctrl(PIN_BGF_EINT, PIN_STA_MUX); */
		iRet += wmt_plat_eirq_ctrl(PIN_BGF_EINT, PIN_STA_INIT);
		iRet += wmt_plat_eirq_ctrl(PIN_BGF_EINT, PIN_STA_EINT_DIS);
		WMT_PLAT_DBG_FUNC("CONSYS-HW, BGF IRQ registered and disabled\n");

	} else {

		/* set bgf eint/all eint to deinit state, namely input low state */
		iRet += wmt_plat_eirq_ctrl(PIN_BGF_EINT, PIN_STA_EINT_DIS);
		iRet += wmt_plat_eirq_ctrl(PIN_BGF_EINT, PIN_STA_DEINIT);
		WMT_PLAT_DBG_FUNC("CONSYS-HW, BGF IRQ unregistered and disabled\n");
		/* iRet += wmt_plat_gpio_ctrl(PIN_BGF_EINT, PIN_STA_DEINIT); */
	}
	WMT_PLAT_DBG_FUNC("CONSYS-HW-GPIO-CTRL(0x%08x), finish\n", on);
	return iRet;

}

INT32 mtk_wcn_consys_hw_pwr_on(UINT32 co_clock_type)
{
	INT32 iRet = 0;

	WMT_PLAT_INFO_FUNC("CONSYS-HW-PWR-ON, start\n");

	iRet += mtk_wcn_consys_hw_reg_ctrl(1, co_clock_type);
	iRet += mtk_wcn_consys_hw_gpio_ctrl(1);

	WMT_PLAT_INFO_FUNC("CONSYS-HW-PWR-ON, finish(%d)\n", iRet);
	return iRet;
}

INT32 mtk_wcn_consys_hw_pwr_off(VOID)
{
	INT32 iRet = 0;

	WMT_PLAT_INFO_FUNC("CONSYS-HW-PWR-OFF, start\n");

	iRet += mtk_wcn_consys_hw_reg_ctrl(0, 0);
	iRet += mtk_wcn_consys_hw_gpio_ctrl(0);

	WMT_PLAT_INFO_FUNC("CONSYS-HW-PWR-OFF, finish(%d)\n", iRet);
	return iRet;
}

INT32 mtk_wcn_consys_hw_rst(UINT32 co_clock_type)
{
	INT32 iRet = 0;

	WMT_PLAT_INFO_FUNC("CONSYS-HW, hw_rst start, eirq should be disabled before this step\n");

	/*1. do whole hw power off flow */
	iRet += mtk_wcn_consys_hw_reg_ctrl(0, co_clock_type);

	/*2. do whole hw power on flow */
	iRet += mtk_wcn_consys_hw_reg_ctrl(1, co_clock_type);

	WMT_PLAT_INFO_FUNC("CONSYS-HW, hw_rst finish, eirq should be enabled after this step\n");
	return iRet;
}

#if CONSYS_BT_WIFI_SHARE_V33
INT32 mtk_wcn_consys_hw_bt_paldo_ctrl(UINT32 enable)
{
	/* spin_lock_irqsave(&gBtWifiV33.lock,gBtWifiV33.flags); */
	if (enable) {
		if (1 == gBtWifiV33.counter) {
			gBtWifiV33.counter++;
			WMT_PLAT_DBG_FUNC("V33 has been enabled,counter(%d)\n", gBtWifiV33.counter);
		} else if (2 == gBtWifiV33.counter) {
			WMT_PLAT_DBG_FUNC("V33 has been enabled,counter(%d)\n", gBtWifiV33.counter);
		} else {
#if CONSYS_PMIC_CTRL_ENABLE
			/*do BT PMIC on,depenency PMIC API ready */
			/*switch BT PALDO control from SW mode to HW mode:0x416[5]-->0x1 */
			/* VOL_DEFAULT, VOL_3300, VOL_3400, VOL_3500, VOL_3600 */
			hwPowerOn(MT6323_POWER_LDO_VCN33, VOL_3300, "wcn_drv");
			upmu_set_vcn33_on_ctrl_bt(1);
#endif
			WMT_PLAT_INFO_FUNC("WMT do BT/WIFI v3.3 on\n");
			gBtWifiV33.counter++;
		}

	} else {
		if (1 == gBtWifiV33.counter) {
			/*do BT PMIC off */
			/*switch BT PALDO control from HW mode to SW mode:0x416[5]-->0x0 */
#if CONSYS_PMIC_CTRL_ENABLE
		    upmu_set_vcn33_on_ctrl_bt(0);
			hwPowerDown(MT6323_POWER_LDO_VCN33, "wcn_drv");
#endif
			WMT_PLAT_INFO_FUNC("WMT do BT/WIFI v3.3 off\n");
			gBtWifiV33.counter--;
		} else if (2 == gBtWifiV33.counter) {
			gBtWifiV33.counter--;
			WMT_PLAT_DBG_FUNC("V33 no need disabled,counter(%d)\n", gBtWifiV33.counter);
		} else {
			WMT_PLAT_DBG_FUNC("V33 has been disabled,counter(%d)\n", gBtWifiV33.counter);
		}

	}
	/* spin_unlock_irqrestore(&gBtWifiV33.lock,gBtWifiV33.flags); */
	return 0;
}

INT32 mtk_wcn_consys_hw_wifi_paldo_ctrl(UINT32 enable)
{
	mtk_wcn_consys_hw_bt_paldo_ctrl(enable);
	return 0;
}
EXPORT_SYMBOL(mtk_wcn_consys_hw_wifi_paldo_ctrl);

#else
INT32 mtk_wcn_consys_hw_bt_paldo_ctrl(UINT32 enable)
{

	if (enable) {
		/*do BT PMIC on,depenency PMIC API ready */
		/*switch BT PALDO control from SW mode to HW mode:0x416[5]-->0x1 */
		if (reg_VCN33_BT) {
			regulator_set_voltage(reg_VCN33_BT, 3300000, 3300000);
			if (regulator_enable(reg_VCN33_BT))
				WMT_PLAT_ERR_FUNC("WMT do BT PMIC on fail!\n");
		}
		if (pmic_regmap)
			regmap_update_bits(pmic_regmap, 0x416, 0x1 << 5, 0x1 << 5);/*BT*/
		WMT_PLAT_INFO_FUNC("WMT do BT PMIC on\n");
	} else {
		/*do BT PMIC off */
		/*switch BT PALDO control from HW mode to SW mode:0x416[5]-->0x0 */
		if (pmic_regmap)
			regmap_update_bits(pmic_regmap, 0x416, 0x1 << 5, 0x0 << 5);/*BT*/
		if (reg_VCN33_BT)
			if (regulator_disable(reg_VCN33_BT))
				WMT_PLAT_ERR_FUNC("WMT do BT PMIC off fail!\n");
		WMT_PLAT_INFO_FUNC("WMT do BT PMIC off\n");
	}

	return 0;

}

INT32 mtk_wcn_consys_hw_wifi_paldo_ctrl(UINT32 enable)
{

	if (enable) {
		/*do WIFI PMIC on,depenency PMIC API ready */
		/*switch WIFI PALDO control from SW mode to HW mode:0x418[14]-->0x1 */
		if (reg_VCN33_WIFI) {
			regulator_set_voltage(reg_VCN33_WIFI, 3300000, 3300000);
			if (regulator_enable(reg_VCN33_WIFI))
				WMT_PLAT_ERR_FUNC("WMT do WIFI PMIC on fail!\n");
			else
				WMT_PLAT_INFO_FUNC("WMT do WIFI PMIC on !\n");
		}
		if (pmic_regmap)
			regmap_update_bits(pmic_regmap, 0x418, 0x1 << 14, 0x1 << 14);/*WIFI*/
		WMT_PLAT_INFO_FUNC("WMT do WIFI PMIC on\n");
	} else {
		/*do WIFI PMIC off */
		/*switch WIFI PALDO control from HW mode to SW mode:0x418[14]-->0x0 */
		if (pmic_regmap)
			regmap_update_bits(pmic_regmap, 0x418, 0x1 << 14, 0x0 << 14);/*WIFI*/
		if (reg_VCN33_WIFI)
			if (regulator_disable(reg_VCN33_WIFI))
				WMT_PLAT_ERR_FUNC("WMT do WIFI PMIC off fail!\n");
		WMT_PLAT_INFO_FUNC("WMT do WIFI PMIC off\n");
	}

	return 0;
}
EXPORT_SYMBOL(mtk_wcn_consys_hw_wifi_paldo_ctrl);

#endif
INT32 mtk_wcn_consys_hw_vcn28_ctrl(UINT32 enable)
{
	if (enable) {
		/*in co-clock mode,need to turn on vcn28 when fm on */
		if (reg_VCN28) {
			regulator_set_voltage(reg_VCN28, 2800000, 2800000);
			if (regulator_enable(reg_VCN28))
				WMT_PLAT_ERR_FUNC("WMT do VCN28 PMIC on fail!\n");
		}
		WMT_PLAT_INFO_FUNC("turn on vcn28 for fm/gps usage in co-clock mode\n");
	} else {
		/*in co-clock mode,need to turn off vcn28 when fm off */
		if (reg_VCN28)
			if (regulator_disable(reg_VCN28))
				WMT_PLAT_ERR_FUNC("WMT do VCN28 PMIC off fail!\n");
		WMT_PLAT_INFO_FUNC("turn off vcn28 for fm/gps usage in co-clock mode\n");
	}
	return 0;
}

INT32 mtk_wcn_consys_hw_state_show(VOID)
{
	return 0;
}

INT32 mtk_wcn_consys_hw_restore(struct device *device)
{
	UINT32 addrPhy = 0;

	if (gConEmiPhyBase) {

#if CONSYS_EMI_MPU_SETTING
		/*set MPU for EMI share Memory */
		WMT_PLAT_INFO_FUNC("setting MPU for EMI share memory\n");

#if 0
	emi_mpu_set_region_protection(gConEmiPhyBase + SZ_1M/2,
		gConEmiPhyBase + SZ_1M,
		5,
		SET_ACCESS_PERMISSON(FORBIDDEN, NO_PROTECTION, FORBIDDEN, NO_PROTECTION));


#else
		WMT_PLAT_WARN_FUNC("not define platform config\n");
#endif

#endif
		/*consys to ap emi remapping register:10001320, cal remapping address */
		addrPhy = (gConEmiPhyBase & 0xFFF00000) >> 20;

		/*
		 * The "-0x400" is present in Amazon's own driver and was
		 * missing here. This register is the aperture through which
		 * the CONNSYS firmware reaches AP DRAM, so a wrong value does
		 * not fail loudly - it silently points the chip's DMA at the
		 * WRONG physical address. That matches the corruption seen on
		 * this port exactly: a few seconds after any chip-side assert,
		 * kernel heap metadata is wrecked and something unrelated dies
		 * (slab alloc/free, irq_desc, __fput). The corruption survived
		 * disabling every driver-side assert-handling path in turn -
		 * paged dump, paged trace, and the whole ctx-save block - which
		 * is what you'd expect if the writer is the chip, not us.
		 *
		 * The register offset was wrong too (0x310, an MT7623-lineage
		 * value, vs 0x320 for this SoC) - fixed in the header.
		 */
		addrPhy -= 0x400;

		/*enable consys to ap emi remapping bit12 */
		addrPhy = addrPhy | 0x1000;

		/* Mask the field before writing - see the long comment on the
		 * matching write in mtk_wcn_consys_hw_init().
		 */
		CONSYS_REG_WRITE(conn_reg.topckgen_base + CONSYS_EMI_MAPPING_OFFSET,
				 (CONSYS_REG_READ(conn_reg.topckgen_base + CONSYS_EMI_MAPPING_OFFSET)
				  & ~0x1FFF) | addrPhy);

		WMT_PLAT_INFO_FUNC("CONSYS_EMI_MAPPING dump in restore cb(0x%08x) want(0x%08x)\n",
				   CONSYS_REG_READ(conn_reg.topckgen_base + CONSYS_EMI_MAPPING_OFFSET),
				   addrPhy);

#if 1
		pEmibaseaddr = ioremap(gConEmiPhyBase + CONSYS_EMI_AP_PHY_OFFSET, CONSYS_EMI_MEM_SIZE);
#else
		pEmibaseaddr = ioremap(CONSYS_EMI_AP_PHY_BASE, CONSYS_EMI_MEM_SIZE);
#endif
		if (pEmibaseaddr) {
			WMT_PLAT_INFO_FUNC("EMI mapping OK(0x%p)\n", pEmibaseaddr);
			memset_io(pEmibaseaddr, 0, CONSYS_EMI_MEM_SIZE);
		} else {
			WMT_PLAT_ERR_FUNC("EMI mapping fail\n");
		}
	} else {
		WMT_PLAT_ERR_FUNC("consys emi memory address gConEmiPhyBase invalid\n");
	}

	return 0;
}

/*Reserved memory by device tree!*/
int reserve_memory_consys_fn(struct reserved_mem *rmem)
{
	WMT_PLAT_WARN_FUNC(" name: %s, base: 0x%llx, size: 0x%llx\n", rmem->name,
			   (unsigned long long)rmem->base, (unsigned long long)rmem->size);
	gConEmiPhyBase = rmem->base;
	return 0;
}

RESERVEDMEM_OF_DECLARE(reserve_memory_test, "mediatek,consys-reserve-memory", reserve_memory_consys_fn);


/*
 * /proc/biscuit_emi - read the CONNSYS EMI window.
 *
 * The WiFi firmware image is encrypted on disk and the chip decrypts it as it
 * places it, so this DRAM holds the only plaintext copy of that firmware that
 * exists anywhere. Reading it back is the difference between guessing what the
 * firmware waits for and looking at it. /dev/mem cannot reach it: this is a
 * no-map reserved region and has no kernel mapping to hand out.
 */
static ssize_t biscuit_emi_read(struct file *f, char __user *buf, size_t len, loff_t *off)
{
	void __iomem *p;
	void *tmp;
	size_t n;

	if (!gConEmiPhyBase || *off >= (2 * 1024 * 1024))
		return 0;
	n = min(len, (size_t)(2 * 1024 * 1024 - (size_t)*off));
	n = min(n, (size_t)(64 * 1024));

	p = ioremap(gConEmiPhyBase + *off, n);
	if (!p)
		return -EIO;
	tmp = kmalloc(n, GFP_KERNEL);
	if (!tmp) {
		iounmap(p);
		return -ENOMEM;
	}
	memcpy_fromio(tmp, p, n);
	iounmap(p);
	if (copy_to_user(buf, tmp, n)) {
		kfree(tmp);
		return -EFAULT;
	}
	kfree(tmp);
	*off += n;
	return n;
}

static const struct proc_ops biscuit_emi_fops = {
	.proc_read = biscuit_emi_read,
	.proc_lseek = default_llseek,
};

INT32 mtk_wcn_consys_hw_init(void)
{

	INT32 iRet = -1;
	UINT32 addrPhy = 0;
	INT32 i = 0;
	struct device_node *node = NULL;

	/* create this early - later paths in this function can return first */
	proc_create("biscuit_emi", 0400, NULL, &biscuit_emi_fops);

	node = of_find_compatible_node(NULL, NULL, "mediatek,mt8163-consys");
	if (node) {
		/*
		 * biscuit's mt8163-amazon-common.dtsi consys@18070000 node
		 * (copied from Amazon's real production DT) has 3 reg
		 * entries in this order: CONN_MCU_CONFIG_BASE, AP_RGU_BASE,
		 * TOPCKGEN_BASE -- matching Amazon's original 3.18 driver.
		 * This frank-w-derived backend was already modernized to
		 * use the reset-controller framework (see rstc /
		 * devm_reset_control_get() below) instead of raw AP_RGU
		 * MMIO poking, so ap_rgu_base is currently unused, but it
		 * must still be consumed here (index 1) or the following
		 * of_iomap(node, 2) for topckgen would silently read the
		 * AP_RGU window instead.
		 */
		conn_reg.mcu_base = (SIZE_T) of_iomap(node, i);
		WMT_PLAT_DBG_FUNC("Get mcu register base(0x%zx)\n", conn_reg.mcu_base);
		i++;

		conn_reg.ap_rgu_base = (SIZE_T) of_iomap(node, i);
		WMT_PLAT_DBG_FUNC("Get ap_rgu register base(0x%zx)\n", conn_reg.ap_rgu_base);
		i++;

		conn_reg.topckgen_base = (SIZE_T) of_iomap(node, i);
		WMT_PLAT_DBG_FUNC("Get topckgen register base(0x%zx)\n", conn_reg.topckgen_base);
		i++;
	} else {
		WMT_PLAT_ERR_FUNC("[%s] can't find CONSYS compatible node\n", __func__);
		return iRet;
	}
	if (gConEmiPhyBase) {
#if CONSYS_EMI_MPU_SETTING
		/*set MPU for EMI share Memory */
		WMT_PLAT_INFO_FUNC("setting MPU for EMI share memory\n");

#if 0
	emi_mpu_set_region_protection(gConEmiPhyBase + SZ_1M/2,
		gConEmiPhyBase + SZ_1M,
		5,
		SET_ACCESS_PERMISSON(FORBIDDEN, NO_PROTECTION, FORBIDDEN, NO_PROTECTION));
#else
		WMT_PLAT_WARN_FUNC("not define platform config\n");
#endif

#endif
		WMT_PLAT_DBG_FUNC("get consys start phy address(0x%zx)\n", (SIZE_T) gConEmiPhyBase);

		/*consys to ap emi remapping register:10001320, cal remapping address */
		addrPhy = (gConEmiPhyBase & 0xFFF00000) >> 20;

		/*
		 * The "-0x400" is present in Amazon's own driver and was
		 * missing here. This register is the aperture through which
		 * the CONNSYS firmware reaches AP DRAM, so a wrong value does
		 * not fail loudly - it silently points the chip's DMA at the
		 * WRONG physical address. That matches the corruption seen on
		 * this port exactly: a few seconds after any chip-side assert,
		 * kernel heap metadata is wrecked and something unrelated dies
		 * (slab alloc/free, irq_desc, __fput). The corruption survived
		 * disabling every driver-side assert-handling path in turn -
		 * paged dump, paged trace, and the whole ctx-save block - which
		 * is what you'd expect if the writer is the chip, not us.
		 *
		 * The register offset was wrong too (0x310, an MT7623-lineage
		 * value, vs 0x320 for this SoC) - fixed in the header.
		 */
		addrPhy -= 0x400;

		/*enable consys to ap emi remapping bit12 */
		addrPhy = addrPhy | 0x1000;

		/*
		 * Clear the address field before writing it.
		 *
		 * Upstream (and Amazon) do a bare read-modify-write with OR
		 * here, which silently assumes the field is zero to begin
		 * with. On this boot path it is not: something before us -
		 * LK/u-boot, or the register's own reset value - leaves 0x5f4
		 * in it, the un-offset megabyte index. OR-ing our 0x1f4 into
		 * that cannot clear the stale 0x400 bit, so the register ended
		 * up 0x15f4 instead of 0x11f4 and the aperture pointed at
		 * (0x5f4 << 20) + 0x40000000 = 0x9f400000 - past the end of the
		 * 1GB fitted here.
		 *
		 * Nothing fails loudly when that happens. Every firmware
		 * section still "downloads" successfully, because the HIF
		 * handshake is driven by byte counts rather than by content,
		 * but the ~344KB of image destined for EMI (sections 2 and 3,
		 * 0xf0006000 and 0xf004e000 - about 92% of WIFI_RAM_CODE_8163)
		 * is written into nowhere. The MCU then sits in ROM spinning
		 * around 0xe93c-0xe968 with "INIT" in its mailboxes, and the
		 * only visible symptom is that the ready bit never asserts:
		 *   wlanAdapterStart: Waiting for Ready bit: Timeout, ID=200
		 *
		 * Bits 0..11 are the megabyte index and bit 12 is the enable,
		 * so mask off the whole 13-bit field rather than OR-ing.
		 */
		CONSYS_REG_WRITE(conn_reg.topckgen_base + CONSYS_EMI_MAPPING_OFFSET,
				 (CONSYS_REG_READ(conn_reg.topckgen_base + CONSYS_EMI_MAPPING_OFFSET)
				  & ~0x1FFF) | addrPhy);

		WMT_PLAT_INFO_FUNC("CONSYS_EMI_MAPPING dump(0x%08x) want(0x%08x)\n",
				   CONSYS_REG_READ(conn_reg.topckgen_base + CONSYS_EMI_MAPPING_OFFSET),
				   addrPhy);

#if 1
		pEmibaseaddr = ioremap(gConEmiPhyBase + CONSYS_EMI_AP_PHY_OFFSET, CONSYS_EMI_MEM_SIZE);
#else
		pEmibaseaddr = ioremap(CONSYS_EMI_AP_PHY_BASE, CONSYS_EMI_MEM_SIZE);
#endif
		/* pEmibaseaddr = ioremap(0x80090400,270*KBYTE); */
		if (pEmibaseaddr) {
			WMT_PLAT_INFO_FUNC("EMI mapping OK(0x%p)\n", pEmibaseaddr);
			memset_io(pEmibaseaddr, 0, CONSYS_EMI_MEM_SIZE);
			iRet = 0;
		} else {
			WMT_PLAT_ERR_FUNC("EMI mapping fail\n");
		}
	} else {
		WMT_PLAT_ERR_FUNC("consys emi memory address gConEmiPhyBase invalid\n");
	}
#ifdef CONFIG_MTK_HIBERNATION
	WMT_PLAT_INFO_FUNC("register connsys restore cb for complying with IPOH function\n");
	register_swsusp_restore_noirq_func(ID_M_CONNSYS, mtk_wcn_consys_hw_restore, NULL);
#endif
	iRet = platform_driver_register(&mtk_wmt_dev_drv);
	if (iRet)
		WMT_PLAT_ERR_FUNC("WMT platform driver registered failed(%d)\n", iRet);
	return iRet;
}

INT32 mtk_wcn_consys_hw_deinit(void)
{
	if (pEmibaseaddr) {
		iounmap(pEmibaseaddr);
		pEmibaseaddr = NULL;
	}
#ifdef CONFIG_MTK_HIBERNATION
	unregister_swsusp_restore_noirq_func(ID_M_CONNSYS);
#endif

	platform_driver_unregister(&mtk_wmt_dev_drv);
	return 0;
}

UINT8 *mtk_wcn_consys_emi_virt_addr_get(UINT32 ctrl_state_offset)
{
	UINT8 *p_virtual_addr = NULL;

	if (!pEmibaseaddr) {
		WMT_PLAT_ERR_FUNC("EMI base address is NULL\n");
		return NULL;
	}
	/*
	 * ctrl_state_offset can come from chip-supplied register content
	 * (e.g. btm_core.c computing "chip_sync_addr - emi_phy_addr" from
	 * a hardware register read on a connsys chip that just crashed,
	 * which can hold garbage). pEmibaseaddr is an ioremap() of exactly
	 * CONSYS_EMI_MEM_SIZE bytes - with no bounds check here, a garbage
	 * offset produces a wild pointer into unmapped MMIO space (an
	 * immediate external abort) or, if it wraps into another mapped
	 * region, a write to the wrong hardware register. Reject anything
	 * outside the actual ioremap'd window instead.
	 */
	if (ctrl_state_offset >= CONSYS_EMI_MEM_SIZE) {
		WMT_PLAT_ERR_FUNC("ctrl_state_offset(%08x) exceeds EMI window(%08x)\n",
				  ctrl_state_offset, (UINT32)CONSYS_EMI_MEM_SIZE);
		return NULL;
	}
	WMT_PLAT_DBG_FUNC("ctrl_state_offset(%08x)\n", ctrl_state_offset);
	p_virtual_addr = pEmibaseaddr + ctrl_state_offset;

	return p_virtual_addr;
}

UINT32 mtk_wcn_consys_soc_chipid(void)
{
	return PLATFORM_SOC_CHIP;
}

struct pinctrl *mtk_wcn_consys_get_pinctrl(void)
{
	return consys_pinctrl;
}
INT32 mtk_wcn_consys_set_dynamic_dump(PUINT32 str_buf)
{
	PUINT8 vir_addr = NULL;

	vir_addr = mtk_wcn_consys_emi_virt_addr_get(EXP_APMEM_CTRL_CHIP_DYNAMIC_DUMP);
	if (!vir_addr) {
		WMT_PLAT_ERR_FUNC("get vir address fail\n");
		return -2;
	}
	memcpy(vir_addr, str_buf, DYNAMIC_DUMP_GROUP_NUM*8);
	WMT_PLAT_INFO_FUNC("dynamic dump register value(0x%08x)\n", CONSYS_REG_READ(vir_addr));
	return 0;
}
