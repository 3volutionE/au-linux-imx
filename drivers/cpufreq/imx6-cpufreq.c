/*
 * Copyright (C) 2010-2013 Freescale Semiconductor, Inc. All Rights Reserved.
 */

/*
 * The code contained herein is licensed under the GNU General Public
 * License. You may obtain a copy of the GNU General Public License
 * Version 2 or later at the following locations:
 *
 * http://www.opensource.org/licenses/gpl-license.html
 * http://www.gnu.org/copyleft/gpl.html
 */

/*
 * A driver for the Freescale Semiconductor i.MXC CPUfreq module.
 * The CPUFREQ driver is for controlling CPU frequency. It allows you to change
 * the CPU clock speed on the fly.
 */

#include <linux/module.h>
#include <linux/cpufreq.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/of_device.h>
#include <linux/of_address.h>
#include <linux/delay.h>
#include <linux/suspend.h>
#include <linux/regulator/consumer.h>
#include <linux/mfd/syscon.h>
#include <linux/io.h>
#include <mach/hardware.h>
#include <mach/clock.h>
#include <mach/busfreq.h>
#include <mach/common.h>
#include <asm/cpu.h>
#include <asm/smp_plat.h>

#define CLK32_FREQ	32768
#define NANOSECOND	(1000 * 1000 * 1000)
#define OCOTP_SPEED_BIT_OFFSET (16)
#define SPEED_FUSE	(0x440)
#define ANA_REG_CORE	0x140

enum IMX_LDO_MODE {
	LDO_MODE_DEFAULT = -1,
	LDO_MODE_ENABLED = 0,
	LDO_MODE_BYPASSED = 1,
};

/*The below value aligned with SPEED_GRADING bits in 0x440 fuse offset */
enum IMX_CPU_RATE {
	CPU_AT_800MHz = 0,
	CPU_AT_1GHz = 2,
	CPU_AT_1_2GHz = 3,
	CPU_AT_DEFAULT = 0xff
};
static int cpu_freq_khz_min;
static int cpu_freq_khz_max;

static struct clk *cpu_clk, *pll1_sys, *pll1_sw, *step, *pll2_pfd2_396m;
static struct cpufreq_frequency_table *imx_freq_table;
static bool arm_use_pfd396;
static int cpu_op_nr;
static struct cpu_op *cpu_op_tbl;
static struct regulator *arm_regulator, *soc_regulator, *pu_regulator;
static bool cpufreq_suspend;
static bool arm_use_pfd396;
static struct mutex set_cpufreq_lock;
static u32 pre_suspend_rate;

static int ldo_bypass_mode = LDO_MODE_DEFAULT;
static int arm_max_freq = CPU_AT_DEFAULT;

static int set_cpu_freq(int freq)
{
	int i;
	int ret = 0;
	int pll_rate = 0;
	int org_cpu_rate;
	int cpu_volt = 0;
	int soc_volt = 0;
	int pu_volt = 0;
	static bool request_bus_high;

	org_cpu_rate = clk_get_rate(cpu_clk);
	if (org_cpu_rate == freq)
		return ret;
	for (i = 0; i < cpu_op_nr; i++) {
		if (freq == cpu_op_tbl[i].cpu_rate) {
			cpu_volt = cpu_op_tbl[i].cpu_voltage;
			soc_volt = cpu_op_tbl[i].soc_voltage;
			pu_volt = cpu_op_tbl[i].pu_voltage;
			pll_rate = cpu_op_tbl[i].pll_rate;
		}
	}

	pr_debug("new cpufreq %d, origin cpufreq %d, loops_per_jiffy %ld\n",
		freq, org_cpu_rate, loops_per_jiffy);

	if (cpu_volt == 0)
		return ret;

	if (freq > org_cpu_rate) {
		/* increase bus freq if cpufreq is increased */
		if (!request_bus_high) {
			request_bus_freq(BUS_FREQ_HIGH);
			request_bus_high = true;
		}
		if (!IS_ERR(soc_regulator)) {
			ret = regulator_set_voltage(soc_regulator,
				soc_volt, soc_volt);
			if (ret < 0) {
				pr_err("COULD NOT SET SOC VOLTAGE!!!!\n");
				return ret;
			}
		}

		if (!IS_ERR(pu_regulator) && imx_anatop_pu_is_enabled()) {
			/* set pu voltage if pu enabled */
			ret = regulator_set_voltage(pu_regulator,
				pu_volt, pu_volt);
			if (ret < 0) {
				pr_err("COULD NOT SET PU VOLTAGE!!!!\n");
				return ret;
			}
		}

		if (!IS_ERR(arm_regulator)) {
			ret = regulator_set_voltage(arm_regulator,
				cpu_volt, cpu_volt);
			if (ret < 0) {
				pr_err("COULD NOT SET CPU VOLTAGE!!!!\n");
				return ret;
			}
		}
	}
	if (freq > clk_get_rate(pll2_pfd2_396m)) {
		/* enable pfd396m */
		clk_prepare(pll2_pfd2_396m);
		clk_enable(pll2_pfd2_396m);
		/* move pll1_sw clk to step */
		clk_set_parent(pll1_sw, step);
		/* setp pll1_sys rate */
		clk_set_rate(pll1_sys, pll_rate);
		if (org_cpu_rate <= clk_get_rate(pll2_pfd2_396m)) {
			/* enable pll1_sys */
			clk_prepare(pll1_sys);
			clk_enable(pll1_sys);
		}
		/* move pll1_sw clk to pll1_sys */
		clk_set_parent(pll1_sw, pll1_sys);
		/* disable pfd396m */
		clk_disable(pll2_pfd2_396m);
		clk_unprepare(pll2_pfd2_396m);
		/* need to maintain pfd396's count right */
		if (arm_use_pfd396) {
			clk_disable(pll2_pfd2_396m);
			clk_unprepare(pll2_pfd2_396m);
		}
		arm_use_pfd396 = false;
	} else {
		/* enable pfd396m */
		clk_prepare(pll2_pfd2_396m);
		clk_enable(pll2_pfd2_396m);
		/* move pll1_sw clk to step */
		clk_set_parent(pll1_sw, step);
		/* disable pll1_sys */
		clk_disable(pll1_sys);
		clk_unprepare(pll1_sys);
		arm_use_pfd396 = true;
	}
	/* set arm divider */
	clk_set_rate(cpu_clk, freq);

	if (ret != 0) {
		pr_err("cannot set CPU clock rate\n");
		return ret;
	}
	if (freq < org_cpu_rate) {
		if (!IS_ERR(arm_regulator)) {
			ret = regulator_set_voltage(arm_regulator,
				cpu_volt, cpu_volt);
			if (ret < 0) {
				pr_err("COULD NOT SET CPU VOLTAGE!!!!\n");
				return ret;
			}
		}
		if (!IS_ERR(soc_regulator)) {
			ret = regulator_set_voltage(soc_regulator,
				soc_volt, soc_volt);
			if (ret < 0) {
				pr_err("COULD NOT SET SOC VOLTAGE!!!!\n");
				return ret;
			}
		}

		if (!IS_ERR(pu_regulator) && imx_anatop_pu_is_enabled()) {
			/* set pu voltage if pu enabled */
			ret = regulator_set_voltage(pu_regulator,
				pu_volt, pu_volt);
			if (ret < 0) {
				pr_err("COULD NOT SET PU VOLTAGE!!!!\n");
				return ret;
			}
		}

		/* release bus freq when cpufreq is lower to lowest setpoint */
		if (freq == cpu_op_tbl[0].cpu_rate && request_bus_high) {
			release_bus_freq(BUS_FREQ_HIGH);
			request_bus_high = false;
		}
	}
	return ret;
}

static int mxc_verify_speed(struct cpufreq_policy *policy)
{
	if (policy->cpu >= num_possible_cpus())
		return -EINVAL;

	return cpufreq_frequency_table_verify(policy, imx_freq_table);
}

static unsigned int mxc_get_speed(unsigned int cpu)
{
	if (cpu >= num_possible_cpus())
		return 0;
	return clk_get_rate(cpu_clk) / 1000;
}

static int mxc_set_target(struct cpufreq_policy *policy,
			  unsigned int target_freq, unsigned int relation)
{
	struct cpufreq_freqs freqs;
	int freq_Hz;
	int ret = 0, i, num_cpus;
	unsigned int index;

	num_cpus = num_possible_cpus();
	if (policy->cpu >= num_cpus)
		return 0;

	mutex_lock(&set_cpufreq_lock);
	if (cpufreq_suspend) {
		mutex_unlock(&set_cpufreq_lock);
		return ret;
	}

	cpufreq_frequency_table_target(policy, imx_freq_table,
			target_freq, relation, &index);
	freq_Hz = imx_freq_table[index].frequency * 1000;

	freqs.old = clk_get_rate(cpu_clk) / 1000;
	freqs.new = freq_Hz / 1000;
	freqs.cpu = policy->cpu;
	freqs.flags = 0;

	for (i = 0; i < num_cpus; i++) {
		freqs.cpu = i;
		cpufreq_notify_transition(&freqs, CPUFREQ_PRECHANGE);
	}

	ret = set_cpu_freq(freq_Hz);

	if (ret) {
		/* restore cpufreq and tell cpufreq core if set fail */
		freqs.old = clk_get_rate(cpu_clk) / 1000;
		freqs.new = freqs.old;
		freqs.cpu = policy->cpu;
		freqs.flags = 0;
		for (i = 0; i < num_cpus; i++) {
			freqs.cpu = i;
			cpufreq_notify_transition(&freqs, CPUFREQ_PRECHANGE);
		}
		goto  Set_finish;
	}

#ifdef CONFIG_SMP
	/*
	 * Loops per jiffy is not updated by the CPUFREQ driver for SMP systems.
	 * So update it for all CPUs.
	 */
	for_each_possible_cpu(i)
		per_cpu(cpu_data, i).loops_per_jiffy =
		cpufreq_scale(per_cpu(cpu_data, i).loops_per_jiffy,
		freqs.old, freqs.new);
	/*
	 * Update global loops_per_jiffy to cpu0's loops_per_jiffy,
	 * as all CPUs are running at same freq
	 */
	loops_per_jiffy = per_cpu(cpu_data, 0).loops_per_jiffy;
#endif

Set_finish:
	for (i = 0; i < num_cpus; i++) {
		freqs.cpu = i;
		cpufreq_notify_transition(&freqs, CPUFREQ_POSTCHANGE);
	}
	mutex_unlock(&set_cpufreq_lock);

	return ret;
}

static int mxc_cpufreq_init(struct cpufreq_policy *policy)
{
	int ret;
	int i;

	if (policy->cpu >= num_possible_cpus())
		return -EINVAL;

	if (imx_freq_table == NULL) {
		imx_freq_table = kmalloc(
			sizeof(struct cpufreq_frequency_table) *
			(cpu_op_nr + 1), GFP_KERNEL);
		if (!imx_freq_table) {
			ret = -ENOMEM;
			goto err0;
		}

		cpu_freq_khz_min = cpu_op_tbl[cpu_op_nr - 1].cpu_rate / 1000;
		cpu_freq_khz_max = cpu_op_tbl[cpu_op_nr - 1].cpu_rate / 1000;

		for (i = 0; i < cpu_op_nr; i++) {
			imx_freq_table[i].index = i;
			imx_freq_table[i].frequency = cpu_op_tbl[i].cpu_rate /
				1000;

			if ((cpu_op_tbl[i].cpu_rate / 1000) < cpu_freq_khz_min)
				cpu_freq_khz_min = cpu_op_tbl[i].cpu_rate /
					1000;

			if ((cpu_op_tbl[i].cpu_rate / 1000) > cpu_freq_khz_max)
				cpu_freq_khz_max = cpu_op_tbl[i].cpu_rate /
					1000;
		}

		imx_freq_table[i].index = i;
		imx_freq_table[i].frequency = CPUFREQ_TABLE_END;
	}


	policy->cur = clk_get_rate(cpu_clk) / 1000;
	policy->min = policy->cpuinfo.min_freq = cpu_freq_khz_min;
	policy->max = policy->cpuinfo.max_freq = cpu_freq_khz_max;

	/*
	 * All processors share the same frequency and voltage.
	 * So all frequencies need to be scaled together.
	 */
	if (is_smp()) {
		policy->shared_type = CPUFREQ_SHARED_TYPE_ANY;
		cpumask_setall(policy->cpus);
	}

	/* Manual states, that PLL stabilizes in two CLK32 periods */
	policy->cpuinfo.transition_latency = 2 * NANOSECOND / CLK32_FREQ;

	ret = cpufreq_frequency_table_cpuinfo(policy, imx_freq_table);

	if (ret < 0) {
		pr_err("%s: failed to register i.MXC CPUfreq with\
			error code %d\n", __func__, ret);
		ret = -ENOMEM;
		goto err0;
	}

	cpufreq_frequency_table_get_attr(imx_freq_table, policy->cpu);
	return 0;
err0:
	kfree(imx_freq_table);
	return ret;
}

static int mxc_cpufreq_exit(struct cpufreq_policy *policy)
{
	cpufreq_frequency_table_put_attr(policy->cpu);
	if (policy->cpu == 0) {
		set_cpu_freq(cpu_freq_khz_max * 1000);
		clk_put(cpu_clk);
		kfree(imx_freq_table);
	}
	return 0;
}

static struct cpufreq_driver mxc_driver = {
	.flags = CPUFREQ_STICKY,
	.verify = mxc_verify_speed,
	.target = mxc_set_target,
	.get = mxc_get_speed,
	.init = mxc_cpufreq_init,
	.exit = mxc_cpufreq_exit,
	.name = "imx",
};

static int cpufreq_pm_notify(struct notifier_block *nb, unsigned long event,
	void *dummy)
{
	unsigned int i;
	int num_cpus;
	int ret;
	struct cpufreq_freqs freqs;

	num_cpus = num_possible_cpus();
	mutex_lock(&set_cpufreq_lock);
	if (event == PM_SUSPEND_PREPARE) {
		pre_suspend_rate = clk_get_rate(cpu_clk);
		if (pre_suspend_rate != (imx_freq_table[cpu_op_nr - 1].frequency
			* 1000)) {
			/*
			 * notify cpufreq core will raise up cpufreq to highest
			 */
			freqs.old = pre_suspend_rate / 1000;
			freqs.new = imx_freq_table[cpu_op_nr - 1].frequency;
			freqs.flags = 0;
			for (i = 0; i < num_cpus; i++) {
				freqs.cpu = i;
				cpufreq_notify_transition(&freqs, CPUFREQ_PRECHANGE);
			}
			ret = set_cpu_freq(imx_freq_table[cpu_op_nr - 1].frequency
				* 1000);
			/* restore cpufreq and tell cpufreq core if set fail */
			if (ret) {
				freqs.old =  clk_get_rate(cpu_clk)/1000;
				freqs.new = freqs.old;
				freqs.flags = 0;
				for (i = 0; i < num_cpus; i++) {
					freqs.cpu = i;
					cpufreq_notify_transition(&freqs, CPUFREQ_PRECHANGE);
				}
				/* if update freq error,return */
				goto Notify_finish;
			}
#ifdef CONFIG_SMP
			for_each_possible_cpu(i)
				per_cpu(cpu_data, i).loops_per_jiffy =
					cpufreq_scale(per_cpu(cpu_data, i).loops_per_jiffy,
					pre_suspend_rate / 1000,
					imx_freq_table[cpu_op_nr - 1].frequency);
			loops_per_jiffy = per_cpu(cpu_data, 0).loops_per_jiffy;
#else
			loops_per_jiffy = cpufreq_scale(loops_per_jiffy,
				pre_suspend_rate / 1000,
				imx_freq_table[cpu_op_nr - 1].frequency);
#endif
			for (i = 0; i < num_cpus; i++) {
				freqs.cpu = i;
				cpufreq_notify_transition(&freqs, CPUFREQ_POSTCHANGE);
			}
		}
		cpufreq_suspend = true;
	} else if (event == PM_POST_SUSPEND) {
		if (clk_get_rate(cpu_clk) != pre_suspend_rate) {
			/*
			 * notify cpufreq core will restore rate before suspend
			 */
			freqs.old = imx_freq_table[cpu_op_nr - 1].frequency;
			freqs.new = pre_suspend_rate / 1000;
			freqs.flags = 0;
			for (i = 0; i < num_cpus; i++) {
				freqs.cpu = i;
				cpufreq_notify_transition(&freqs, CPUFREQ_PRECHANGE);
			}
			ret = set_cpu_freq(pre_suspend_rate);
			/* restore cpufreq and tell cpufreq core if set fail */
			if (ret) {
				freqs.old =  clk_get_rate(cpu_clk)/1000;
				freqs.new = freqs.old;
				freqs.flags = 0;
				for (i = 0; i < num_cpus; i++) {
					freqs.cpu = i;
					cpufreq_notify_transition(&freqs, CPUFREQ_PRECHANGE);
				}
				/*if update freq error,return*/
				goto Notify_finish;
			}
#ifdef CONFIG_SMP
			for_each_possible_cpu(i)
				per_cpu(cpu_data, i).loops_per_jiffy =
					cpufreq_scale(per_cpu(cpu_data, i).loops_per_jiffy,
					imx_freq_table[cpu_op_nr - 1].frequency,
					pre_suspend_rate / 1000);
			loops_per_jiffy = per_cpu(cpu_data, 0).loops_per_jiffy;
#else
			loops_per_jiffy = cpufreq_scale(loops_per_jiffy,
				imx_freq_table[cpu_op_nr - 1].frequency,
				pre_suspend_rate / 1000);
#endif
			for (i = 0; i < num_cpus; i++) {
				freqs.cpu = i;
				cpufreq_notify_transition(&freqs, CPUFREQ_POSTCHANGE);
			}
		}
		cpufreq_suspend = false;
	}
	mutex_unlock(&set_cpufreq_lock);
	return NOTIFY_OK;

Notify_finish:
	for (i = 0; i < num_cpus; i++) {
		freqs.cpu = i;
		cpufreq_notify_transition(&freqs, CPUFREQ_POSTCHANGE);
	}
	mutex_unlock(&set_cpufreq_lock);
	return NOTIFY_OK;
}

static struct notifier_block imx_cpufreq_pm_notifier = {
	.notifier_call = cpufreq_pm_notify,
};

static int __devinit cpufreq_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct device_node *np;
	void __iomem *base;
	int offset = 0;
	int i;

	np = of_find_node_by_name(NULL, "ocotp");
	base = of_iomap(np, 0);
	WARN_ON(!base);

	if (cpu_is_imx6q() || cpu_is_imx6dl()) {
		unsigned int reg;
		/*
		 * read fuse bit to know the max cpu freq : offset 0x440
		 * bit[17:16]:SPEED_GRADING[1:0]
		 */
		reg = __raw_readl(base + SPEED_FUSE);
		reg &= (0x3 << OCOTP_SPEED_BIT_OFFSET);
		reg >>= OCOTP_SPEED_BIT_OFFSET;
		/*
		 * choose the little value to run lower max cpufreq if the flag
		 * is overwrited by cmdline, else get speed from fuse bit
		 */
		if (arm_max_freq != CPU_AT_DEFAULT)
			arm_max_freq = (reg > arm_max_freq) ? arm_max_freq
					: reg;
		else
			arm_max_freq = reg;
	} else if (arm_max_freq == CPU_AT_DEFAULT) {
		/* mx6sl max freq is 1Ghz default */
		arm_max_freq = CPU_AT_1GHz;
	} else if (arm_max_freq == CPU_AT_1_2GHz) {
		pr_info("This chip didn't support 1.2GHz!please check \
			your cmdline \n");
	}
	pr_info("Max freq is %s\n", (arm_max_freq == CPU_AT_1_2GHz) ?
		"1.2GHz" : ((arm_max_freq == CPU_AT_1GHz) ? "1Ghz" : "800Mhz"));

	if (ldo_bypass_mode == LDO_MODE_DEFAULT) {
		ret = of_property_read_u32(pdev->dev.of_node, "bypass-mode",
						&ldo_bypass_mode);
		if (ret) {
			pr_info("no bypass-mode,force to enable LDO\n");
			ldo_bypass_mode = LDO_MODE_ENABLED;
		} else {
			pr_info("get bypass-mode from dts,not cmdline:\
				%d", ldo_bypass_mode);
		}
	}

	/*
	 * Generally ldo_bypass use external regulator from dts, but one
	 * exception for 1.2G on Sabresd: In this case, we will raise VDDARM_IN
	 * /VDDSOC_IN from 1.375V to 1.425V by programing these external
	 * regulator firstly, although it works as ldo_enable mode.
	 */
	if (ldo_bypass_mode == LDO_MODE_BYPASSED ||
		arm_max_freq == CPU_AT_1_2GHz) {
		arm_regulator = devm_regulator_get(&pdev->dev, "VDDARM");
		if (IS_ERR(arm_regulator)) {
			pr_warning("failed to get external arm regulator\n");
			return -ENODEV;
		}

		soc_regulator = devm_regulator_get(&pdev->dev, "VDDSOC");
		if (IS_ERR(soc_regulator)) {
			pr_warning("failed to get external soc regulator\n");
			return -ENODEV;
		}
		pu_regulator = devm_regulator_get(&pdev->dev, "VDDPU");
		if (IS_ERR(pu_regulator))
			pr_warning("failed to get external pu regulator\n");
	}

	/* force LDO enabled mode in 1.2Ghz board */
	if (arm_max_freq == CPU_AT_1_2GHz) {
		/*
		 * For 1.2G chip only work on LDO enable mode and VDDARM_IN/
		 * VDDSOC_IN need be increased form 1.375V to 1.425V for ldo
		 * bypass mode
		 */
		pr_info("Force to LDO ENABLE mode on 1.2G chip.\n");
		if (!IS_ERR(arm_regulator)) {
			 ret = regulator_set_voltage(arm_regulator, 1425000,
							1425000);
			 if (ret < 0) {
				pr_err("COULD NOT SET  VDDARM_IN\n");
				return ret;
			}
		}
		if (!IS_ERR(soc_regulator)) {
			ret = regulator_set_voltage(soc_regulator, 1425000,
							1425000);
			if (ret < 0) {
				pr_err("COULD NOT SET VDDSOC!!!!\n");
				return ret;
			}
		}
		ldo_bypass_mode = LDO_MODE_ENABLED;
	}
	if (ldo_bypass_mode == LDO_MODE_ENABLED) {
		/* use internal anatop regulator */
		pr_info("use internal regulator!\n");
		arm_regulator = regulator_get(NULL, "cpu");
		if (IS_ERR(arm_regulator)) {
			pr_err("failed to get arm regulator\n");
			return PTR_ERR(arm_regulator);
		}
		soc_regulator = regulator_get(NULL, "vddsoc");
		if (IS_ERR(soc_regulator)) {
			pr_err("failed to get soc regulator\n");
			return PTR_ERR(soc_regulator);
		}
		pu_regulator = regulator_get(NULL, "vddpu");
		if (IS_ERR(pu_regulator)) {
			pr_err("failed to get pu regulator\n");
			return PTR_ERR(pu_regulator);
		}
	} else {
		pr_info("bypass internal regulator!\n");
		/*
		 * For chip boot on 800Mhz, decrease VDDARM_IN/VDDSOC_IN
		 * from 1.375V to 1.3V for ldo bypass mode
		 */
		if (!IS_ERR(arm_regulator)) {
			 ret = regulator_set_voltage(arm_regulator, 1300000,
							1300000);
			 if (ret < 0) {
				pr_err("COULD NOT SET  VDDARM_IN\n");
				return ret;
			}
		}
		if (!IS_ERR(soc_regulator)) {
			ret = regulator_set_voltage(soc_regulator, 1300000,
							1300000);
			if (ret < 0) {
				pr_err("COULD NOT SET VDDSOC!!!!\n");
				return ret;
			}
		}

		/* digital bypass VDDPU/VDDSOC/VDDARM */
		imx_anatop_bypass_ldo();
	}

	pll1_sys = devm_clk_get(&pdev->dev, "pll1_sys");
	if (IS_ERR(pll1_sys)) {
		pr_err("%s: failed to get pll1_sys\n", __func__);
		ret = PTR_ERR(pll1_sys);
		goto err5;
	}

	pll1_sw = devm_clk_get(&pdev->dev, "pll1_sw");
	if (IS_ERR(pll1_sw)) {
		pr_err("%s: failed to get pll1_sw\n", __func__);
		ret = PTR_ERR(pll1_sw);
		goto err4;
	}

	step = devm_clk_get(&pdev->dev, "step");
	if (IS_ERR(step)) {
		pr_err("%s: failed to get step\n", __func__);
		ret = PTR_ERR(step);
		goto err3;
	}

	pll2_pfd2_396m = devm_clk_get(&pdev->dev, "pll2_pfd2_396m");
	if (IS_ERR(pll2_pfd2_396m)) {
		pr_err("%s: failed to get pll2_pfd2_396m\n", __func__);
		ret = PTR_ERR(pll2_pfd2_396m);
		goto err2;
	}

	cpu_clk = devm_clk_get(&pdev->dev, "arm");
	if (IS_ERR(cpu_clk)) {
		pr_err("%s: failed to get cpu clock\n", __func__);
		ret = PTR_ERR(cpu_clk);
		goto err1;
	}

	mutex_init(&set_cpufreq_lock);
	register_pm_notifier(&imx_cpufreq_pm_notifier);

	np = of_find_node_by_name(NULL, "cpufreq-setpoint");
	if (!np) {
		pr_err("%s: failed to find device tree data!\n",
			__func__);
		return -EINVAL;
	}

	ret = of_property_read_u32(np, "setpoint-number", &cpu_op_nr);
	if (ret) {
		pr_err("%s: failed to get setpoint number!\n",
			__func__);
		return -EINVAL;
	}
	/* 4 setpoint-number for mx6q ,3 setpoint-number for mx6dl/sl */
	if (cpu_op_nr == 4)
		/* use to calculate the real setpoint */
		offset = 1;

	cpu_op_tbl = kzalloc(sizeof(*cpu_op_tbl) * cpu_op_nr, GFP_KERNEL);
	if (!cpu_op_tbl) {
		ret = -ENOMEM;
		goto err6;
	}

	i = 0;
	for_each_compatible_node(np, NULL, "setpoint-table") {
		if (of_property_read_u32(np, "pll_rate",
			&(cpu_op_tbl[i].pll_rate)))
			continue;
		if (of_property_read_u32(np, "cpu_rate",
			&(cpu_op_tbl[i].cpu_rate)))
			continue;
		if (of_property_read_u32(np, "cpu_podf",
			&(cpu_op_tbl[i].cpu_podf)))
			continue;
		if (of_property_read_u32(np, "pu_voltage",
			&(cpu_op_tbl[i].pu_voltage)))
			continue;
		if (of_property_read_u32(np, "soc_voltage",
			&(cpu_op_tbl[i].soc_voltage)))
			continue;
		if (of_property_read_u32(np, "cpu_voltage",
			&(cpu_op_tbl[i].cpu_voltage)))
			continue;
		i++;
	}

	/*
	 * mx6q:remove one setpoint if maxfreq is 1G, remove two if 800Mhz
	 * mx6dl/sl:remove none if maxfreq is 1G, remove one if 800Mhz
	 */
	if (arm_max_freq == CPU_AT_1GHz)
		cpu_op_nr -= offset;
	else if (arm_max_freq == CPU_AT_800MHz)
		cpu_op_nr -= (offset + 1);
	pr_info("cpufreq support %d setpoint:\n", cpu_op_nr);

	for (i = 0; i < cpu_op_nr; i++) {
		pr_info("%d, %d, %d, %d, %d, %d\n",
			cpu_op_tbl[i].pll_rate, cpu_op_tbl[i].cpu_rate,
			cpu_op_tbl[i].cpu_podf, cpu_op_tbl[i].pu_voltage,
			cpu_op_tbl[i].soc_voltage,
			cpu_op_tbl[i].cpu_voltage);
	}

	/* prepare and enable pll1 clock to make use count right */
	clk_prepare(pll1_sys);
	clk_enable(pll1_sys);

	ret = cpufreq_register_driver(&mxc_driver);
	if (ret) {
		pr_err("failed to register imx cpufreq driver!\n");
		unregister_pm_notifier(&imx_cpufreq_pm_notifier);
		goto err1;
	}

	return  0;

err1:
	devm_clk_put(&pdev->dev, cpu_clk);
err2:
	devm_clk_put(&pdev->dev, pll2_pfd2_396m);
err3:
	devm_clk_put(&pdev->dev, step);
err4:
	devm_clk_put(&pdev->dev, pll1_sw);
err5:
	devm_clk_put(&pdev->dev, pll1_sys);
err6:
	kfree(cpu_op_tbl);
	return ret;
}

static const struct of_device_id imx6_cpufreq_ids[] = {
	{ .compatible = "fsl,imx_cpufreq", },
	{ /* sentinel */ }
};

static struct platform_driver imx6_cpufreq_driver = {
	.driver = {
		   .name = "imx_cpufreq",
		   .of_match_table = imx6_cpufreq_ids,
		},
	.probe = cpufreq_probe,
};

static int __init mxc_cpufreq_driver_init(void)
{
	if (platform_driver_register(&imx6_cpufreq_driver) != 0) {
		pr_err("cpufreq_driver register failed\n");
		return -ENODEV;
	}
	pr_info("Cpu freq driver module loaded\n");
	return 0;
}

static void mxc_cpufreq_driver_exit(void)
{
	platform_driver_unregister(&imx6_cpufreq_driver);
}

static int __init enable_ldo(char *p)
{
	if (memcmp(p, "on", 2) == 0) {
		ldo_bypass_mode = LDO_MODE_ENABLED;
		p += 2;
	} else if (memcmp(p, "off", 3) == 0) {
		ldo_bypass_mode = LDO_MODE_BYPASSED;
		p += 3;
	}
	return 0;
}
early_param("ldo_active", enable_ldo);

static int __init arm_core_max(char *p)
{
	if (memcmp(p, "1200", 4) == 0) {
		arm_max_freq = CPU_AT_1_2GHz;
		p += 4;
	} else if (memcmp(p, "1000", 4) == 0) {
		arm_max_freq = CPU_AT_1GHz;
		p += 4;
	} else if (memcmp(p, "800", 3) == 0) {
		arm_max_freq = CPU_AT_800MHz;
		p += 3;
	}
	return 0;
}

early_param("arm_freq", arm_core_max);
module_init(mxc_cpufreq_driver_init);
module_exit(mxc_cpufreq_driver_exit);

MODULE_AUTHOR("Freescale Semiconductor Inc. Yong Shen <yong.shen@linaro.org>");
MODULE_DESCRIPTION("CPUfreq driver for i.MX");
MODULE_LICENSE("GPL");
