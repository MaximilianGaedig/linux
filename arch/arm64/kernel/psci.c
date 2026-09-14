// SPDX-License-Identifier: GPL-2.0-only
/*
 *
 * Copyright (C) 2013 ARM Limited
 *
 * Author: Will Deacon <will.deacon@arm.com>
 */

#define pr_fmt(fmt) "psci: " fmt

#include <linux/arm-smccc.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/of.h>
#include <linux/smp.h>
#include <linux/delay.h>
#include <linux/psci.h>
#include <linux/mm.h>

#include <uapi/linux/psci.h>

#include <asm/cpu_ops.h>
#include <asm/errno.h>
#include <asm/smp_plat.h>

/*
 * MT8163 quirk: the secure firmware on these boards accepts PSCI CPU_ON and
 * programs the warm-boot entry, but never powers up the secondary core, so
 * it stays ON_PENDING forever. Supply the missing steps from here: clear the
 * cluster's ACINACTM (CCI snoop) bit through the vendor SIP call, as the
 * vendor kernel does before releasing any core, and after a successful
 * CPU_ON run the SPM power-on sequence for the core's MTCMOS domain.
 * This belongs in the firmware; it lives here because arm64's PSCI cpu_ops
 * have no platform hook.
 */
#define MTK_SIP_KERNEL_MCUSYS_WRITE	0x82000201
#define MT8163_MCUCFG_BASE		0x10200000
#define MT8163_MP0_AXI_CONFIG		(MT8163_MCUCFG_BASE + 0x02c)
#define MT8163_ACINACTM			BIT(4)

#define MT8163_SPM_BASE			0x10006000
#define SPM_POWERON_CONFIG_SET		0x0000
#define SPM_PROJECT_CODE		0x0b16
#define SPM_PWR_STATUS			0x060c
#define SPM_PWR_STATUS_2ND		0x0610

#define PWR_RST_B			BIT(0)
#define PWR_ISO				BIT(1)
#define PWR_ON				BIT(2)
#define PWR_ON_2ND			BIT(3)
#define PWR_CLK_DIS			BIT(4)
#define SRAM_CKISO			BIT(5)
#define SRAM_ISOINT_B			BIT(6)
#define L1_PDN				BIT(0)
#define L1_PDN_ACK			BIT(8)

#define MT8163_SPM_POLL_US		200000

static const struct {
	unsigned int pwr_con, l1_pdn;
	u32 status_bit;
} mt8163_cpu_mtcmos[] = {
	[1] = { 0x0218, 0x0264, BIT(10) },
	[2] = { 0x021c, 0x026c, BIT(11) },
	[3] = { 0x0220, 0x0274, BIT(12) },
};

static bool mt8163_quirk;

static void __init mt8163_cpu_quirk_init(void)
{
	void __iomem *mcucfg;
	struct arm_smccc_res res;
	u32 val;

	mt8163_quirk = of_machine_is_compatible("mediatek,mt8163");
	if (!mt8163_quirk)
		return;

	mcucfg = ioremap(MT8163_MCUCFG_BASE, SZ_4K);
	if (!mcucfg)
		return;
	val = readl(mcucfg + 0x02c);
	iounmap(mcucfg);

	arm_smccc_smc(MTK_SIP_KERNEL_MCUSYS_WRITE, MT8163_MP0_AXI_CONFIG,
		      val & ~MT8163_ACINACTM, 0, 0, 0, 0, 0, &res);
}

static bool mt8163_spm_poll(void __iomem *reg, u32 mask, u32 want)
{
	u32 val;

	return !readl_poll_timeout_atomic(reg, val, (val & mask) == want, 1,
					  MT8163_SPM_POLL_US);
}

static void mt8163_cpu_power_on(unsigned int cpu)
{
	static DEFINE_SPINLOCK(lock);
	unsigned int pwr_con, l1_pdn;
	void __iomem *spm;
	unsigned long flags;
	u32 bit;

	if (cpu < 1 || cpu >= ARRAY_SIZE(mt8163_cpu_mtcmos))
		return;
	pwr_con = mt8163_cpu_mtcmos[cpu].pwr_con;
	l1_pdn = mt8163_cpu_mtcmos[cpu].l1_pdn;
	bit = mt8163_cpu_mtcmos[cpu].status_bit;

	spm = ioremap(MT8163_SPM_BASE, SZ_4K);
	if (!spm) {
		pr_err("CPU%u: cannot map SPM\n", cpu);
		return;
	}

	spin_lock_irqsave(&lock, flags);

	writel((SPM_PROJECT_CODE << 16) | BIT(0), spm + SPM_POWERON_CONFIG_SET);

	writel(readl(spm + pwr_con) | PWR_ON, spm + pwr_con);
	udelay(1);
	writel(readl(spm + pwr_con) | PWR_ON_2ND, spm + pwr_con);
	if (!mt8163_spm_poll(spm + SPM_PWR_STATUS, bit, bit) ||
	    !mt8163_spm_poll(spm + SPM_PWR_STATUS_2ND, bit, bit)) {
		pr_err("CPU%u: MTCMOS power-on timed out\n", cpu);
		goto out;
	}

	writel(readl(spm + pwr_con) & ~PWR_ISO, spm + pwr_con);
	writel(readl(spm + l1_pdn) & ~L1_PDN, spm + l1_pdn);
	if (!mt8163_spm_poll(spm + l1_pdn, L1_PDN_ACK, 0)) {
		pr_err("CPU%u: SRAM power-on timed out\n", cpu);
		goto out;
	}
	udelay(1);

	writel(readl(spm + pwr_con) | SRAM_ISOINT_B, spm + pwr_con);
	writel(readl(spm + pwr_con) & ~SRAM_CKISO, spm + pwr_con);
	writel(readl(spm + pwr_con) & ~PWR_CLK_DIS, spm + pwr_con);
	writel(readl(spm + pwr_con) | PWR_RST_B, spm + pwr_con);
out:
	spin_unlock_irqrestore(&lock, flags);
	iounmap(spm);
}

static int __init cpu_psci_cpu_init(unsigned int cpu)
{
	return 0;
}

static int __init cpu_psci_cpu_prepare(unsigned int cpu)
{
	static bool quirk_checked;

	if (!psci_ops.cpu_on) {
		pr_err("no cpu_on method, not booting CPU%d\n", cpu);
		return -ENODEV;
	}

	if (!quirk_checked) {
		mt8163_cpu_quirk_init();
		quirk_checked = true;
	}

	return 0;
}

static int cpu_psci_cpu_boot(unsigned int cpu)
{
	phys_addr_t pa_secondary_entry = __pa_symbol(secondary_entry);
	int err = psci_ops.cpu_on(cpu_logical_map(cpu), pa_secondary_entry);
	if (err && err != -EPERM)
		pr_err("failed to boot CPU%d (%d)\n", cpu, err);
	else if (!err && mt8163_quirk)
		mt8163_cpu_power_on(cpu);

	return err;
}

#ifdef CONFIG_HOTPLUG_CPU
static bool cpu_psci_cpu_can_disable(unsigned int cpu)
{
	return !psci_tos_resident_on(cpu);
}

static int cpu_psci_cpu_disable(unsigned int cpu)
{
	/* Fail early if we don't have CPU_OFF support */
	if (!psci_ops.cpu_off)
		return -EOPNOTSUPP;

	/* Trusted OS will deny CPU_OFF */
	if (psci_tos_resident_on(cpu))
		return -EPERM;

	return 0;
}

static void cpu_psci_cpu_die(unsigned int cpu)
{
	/*
	 * There are no known implementations of PSCI actually using the
	 * power state field, pass a sensible default for now.
	 */
	u32 state = PSCI_POWER_STATE_TYPE_POWER_DOWN <<
		    PSCI_0_2_POWER_STATE_TYPE_SHIFT;

	psci_ops.cpu_off(state);
}

static int cpu_psci_cpu_kill(unsigned int cpu)
{
	int err;
	unsigned long start, end;

	if (!psci_ops.affinity_info)
		return 0;
	/*
	 * cpu_kill could race with cpu_die and we can
	 * potentially end up declaring this cpu undead
	 * while it is dying. So, try again a few times.
	 */

	start = jiffies;
	end = start + msecs_to_jiffies(100);
	do {
		err = psci_ops.affinity_info(cpu_logical_map(cpu), 0);
		if (err == PSCI_0_2_AFFINITY_LEVEL_OFF) {
			pr_info("CPU%d killed (polled %d ms)\n", cpu,
				jiffies_to_msecs(jiffies - start));
			return 0;
		}

		usleep_range(100, 1000);
	} while (time_before(jiffies, end));

	pr_warn("CPU%d may not have shut down cleanly (AFFINITY_INFO reports %d)\n",
			cpu, err);
	return -ETIMEDOUT;
}
#endif

const struct cpu_operations cpu_psci_ops = {
	.name		= "psci",
	.cpu_init	= cpu_psci_cpu_init,
	.cpu_prepare	= cpu_psci_cpu_prepare,
	.cpu_boot	= cpu_psci_cpu_boot,
#ifdef CONFIG_HOTPLUG_CPU
	.cpu_can_disable = cpu_psci_cpu_can_disable,
	.cpu_disable	= cpu_psci_cpu_disable,
	.cpu_die	= cpu_psci_cpu_die,
	.cpu_kill	= cpu_psci_cpu_kill,
#endif
};

