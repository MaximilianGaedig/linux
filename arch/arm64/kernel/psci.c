// SPDX-License-Identifier: GPL-2.0-only
/*
 *
 * Copyright (C) 2013 ARM Limited
 *
 * Author: Will Deacon <will.deacon@arm.com>
 */

#define pr_fmt(fmt) "psci: " fmt

#include <linux/init.h>
#include <linux/of.h>
#include <linux/smp.h>
#include <linux/delay.h>
#include <linux/psci.h>
#include <linux/mm.h>
#include <linux/io.h>

#include <uapi/linux/psci.h>

#include <asm/cpu_ops.h>
#include <asm/errno.h>
#include <asm/smp_plat.h>
#include <asm/io.h>

/*
 * MT8163-specific: the stock (32-bit) Amazon kernel's SMP bring-up
 * (drivers/misc/mediatek/base/power/mt8163/mt-smp.c, smp_set_boot_addr())
 * writes the secondary CPU entry physical address into an INFRACFG_AO
 * register (base 0x10001000, offset 0x800) *before* releasing the core,
 * unconditionally - not just in the non-PSCI branch. On real hardware here,
 * generic PSCI cpu_on() alone returns success for CPU1-3 but they never
 * actually start executing (boot status stays 0), which matches this SoC's
 * PSCI firmware expecting the jump target to already be staged in this
 * register rather than trusting the entry_point_address SMC argument.
 * Mirror that write here since arch/arm64's generic PSCI cpu_ops has no
 * platform hook for it.
 */
#define MT8163_INFRACFG_AO_BASE	0x10001000
#define MT8163_INFRACFG_AO_BOOT_ADDR_OFF	0x800

/*
 * MT8163-specific SIP SMC service ("MTK_SIP_KERNEL_MCUSYS_WRITE" in the
 * stock kernel's mt_secure_api.h) used to write MCUSYS registers that are
 * secure-world-only accessible - e.g. the AXI snoop-control bit the stock
 * kernel's mt_smp_init_cpus() clears *unconditionally, even under PSCI*,
 * before ever releasing a secondary core:
 *   mcusys_smc_write_phy(MP0_AXI_CONFIG_PHY, val & ~ACINACTM)
 * Without this, CCI/cache-coherency snooping for the CPU cluster is left
 * disabled, which would explain a secondary core powering on via PSCI
 * cpu_on() (which returns success) but never reaching the point where it
 * updates its own boot status - it can hang almost immediately if it can't
 * safely observe coherent memory.
 */
#define MTK_SIP_KERNEL_MCUSYS_WRITE	0x82000201
#define MT8163_MCUCFG_BASE_PHY		0x10200000
#define MT8163_MP0_AXI_CONFIG_PHY	(MT8163_MCUCFG_BASE_PHY + 0x02C)
#define MT8163_ACINACTM			(1U << 4)

static noinline int mt8163_secure_call(u64 function_id, u64 arg0, u64 arg1, u64 arg2)
{
	register u64 reg0 __asm__("x0") = function_id;
	register u64 reg1 __asm__("x1") = arg0;
	register u64 reg2 __asm__("x2") = arg1;
	register u64 reg3 __asm__("x3") = arg2;

	asm volatile ("smc    #0\n" : "+r" (reg0) : "r"(reg1), "r"(reg2), "r"(reg3));

	return (int)reg0;
}

static void mt8163_enable_cci_snoop(void)
{
	void __iomem *mcucfg_base;
	u32 val;
	int ret;

	mcucfg_base = ioremap(MT8163_MCUCFG_BASE_PHY, 0x1000);
	if (!mcucfg_base) {
		pr_err("biscuit-debug: mt8163_enable_cci_snoop: ioremap failed\n");
		return;
	}

	val = readl_relaxed(mcucfg_base + 0x02C);
	iounmap(mcucfg_base);

	ret = mt8163_secure_call(MTK_SIP_KERNEL_MCUSYS_WRITE,
				  MT8163_MP0_AXI_CONFIG_PHY, val & ~MT8163_ACINACTM, 0);
	pr_err("biscuit-debug: mt8163_enable_cci_snoop: read val=0x%x, smc write ret=%d\n",
	       val, ret);
}

/*
 * MT8163 per-CPU MTCMOS power-domain sequencing, ported from the stock
 * kernel's drivers/misc/mediatek/base/power/mt8163/mtcmos.c
 * spm_mtcmos_ctrl_cpu{1,2,3}(). Confirmed necessary: with only the boot-
 * address register staged and CCI snoop enabled, CPU1-3 still failed to
 * come online - strongly indicating their power rails are simply off
 * (PSCI cpu_on() alone isn't completing this on this device/boot chain).
 * The vendor sequence polls hardware ack bits in tight infinite loops with
 * no timeout; since this device can't be power-cycled remotely if a step
 * hangs, every poll here is bounded and bails out (logging, not hanging)
 * if the SPM never acks - the goal is "never wedge forever", not perfect
 * fidelity to the original.
 */
#define MT8163_SPM_BASE_PHY			0x10006000
#define SPM_POWERON_CONFIG_SET			0x0000
#define SPM_CA7_CPU1_PWR_CON			0x0218
#define SPM_CA7_CPU2_PWR_CON			0x021c
#define SPM_CA7_CPU3_PWR_CON			0x0220
#define SPM_CA7_CPU1_L1_PDN			0x0264
#define SPM_CA7_CPU2_L1_PDN			0x026c
#define SPM_CA7_CPU3_L1_PDN			0x0274
#define SPM_PWR_STATUS				0x060c
#define SPM_PWR_STATUS_2ND			0x0610
#define SPM_PROJECT_CODE			0x0b16

#define SRAM_ISOINT_B				(1U << 6)
#define SRAM_CKISO				(1U << 5)
#define PWR_CLK_DIS				(1U << 4)
#define PWR_ON_2ND				(1U << 3)
#define PWR_ON					(1U << 2)
#define PWR_ISO					(1U << 1)
#define PWR_RST_B				(1U << 0)
#define L1_PDN_ACK				(1U << 8)
#define L1_PDN					(1U << 0)

#define CA7_CPU1				(1U << 10)
#define CA7_CPU2				(1U << 11)
#define CA7_CPU3				(1U << 12)

#define MT8163_SPM_POLL_MAX_US			200000

static void __iomem *mt8163_spm_base;
static DEFINE_SPINLOCK(mt8163_spm_lock);

static bool mt8163_spm_poll(void __iomem *base, unsigned int off, u32 mask, u32 want)
{
	unsigned int waited = 0;

	while ((readl_relaxed(base + off) & mask) != want) {
		udelay(1);
		if (++waited >= MT8163_SPM_POLL_MAX_US)
			return false;
	}
	return true;
}

/* Returns true if the CPU's power domain was successfully turned on. */
static bool mt8163_mtcmos_power_on_cpu(unsigned int cpu)
{
	unsigned long flags;
	unsigned int pwr_con_off, l1_pdn_off, ca7_bit;
	u32 val;
	bool ok = true;

	switch (cpu) {
	case 1:
		pwr_con_off = SPM_CA7_CPU1_PWR_CON;
		l1_pdn_off = SPM_CA7_CPU1_L1_PDN;
		ca7_bit = CA7_CPU1;
		break;
	case 2:
		pwr_con_off = SPM_CA7_CPU2_PWR_CON;
		l1_pdn_off = SPM_CA7_CPU2_L1_PDN;
		ca7_bit = CA7_CPU2;
		break;
	case 3:
		pwr_con_off = SPM_CA7_CPU3_PWR_CON;
		l1_pdn_off = SPM_CA7_CPU3_L1_PDN;
		ca7_bit = CA7_CPU3;
		break;
	default:
		return true;
	}

	if (!mt8163_spm_base) {
		mt8163_spm_base = ioremap(MT8163_SPM_BASE_PHY, 0x1000);
		if (!mt8163_spm_base) {
			pr_err("biscuit-debug: mtcmos: ioremap SPM base failed\n");
			return false;
		}
	}

	spin_lock_irqsave(&mt8163_spm_lock, flags);

	/* enable register control */
	writel_relaxed((SPM_PROJECT_CODE << 16) | (1U << 0),
			mt8163_spm_base + SPM_POWERON_CONFIG_SET);

	val = readl_relaxed(mt8163_spm_base + pwr_con_off);
	writel_relaxed(val | PWR_ON, mt8163_spm_base + pwr_con_off);
	udelay(1);
	val = readl_relaxed(mt8163_spm_base + pwr_con_off);
	writel_relaxed(val | PWR_ON_2ND, mt8163_spm_base + pwr_con_off);

	if (!mt8163_spm_poll(mt8163_spm_base, SPM_PWR_STATUS, ca7_bit, ca7_bit) ||
	    !mt8163_spm_poll(mt8163_spm_base, SPM_PWR_STATUS_2ND, ca7_bit, ca7_bit)) {
		pr_err("biscuit-debug: mtcmos: cpu%u PWR_STATUS ack timeout\n", cpu);
		ok = false;
		goto out;
	}

	val = readl_relaxed(mt8163_spm_base + pwr_con_off);
	writel_relaxed(val & ~PWR_ISO, mt8163_spm_base + pwr_con_off);

	val = readl_relaxed(mt8163_spm_base + l1_pdn_off);
	writel_relaxed(val & ~L1_PDN, mt8163_spm_base + l1_pdn_off);

	if (!mt8163_spm_poll(mt8163_spm_base, l1_pdn_off, L1_PDN_ACK, 0)) {
		pr_err("biscuit-debug: mtcmos: cpu%u L1_PDN_ACK timeout\n", cpu);
		ok = false;
		goto out;
	}

	udelay(1);
	val = readl_relaxed(mt8163_spm_base + pwr_con_off);
	writel_relaxed(val | SRAM_ISOINT_B, mt8163_spm_base + pwr_con_off);
	val = readl_relaxed(mt8163_spm_base + pwr_con_off);
	writel_relaxed(val & ~SRAM_CKISO, mt8163_spm_base + pwr_con_off);

	val = readl_relaxed(mt8163_spm_base + pwr_con_off);
	writel_relaxed(val & ~PWR_CLK_DIS, mt8163_spm_base + pwr_con_off);
	val = readl_relaxed(mt8163_spm_base + pwr_con_off);
	writel_relaxed(val | PWR_RST_B, mt8163_spm_base + pwr_con_off);

out:
	spin_unlock_irqrestore(&mt8163_spm_lock, flags);
	pr_err("biscuit-debug: mtcmos: cpu%u power-on %s\n", cpu, ok ? "OK" : "FAILED");
	return ok;
}

/*
 * SW_ROM_PD (bit 31 of BOOTROM_SEC_CTRL = INFRACFG_AO_BASE+0x804, the
 * register right next to the boot-address register at +0x800) enables
 * "BootROM power-down mode" - per the stock kernel's mt_smp_prepare_cpus()
 * (drivers/misc/mediatek/base/power/mt8163/mt-smp.c), this is set
 * *alongside* staging the boot address, and plausibly gates whether the
 * BootROM's reset-vector polling logic honors that boot-address register
 * at all. Never set by this port until now.
 */
#define MT8163_INFRACFG_AO_BOOTROM_SEC_CTRL_OFF	0x804
#define MT8163_SW_ROM_PD				(1U << 31)

static void mt8163_smp_set_boot_addr(phys_addr_t entry)
{
	void __iomem *infracfg_ao_base;
	u32 readback_addr, readback_ctrl;
	int smc_ret1, smc_ret2;

	/*
	 * Confirmed on real hardware: a direct non-secure writel_relaxed()
	 * to these registers is silently dropped - readback immediately
	 * after writing shows 0x0 regardless of what was written. This
	 * region is evidently secure-world-protected against non-secure
	 * (kernel/EL1) writes, the same way the CCI-snoop MP0_AXI_CONFIG
	 * register needs the MTK_SIP_KERNEL_MCUSYS_WRITE SMC rather than a
	 * direct poke. Route through the same secure-call service instead.
	 */
	smc_ret1 = mt8163_secure_call(MTK_SIP_KERNEL_MCUSYS_WRITE,
				       MT8163_INFRACFG_AO_BASE + MT8163_INFRACFG_AO_BOOT_ADDR_OFF,
				       (u32)entry, 0);
	pr_err("biscuit-debug: mt8163_smp_set_boot_addr: SMC-wrote entry=%pa to infracfg_ao+0x800, ret=%d\n",
	       &entry, smc_ret1);

	smc_ret2 = mt8163_secure_call(MTK_SIP_KERNEL_MCUSYS_WRITE,
				       MT8163_INFRACFG_AO_BASE + MT8163_INFRACFG_AO_BOOTROM_SEC_CTRL_OFF,
				       MT8163_SW_ROM_PD, 0);
	pr_err("biscuit-debug: mt8163_smp_set_boot_addr: SMC-set SW_ROM_PD at infracfg_ao+0x804, ret=%d\n",
	       smc_ret2);

	/* Read back (plain MMIO read, not SMC) to confirm the SMC writes stuck. */
	infracfg_ao_base = ioremap(MT8163_INFRACFG_AO_BASE, 0x1000);
	if (!infracfg_ao_base) {
		pr_err("biscuit-debug: mt8163_smp_set_boot_addr: ioremap failed\n");
		return;
	}
	readback_addr = readl_relaxed(infracfg_ao_base + MT8163_INFRACFG_AO_BOOT_ADDR_OFF);
	readback_ctrl = readl_relaxed(infracfg_ao_base + MT8163_INFRACFG_AO_BOOTROM_SEC_CTRL_OFF);
	pr_err("biscuit-debug: mt8163_smp_set_boot_addr: readback boot_addr=0x%x sec_ctrl=0x%x\n",
	       readback_addr, readback_ctrl);
	iounmap(infracfg_ao_base);
}

/*
 * Diagnostic only: dump the "mediatek,mt8163-atf-reserved-memory" region
 * (physical 0x43000000, 0x30000 bytes per mt8163.dtsi) that genuine ATF/
 * BL31 writes its own log ring buffer into, if it's running at all. Pure
 * read, no hardware side effects - safe regardless of whether real ATF is
 * present. Scans for printable ASCII runs since we don't have the log
 * ring's exact header format, just to see whether ATF logged anything
 * around our cpu_on attempts.
 */
#define MT8163_ATF_LOG_BASE	0x43000000
#define MT8163_ATF_LOG_SIZE	0x30000

static void mt8163_dump_atf_log(void)
{
	void __iomem *atf_base;
	char line[65];
	unsigned int i, j, n;

	atf_base = ioremap(MT8163_ATF_LOG_BASE, MT8163_ATF_LOG_SIZE);
	if (!atf_base) {
		pr_err("biscuit-debug: atf log: ioremap failed\n");
		return;
	}

	pr_err("biscuit-debug: atf log: scanning 0x%x bytes at 0x%x for ASCII text\n",
	       MT8163_ATF_LOG_SIZE, MT8163_ATF_LOG_BASE);

	for (i = 0; i < MT8163_ATF_LOG_SIZE; ) {
		u8 c = readb_relaxed(atf_base + i);

		if (c >= 0x20 && c < 0x7f) {
			n = 0;
			for (j = i; j < MT8163_ATF_LOG_SIZE && n < sizeof(line) - 1; j++) {
				u8 cc = readb_relaxed(atf_base + j);

				if (cc < 0x20 || cc >= 0x7f)
					break;
				line[n++] = cc;
			}
			line[n] = '\0';
			if (n >= 4)
				pr_err("biscuit-debug: atf log[0x%x]: %s\n", i, line);
			i = j + 1;
		} else {
			i++;
		}
	}

	pr_err("biscuit-debug: atf log: scan done\n");

	/*
	 * Zero printable ASCII found above is itself a data point, but could
	 * also just mean the log isn't plain text. Dump the raw first 256
	 * bytes regardless of content (via proper MMIO byte reads into a
	 * local buffer first - print_hex_dump isn't MMIO-safe), so we can
	 * tell "genuinely all zero / untouched" apart from "non-zero binary
	 * data that isn't ASCII".
	 */
	{
		u8 raw[256];

		for (i = 0; i < sizeof(raw); i++)
			raw[i] = readb_relaxed(atf_base + i);
		print_hex_dump(KERN_ERR, "biscuit-debug: atf raw[0x0]: ", DUMP_PREFIX_OFFSET,
				16, 1, raw, sizeof(raw), false);
	}

	iounmap(atf_base);
}

#define PSCI_0_2_FN_BASE		0x84000000
#define PSCI_0_2_FN(n)			(PSCI_0_2_FN_BASE + (n))
#define PSCI_0_2_FN_PSCI_FEATURES	PSCI_0_2_FN(10)
#define PSCI_0_2_FN64_CPU_ON		(PSCI_0_2_FN_BASE | 0x40000000 | 3)

static noinline int mt8163_raw_smc(u32 func, u64 a0, u64 a1, u64 a2)
{
	register u64 x0 __asm__("x0") = func;
	register u64 x1 __asm__("x1") = a0;
	register u64 x2 __asm__("x2") = a1;
	register u64 x3 __asm__("x3") = a2;

	asm volatile ("smc    #0\n" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3));
	return (int)x0;
}

static int __init cpu_psci_cpu_init(unsigned int cpu)
{
	return 0;
}

static int __init cpu_psci_cpu_prepare(unsigned int cpu)
{
	static bool cci_snoop_done;

	if (!psci_ops.cpu_on) {
		pr_err("no cpu_on method, not booting CPU%d\n", cpu);
		return -ENODEV;
	}

	if (!cci_snoop_done) {
		mt8163_enable_cci_snoop();
		cci_snoop_done = true;
		pr_err("biscuit-debug: PSCI_FEATURES(CPU_ON) = %d (0=supported per spec; negative=NOT_SUPPORTED/error)\n",
		       mt8163_raw_smc(PSCI_0_2_FN_PSCI_FEATURES, PSCI_0_2_FN64_CPU_ON, 0, 0));
	}

	return 0;
}

static int cpu_psci_cpu_boot(unsigned int cpu)
{
	phys_addr_t pa_secondary_entry = __pa_symbol(secondary_entry);
	int err;

	/*
	 * Confirmed via stock-kernel research: mt_smp_boot_secondary()
	 * (drivers/misc/mediatek/base/power/mt8163/mt-smp.c) and a live
	 * psci_ops.cpu_on() SMC are mutually exclusive in vendor source
	 * (#ifdef CONFIG_ARM_PSCI chooses one or the other) - the field-
	 * shipped release path for THIS device's CPU nodes actually uses
	 * enable-method "mt-boot", not "psci" at all; PSCI CPU_ON for this
	 * platform has apparently never been exercised in production.
	 * Calling both in sequence, as this code previously did, may cause
	 * ATF to see the power domain already turned on by our own MTCMOS
	 * pokes and silently no-op its own release logic - consistent with
	 * cpu_on() always returning success while the core never runs. Do
	 * the non-secure MTCMOS+boot-address release only, skip the SMC.
	 */
	mt8163_mtcmos_power_on_cpu(cpu);
	mt8163_smp_set_boot_addr(pa_secondary_entry);
	err = 0;
	pr_err("biscuit-debug: mt8163 non-PSCI release (cpu=%u, mpidr=%lu, entry=%pa) done\n",
	       cpu, cpu_logical_map(cpu), &pa_secondary_entry);

	/*
	 * Informational only: also try the real PSCI SMC now that MTCMOS/
	 * boot-addr/SW_ROM_PD are all staged (previously this was called
	 * with no staging at all). Its return value is logged but not
	 * relied on - err above (0) is what's returned to the arm64 SMP
	 * core, since the working theory is this call may be a no-op or
	 * even counterproductive on this platform.
	 */
	if (psci_ops.cpu_on) {
		int psci_err = psci_ops.cpu_on(cpu_logical_map(cpu), pa_secondary_entry);

		pr_err("biscuit-debug: (informational) psci_ops.cpu_on(cpu=%u) = %d\n",
		       cpu, psci_err);
	}

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

/*
 * Diagnostic: read back the per-CPU canary words the amonet trampoline's
 * spin-table stub (spin_stub2.S) writes as the very first thing it does,
 * before ever entering its wait loop - at physical 0x43102000 + cpu*8,
 * expected value 0xC0FFEE00 | cpu_id. Non-zero here proves BootROM
 * actually jumped a given secondary CPU to our stub (i.e. the boot-
 * address register write from the trampoline took effect, regardless of
 * whether reading that register back showed 0) - independent evidence
 * from whether Linux's own SMP bring-up later succeeds.
 */
#define MT8163_SPIN_CANARY_BASE	0x43102000

static void mt8163_dump_spin_canaries(void)
{
	void __iomem *base;
	int cpu;

	/*
	 * Now that MT8163_SPIN_CANARY_BASE's whole range is covered by the
	 * mt8163_spin_table_reserved DT node (no-map), ioremap() is correct
	 * again - the kernel's allocator won't have touched it, unlike the
	 * first (inconclusive) run of this diagnostic before the DT
	 * reservation existed, where the region was ordinary System RAM
	 * and ioremap() correctly refused to double-map it.
	 */
	base = ioremap(MT8163_SPIN_CANARY_BASE, 0x1000);
	if (!base) {
		pr_err("biscuit-debug: spin canary: ioremap failed\n");
		return;
	}

	for (cpu = 0; cpu < 4; cpu++) {
		u32 val = readl_relaxed(base + cpu * 8);

		pr_err("biscuit-debug: spin canary[cpu%d] = 0x%08x (expect 0x%08x if BootROM jumped here)\n",
		       cpu, val, 0xc0ffee00u | cpu);
	}

	iounmap(base);
}

static int __init mt8163_atf_log_late_init(void)
{
	mt8163_dump_atf_log();
	mt8163_dump_spin_canaries();
	return 0;
}
late_initcall(mt8163_atf_log_late_init);

