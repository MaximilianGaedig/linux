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
		pr_err("mt8163: CCI snoop: ioremap failed\n");
		return;
	}

	val = readl_relaxed(mcucfg_base + 0x02C);
	iounmap(mcucfg_base);

	ret = mt8163_secure_call(MTK_SIP_KERNEL_MCUSYS_WRITE,
				  MT8163_MP0_AXI_CONFIG_PHY, val & ~MT8163_ACINACTM, 0);
	pr_debug("mt8163: CCI snoop enable: val=0x%x smc=%d\n",
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


#define PSCI_0_2_FN_BASE		0x84000000
#define PSCI_0_2_FN(n)			(PSCI_0_2_FN_BASE + (n))
#define PSCI_0_2_FN_PSCI_FEATURES	PSCI_0_2_FN(10)
#define PSCI_0_2_FN64_CPU_ON		(PSCI_0_2_FN_BASE | 0x40000000 | 3)
#define PSCI_0_2_FN_PSCI_VERSION	PSCI_0_2_FN(0)
#define PSCI_0_2_FN64_AFFINITY_INFO	(PSCI_0_2_FN_BASE | 0x40000000 | 4)

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
	}

	return 0;
}

/*
 * SPM/MTCMOS power-on for a secondary core, restored and re-ordered.
 *
 * New evidence: PSCI here is v0.2 and CPU_ON is NOT a stub - it answers
 * INVALID_PARAMS (-2) to a bogus MPIDR, and after a real call AFFINITY_INFO
 * moves OFF -> ON_PENDING. So ATF does accept the request and does program the
 * warm-boot entry; the core simply never comes out of reset, so it never
 * reaches even ATF's own warm-boot path and the state stays ON_PENDING.
 *
 * The previous attempt ran this MTCMOS sequence BEFORE cpu_on, which lets ATF
 * see the domain already powered and no-op its own release. Run it AFTER
 * cpu_on instead: ATF programs the boot address, then we supply the power-up
 * it is failing to complete.
 */





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
			pr_err("mt8163: cpu MTCMOS: SPM ioremap failed\n");
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
		pr_err("mt8163: cpu%u MTCMOS PWR_STATUS ack timeout\n", cpu);
		ok = false;
		goto out;
	}

	val = readl_relaxed(mt8163_spm_base + pwr_con_off);
	writel_relaxed(val & ~PWR_ISO, mt8163_spm_base + pwr_con_off);

	val = readl_relaxed(mt8163_spm_base + l1_pdn_off);
	writel_relaxed(val & ~L1_PDN, mt8163_spm_base + l1_pdn_off);

	if (!mt8163_spm_poll(mt8163_spm_base, l1_pdn_off, L1_PDN_ACK, 0)) {
		pr_err("mt8163: cpu%u MTCMOS L1_PDN_ACK timeout\n", cpu);
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
	pr_debug("mt8163: cpu%u MTCMOS power-on %s\n", cpu, ok ? "ok" : "FAILED");
	return ok;
}

static int cpu_psci_cpu_boot(unsigned int cpu)
{
	phys_addr_t pa_secondary_entry = __pa_symbol(secondary_entry);
	int err, before, after;

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
	/*
	 * Plain PSCI, and nothing else.
	 *
	 * Every non-secure route to the secondary entrypoint is blocked on this
	 * board: INFRACFG_AO+0x800 is dropped on a direct write and rejected by
	 * MTK_SIP_KERNEL_MCUSYS_WRITE (-3), and the MCUCFG per-CPU reset vectors
	 * ATF itself uses (MCUCFG+0x38+cpu*8) are rejected by the same SMC (-4);
	 * only MCUCFG+0x2C (CCI snoop) is in its allowlist. So if the cores can
	 * be released from EL1 at all it has to be ATF's own CPU_ON doing it.
	 * Our MTCMOS pokes may themselves be the problem - powering the domain
	 * from non-secure behind ATF's back can make its pwr_domain_on() see the
	 * domain already up and no-op, which matches cpu_on() returning success
	 * while the core never runs.
	 *
	 * Measured with a clean, untouched sequence: cpu_on() still returns 0 and
	 * the cores still never run, matching PSCI_FEATURES(CPU_ON) reporting
	 * NOT_SUPPORTED - this board's secure monitor implements CPU_ON as a stub.
	 * Releasing the secondaries therefore needs the entrypoint written from
	 * secure context (the bootloader), which is out of scope here. The MTCMOS
	 * power-on and INFRACFG/MCUCFG boot-address writers that used to live here
	 * were removed: MTCMOS worked but only powered domains whose cores then
	 * jumped to address 0, and both address writers were rejected outright.
	 */
	/*
	 * One CPU_ON, and then ask the firmware what it thinks happened.
	 *
	 * AFFINITY_INFO is the discriminator this path was missing: if ATF
	 * reports the core ON after a SUCCESS, the core really was released and
	 * the fault is on our side of the handoff (entry address, exception
	 * level, coherency). If it still reports OFF, ATF accepted the call and
	 * did nothing, and no amount of kernel-side work will help.
	 *
	 * This used to call cpu_on twice and report the second, already-on
	 * result (-22 INVALID_PARAMS) alongside the first, which read as a
	 * contradiction and hid the fact that the first call succeeds.
	 */
	before = mt8163_raw_smc(PSCI_0_2_FN64_AFFINITY_INFO, cpu_logical_map(cpu), 0, 0);
	err = psci_ops.cpu_on(cpu_logical_map(cpu), pa_secondary_entry);
	/* ATF has set the entry and left the core ON_PENDING; power it. */
	if (!err)
		mt8163_mtcmos_power_on_cpu(cpu);
	udelay(1000);
	after = mt8163_raw_smc(PSCI_0_2_FN64_AFFINITY_INFO, cpu_logical_map(cpu), 0, 0);
	pr_debug("mt8163: cpu%u on: psci=%d affinity %d -> %d\n",
		 cpu, err, before, after);

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



