/* Copyright (c) 2011-2012, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/smp.h>
#include <linux/wakelock.h>
#include <linux/pm_qos.h>
#include <linux/sysfs.h>
#include <linux/stat.h>
#include <linux/clk.h>
#include <linux/coresight.h>
#include <asm/sections.h>
#include <mach/socinfo.h>

#include "coresight-priv.h"

#define etm_writel(drvdata, cpu, val, off)	\
			__raw_writel((val), drvdata->base + (SZ_4K * cpu) + off)
#define etm_readl(drvdata, cpu, off)	\
			__raw_readl(drvdata->base + (SZ_4K * cpu) + off)


#define ETMCR			(0x000)
#define ETMCCR			(0x004)
#define ETMTRIGGER		(0x008)
#define ETMSR			(0x010)
#define ETMSCR			(0x014)
#define ETMTSSCR		(0x018)
#define ETMTEEVR		(0x020)
#define ETMTECR1		(0x024)
#define ETMFFLR			(0x02C)
#define ETMACVRn(n)		(0x040 + (n * 4))
#define ETMACTRn(n)		(0x080 + (n * 4))
#define ETMCNTRLDVRn(n)		(0x140 + (n * 4))
#define ETMCNTENRn(n)		(0x150 + (n * 4))
#define ETMCNTRLDEVRn(n)	(0x160 + (n * 4))
#define ETMCNTVRn(n)		(0x170 + (n * 4))
#define ETMSQ12EVR		(0x180)
#define ETMSQ21EVR		(0x184)
#define ETMSQ23EVR		(0x188)
#define ETMSQ31EVR		(0x18C)
#define ETMSQ32EVR		(0x190)
#define ETMSQ13EVR		(0x194)
#define ETMSQR			(0x19C)
#define ETMEXTOUTEVRn(n)	(0x1A0 + (n * 4))
#define ETMCIDCVRn(n)		(0x1B0 + (n * 4))
#define ETMCIDCMR		(0x1BC)
#define ETMIMPSPEC0		(0x1C0)
#define ETMIMPSPEC1		(0x1C4)
#define ETMIMPSPEC2		(0x1C8)
#define ETMIMPSPEC3		(0x1CC)
#define ETMIMPSPEC4		(0x1D0)
#define ETMIMPSPEC5		(0x1D4)
#define ETMIMPSPEC6		(0x1D8)
#define ETMIMPSPEC7		(0x1DC)
#define ETMSYNCFR		(0x1E0)
#define ETMIDR			(0x1E4)
#define ETMCCER			(0x1E8)
#define ETMEXTINSELR		(0x1EC)
#define ETMTESSEICR		(0x1F0)
#define ETMEIBCR		(0x1F4)
#define ETMTSEVR		(0x1F8)
#define ETMAUXCR		(0x1FC)
#define ETMTRACEIDR		(0x200)
#define ETMVMIDCVR		(0x240)
#define ETMOSLAR		(0x300)
#define ETMOSLSR		(0x304)
#define ETMOSSRR		(0x308)
#define ETMPDCR			(0x310)
#define ETMPDSR			(0x314)

#define ETM_MAX_ADDR_CMP	(16)
#define ETM_MAX_CNTR		(4)
#define ETM_MAX_CTXID_CMP	(3)

#define ETM_MODE_EXCLUDE	BIT(0)
#define ETM_MODE_CYCACC		BIT(1)
#define ETM_MODE_STALL		BIT(2)
#define ETM_MODE_TIMESTAMP	BIT(3)
#define ETM_MODE_CTXID		BIT(4)
#define ETM_MODE_ALL		(0x1F)

#define ETM_EVENT_MASK		(0x1FFFF)
#define ETM_SYNC_MASK		(0xFFF)
#define ETM_ALL_MASK		(0xFFFFFFFF)

#define ETM_SEQ_STATE_MAX_VAL	(0x2)

enum {
	ETM_ADDR_TYPE_NONE,
	ETM_ADDR_TYPE_SINGLE,
	ETM_ADDR_TYPE_RANGE,
	ETM_ADDR_TYPE_START,
	ETM_ADDR_TYPE_STOP,
};

#define ETM_LOCK(cpu)							\
do {									\
	mb();								\
	etm_writel(drvdata, cpu, 0x0, CORESIGHT_LAR);			\
} while (0)
#define ETM_UNLOCK(cpu)							\
do {									\
	etm_writel(drvdata, cpu, CORESIGHT_UNLOCK, CORESIGHT_LAR);	\
	mb();								\
} while (0)


#ifdef MODULE_PARAM_PREFIX
#undef MODULE_PARAM_PREFIX
#endif
#define MODULE_PARAM_PREFIX "coresight."

#ifdef CONFIG_MSM_QDSS_ETM_DEFAULT_ENABLE
static int etm_boot_enable = 1;
#else
static int etm_boot_enable;
#endif
module_param_named(
	etm_boot_enable, etm_boot_enable, int, S_IRUGO
);

struct etm_drvdata {
	void __iomem			*base;
	bool				enabled;
	struct wake_lock		wake_lock;
	struct pm_qos_request		qos_req;
	struct qdss_source		*src;
	struct mutex			mutex;
	struct device			*dev;
	struct kobject			*kobj;
	struct clk			*clk;
	uint8_t				arch;
	uint8_t				nr_addr_cmp;
	uint8_t				nr_cntr;
	uint8_t				nr_ext_inp;
	uint8_t				nr_ext_out;
	uint8_t				nr_ctxid_cmp;
	uint8_t				reset;
	uint32_t			mode;
	uint32_t			ctrl;
	uint32_t			trigger_event;
	uint32_t			startstop_ctrl;
	uint32_t			enable_event;
	uint32_t			enable_ctrl1;
	uint32_t			fifofull_level;
	uint8_t				addr_idx;
	uint32_t			addr_val[ETM_MAX_ADDR_CMP];
	uint32_t			addr_acctype[ETM_MAX_ADDR_CMP];
	uint32_t			addr_type[ETM_MAX_ADDR_CMP];
	uint8_t				cntr_idx;
	uint32_t			cntr_rld_val[ETM_MAX_CNTR];
	uint32_t			cntr_event[ETM_MAX_CNTR];
	uint32_t			cntr_rld_event[ETM_MAX_CNTR];
	uint32_t			cntr_val[ETM_MAX_CNTR];
	uint32_t			seq_12_event;
	uint32_t			seq_21_event;
	uint32_t			seq_23_event;
	uint32_t			seq_31_event;
	uint32_t			seq_32_event;
	uint32_t			seq_13_event;
	uint32_t			seq_curr_state;
	uint8_t				ctxid_idx;
	uint32_t			ctxid_val[ETM_MAX_CTXID_CMP];
	uint32_t			ctxid_mask;
	uint32_t			sync_freq;
	uint32_t			timestamp_event;
};

static struct etm_drvdata *drvdata;


static void etm_set_pwrdwn(int cpu)
{
	uint32_t etmcr;

	etmcr = etm_readl(drvdata, cpu, ETMCR);
	etmcr |= BIT(0);
	etm_writel(drvdata, cpu, etmcr, ETMCR);
}

static void etm_clr_pwrdwn(int cpu)
{
	uint32_t etmcr;

	etmcr = etm_readl(drvdata, cpu, ETMCR);
	etmcr &= ~BIT(0);
	etm_writel(drvdata, cpu, etmcr, ETMCR);
}

static void etm_set_prog(int cpu)
{
	uint32_t etmcr;
	int count;

	etmcr = etm_readl(drvdata, cpu, ETMCR);
	etmcr |= BIT(10);
	etm_writel(drvdata, cpu, etmcr, ETMCR);

	for (count = TIMEOUT_US; BVAL(etm_readl(drvdata, cpu, ETMSR), 1) != 1
				&& count > 0; count--)
		udelay(1);
	WARN(count == 0, "timeout while setting prog bit, ETMSR: %#x\n",
	     etm_readl(drvdata, cpu, ETMSR));
}

static void etm_clr_prog(int cpu)
{
	uint32_t etmcr;
	int count;

	etmcr = etm_readl(drvdata, cpu, ETMCR);
	etmcr &= ~BIT(10);
	etm_writel(drvdata, cpu, etmcr, ETMCR);

	for (count = TIMEOUT_US; BVAL(etm_readl(drvdata, cpu, ETMSR), 1) != 0
				&& count > 0; count--)
		udelay(1);
	WARN(count == 0, "timeout while clearing prog bit, ETMSR: %#x\n",
	     etm_readl(drvdata, cpu, ETMSR));
}

static void __etm_enable(int cpu)
{
	int i;

	ETM_UNLOCK(cpu);
	
	etm_clr_pwrdwn(cpu);
	etm_set_prog(cpu);

	etm_writel(drvdata, cpu, drvdata->ctrl | BIT(10), ETMCR);
	etm_writel(drvdata, cpu, drvdata->trigger_event, ETMTRIGGER);
	etm_writel(drvdata, cpu, drvdata->startstop_ctrl, ETMTSSCR);
	etm_writel(drvdata, cpu, drvdata->enable_event, ETMTEEVR);
	etm_writel(drvdata, cpu, drvdata->enable_ctrl1, ETMTECR1);
	etm_writel(drvdata, cpu, drvdata->fifofull_level, ETMFFLR);
	for (i = 0; i < drvdata->nr_addr_cmp; i++) {
		etm_writel(drvdata, cpu, drvdata->addr_val[i], ETMACVRn(i));
		etm_writel(drvdata, cpu, drvdata->addr_acctype[i], ETMACTRn(i));
	}
	for (i = 0; i < drvdata->nr_cntr; i++) {
		etm_writel(drvdata, cpu, drvdata->cntr_rld_val[i],
			   ETMCNTRLDVRn(i));
		etm_writel(drvdata, cpu, drvdata->cntr_event[i], ETMCNTENRn(i));
		etm_writel(drvdata, cpu, drvdata->cntr_rld_event[i],
			   ETMCNTRLDEVRn(i));
		etm_writel(drvdata, cpu, drvdata->cntr_val[i], ETMCNTVRn(i));
	}
	etm_writel(drvdata, cpu, drvdata->seq_12_event, ETMSQ12EVR);
	etm_writel(drvdata, cpu, drvdata->seq_21_event, ETMSQ21EVR);
	etm_writel(drvdata, cpu, drvdata->seq_23_event, ETMSQ23EVR);
	etm_writel(drvdata, cpu, drvdata->seq_31_event, ETMSQ31EVR);
	etm_writel(drvdata, cpu, drvdata->seq_32_event, ETMSQ32EVR);
	etm_writel(drvdata, cpu, drvdata->seq_13_event, ETMSQ13EVR);
	etm_writel(drvdata, cpu, drvdata->seq_curr_state, ETMSQR);
	for (i = 0; i < drvdata->nr_ext_out; i++)
		etm_writel(drvdata, cpu, 0x0000406F, ETMEXTOUTEVRn(i));
	for (i = 0; i < drvdata->nr_ctxid_cmp; i++)
		etm_writel(drvdata, cpu, drvdata->ctxid_val[i], ETMCIDCVRn(i));
	etm_writel(drvdata, cpu, drvdata->ctxid_mask, ETMCIDCMR);
	etm_writel(drvdata, cpu, drvdata->sync_freq, ETMSYNCFR);
	etm_writel(drvdata, cpu, 0x00000000, ETMEXTINSELR);
	etm_writel(drvdata, cpu, drvdata->timestamp_event, ETMTSEVR);
	etm_writel(drvdata, cpu, 0x00000000, ETMAUXCR);
	etm_writel(drvdata, cpu, cpu+1, ETMTRACEIDR);
	etm_writel(drvdata, cpu, 0x00000000, ETMVMIDCVR);

	etm_clr_prog(cpu);
	ETM_LOCK(cpu);
}

static int etm_enable(void)
{
	int ret, cpu;

	if (drvdata->enabled) {
		dev_err(drvdata->dev, "ETM tracing already enabled\n");
		ret = -EPERM;
		goto err;
	}

	wake_lock(&drvdata->wake_lock);
	pm_qos_update_request(&drvdata->qos_req, 0);

	ret = clk_prepare_enable(drvdata->clk);
	if (ret)
		goto err_clk;

	ret = qdss_enable(drvdata->src);
	if (ret)
		goto err_qdss;

	for_each_online_cpu(cpu)
		__etm_enable(cpu);

	drvdata->enabled = true;

	pm_qos_update_request(&drvdata->qos_req, PM_QOS_DEFAULT_VALUE);
	wake_unlock(&drvdata->wake_lock);

	dev_info(drvdata->dev, "ETM tracing enabled\n");
	return 0;

err_qdss:
	clk_disable_unprepare(drvdata->clk);
err_clk:
	pm_qos_update_request(&drvdata->qos_req, PM_QOS_DEFAULT_VALUE);
	wake_unlock(&drvdata->wake_lock);
err:
	return ret;
}

static void __etm_disable(int cpu)
{
	ETM_UNLOCK(cpu);
	etm_set_prog(cpu);

	
	etm_writel(drvdata, cpu, 0x6F | BIT(14), ETMTEEVR);

	
	etm_set_pwrdwn(cpu);
	ETM_LOCK(cpu);
}

static int etm_disable(void)
{
	int ret, cpu;

	if (!drvdata->enabled) {
		dev_err(drvdata->dev, "ETM tracing already disabled\n");
		ret = -EPERM;
		goto err;
	}

	wake_lock(&drvdata->wake_lock);
	pm_qos_update_request(&drvdata->qos_req, 0);

	for_each_online_cpu(cpu)
		__etm_disable(cpu);

	drvdata->enabled = false;

	qdss_disable(drvdata->src);

	clk_disable_unprepare(drvdata->clk);

	pm_qos_update_request(&drvdata->qos_req, PM_QOS_DEFAULT_VALUE);
	wake_unlock(&drvdata->wake_lock);

	dev_info(drvdata->dev, "ETM tracing disabled\n");
	return 0;
err:
	return ret;
}

static void etm_os_unlock(void *unused)
{
	unsigned long value = 0x0;

	asm("mcr p14, 1, %0, c1, c0, 4\n\t" : : "r" (value));
	asm("isb\n\t");
}

static ssize_t etm_show_enabled(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	unsigned long val = drvdata->enabled;
	return scnprintf(buf, PAGE_SIZE, "%#lx\n", val);
}

static ssize_t etm_store_enabled(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t size)
{
	int ret = 0;
	unsigned long val;

	if (sscanf(buf, "%lx", &val) != 1)
		return -EINVAL;

	mutex_lock(&drvdata->mutex);
	if (val)
		ret = etm_enable();
	else
		ret = etm_disable();
	mutex_unlock(&drvdata->mutex);

	if (ret)
		return ret;
	return size;
}
static DEVICE_ATTR(enabled, S_IRUGO | S_IWUSR, etm_show_enabled,
		   etm_store_enabled);

static ssize_t etm_show_nr_addr_cmp(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	unsigned long val = drvdata->nr_addr_cmp;
	return scnprintf(buf, PAGE_SIZE, "%#lx\n", val);
}
static DEVICE_ATTR(nr_addr_cmp, S_IRUGO, etm_show_nr_addr_cmp, NULL);

static ssize_t etm_show_nr_cntr(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	unsigned long val = drvdata->nr_cntr;
	return scnprintf(buf, PAGE_SIZE, "%#lx\n", val);
}
static DEVICE_ATTR(nr_cntr, S_IRUGO, etm_show_nr_cntr, NULL);

static ssize_t etm_show_nr_ctxid_cmp(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	unsigned long val = drvdata->nr_ctxid_cmp;
	return scnprintf(buf, PAGE_SIZE, "%#lx\n", val);
}
static DEVICE_ATTR(nr_ctxid_cmp, S_IRUGO, etm_show_nr_ctxid_cmp, NULL);

static ssize_t etm_show_reset(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	unsigned long val = drvdata->reset;
	return scnprintf(buf, PAGE_SIZE, "%#lx\n", val);
}

static ssize_t etm_store_reset(struct device *dev,
			       struct device_attribute *attr, const char *buf,
			       size_t size)
{
	int i;
	unsigned long val;

	if (sscanf(buf, "%lx", &val) != 1)
		return -EINVAL;

	mutex_lock(&drvdata->mutex);
	if (val) {
		drvdata->mode = ETM_MODE_EXCLUDE;
		drvdata->ctrl = 0x0;
		if (cpu_is_krait_v1()) {
			drvdata->mode |= ETM_MODE_CYCACC;
			drvdata->ctrl |= BIT(12);
		}
		drvdata->trigger_event = 0x406F;
		drvdata->startstop_ctrl = 0x0;
		drvdata->enable_event = 0x6F;
		drvdata->enable_ctrl1 = 0x1000000;
		drvdata->fifofull_level = 0x28;
		drvdata->addr_idx = 0x0;
		for (i = 0; i < drvdata->nr_addr_cmp; i++) {
			drvdata->addr_val[i] = 0x0;
			drvdata->addr_acctype[i] = 0x0;
			drvdata->addr_type[i] = ETM_ADDR_TYPE_NONE;
		}
		drvdata->cntr_idx = 0x0;
		for (i = 0; i < drvdata->nr_cntr; i++) {
			drvdata->cntr_rld_val[i] = 0x0;
			drvdata->cntr_event[i] = 0x406F;
			drvdata->cntr_rld_event[i] = 0x406F;
			drvdata->cntr_val[i] = 0x0;
		}
		drvdata->seq_12_event = 0x406F;
		drvdata->seq_21_event = 0x406F;
		drvdata->seq_23_event = 0x406F;
		drvdata->seq_31_event = 0x406F;
		drvdata->seq_32_event = 0x406F;
		drvdata->seq_13_event = 0x406F;
		drvdata->seq_curr_state = 0x0;
		drvdata->ctxid_idx = 0x0;
		for (i = 0; i < drvdata->nr_ctxid_cmp; i++)
			drvdata->ctxid_val[i] = 0x0;
		drvdata->ctxid_mask = 0x0;
		drvdata->sync_freq = 0x80;
		drvdata->timestamp_event = 0x406F;
	}
	mutex_unlock(&drvdata->mutex);
	return size;
}
static DEVICE_ATTR(reset, S_IRUGO | S_IWUSR, etm_show_reset, etm_store_reset);

static ssize_t etm_show_mode(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	unsigned long val = drvdata->mode;
	return scnprintf(buf, PAGE_SIZE, "%#lx\n", val);
}

static ssize_t etm_store_mode(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t size)
{
	unsigned long val;

	if (sscanf(buf, "%lx", &val) != 1)
		return -EINVAL;

	mutex_lock(&drvdata->mutex);
	drvdata->mode = val & ETM_MODE_ALL;

	if (drvdata->mode & ETM_MODE_EXCLUDE)
		drvdata->enable_ctrl1 |= BIT(24);
	else
		drvdata->enable_ctrl1 &= ~BIT(24);

	if (drvdata->mode & ETM_MODE_CYCACC)
		drvdata->ctrl |= BIT(12);
	else
		drvdata->ctrl &= ~BIT(12);

	if (drvdata->mode & ETM_MODE_STALL)
		drvdata->ctrl |= BIT(7);
	else
		drvdata->ctrl &= ~BIT(7);

	if (drvdata->mode & ETM_MODE_TIMESTAMP)
		drvdata->ctrl |= BIT(28);
	else
		drvdata->ctrl &= ~BIT(28);
	if (drvdata->mode & ETM_MODE_CTXID)
		drvdata->ctrl |= (BIT(14) | BIT(15));
	else
		drvdata->ctrl &= ~(BIT(14) | BIT(15));
	mutex_unlock(&drvdata->mutex);

	return size;
}
static DEVICE_ATTR(mode, S_IRUGO | S_IWUSR, etm_show_mode, etm_store_mode);

static ssize_t etm_show_trigger_event(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	unsigned long val = drvdata->trigger_event;
	return scnprintf(buf, PAGE_SIZE, "%#lx\n", val);
}

static ssize_t etm_store_trigger_event(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t size)
{
	unsigned long val;

	if (sscanf(buf, "%lx", &val) != 1)
		return -EINVAL;

	drvdata->trigger_event = val & ETM_EVENT_MASK;
	return size;
}
static DEVICE_ATTR(trigger_event, S_IRUGO | S_IWUSR, etm_show_trigger_event,
		   etm_store_trigger_event);

static ssize_t etm_show_enable_event(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	unsigned long val = drvdata->enable_event;
	return scnprintf(buf, PAGE_SIZE, "%#lx\n", val);
}

static ssize_t etm_store_enable_event(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t size)
{
	unsigned long val;

	if (sscanf(buf, "%lx", &val) != 1)
		return -EINVAL;

	drvdata->enable_event = val & ETM_EVENT_MASK;
	return size;
}
static DEVICE_ATTR(enable_event, S_IRUGO | S_IWUSR, etm_show_enable_event,
		   etm_store_enable_event);

static ssize_t etm_show_fifofull_level(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	unsigned long val = drvdata->fifofull_level;
	return scnprintf(buf, PAGE_SIZE, "%#lx\n", val);
}

static ssize_t etm_store_fifofull_level(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t size)
{
	unsigned long val;

	if (sscanf(buf, "%lx", &val) != 1)
		return -EINVAL;

	drvdata->fifofull_level = val;
	return size;
}
static DEVICE_ATTR(fifofull_level, S_IRUGO | S_IWUSR, etm_show_fifofull_level,
		   etm_store_fifofull_level);

static ssize_t etm_show_addr_idx(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	unsigned long val = drvdata->addr_idx;
	return scnprintf(buf, PAGE_SIZE, "%#lx\n", val);
}

static ssize_t etm_store_addr_idx(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t size)
{
	unsigned long val;

	if (sscanf(buf, "%lx", &val) != 1)
		return -EINVAL;
	if (val >= drvdata->nr_addr_cmp)
		return -EINVAL;

	mutex_lock(&drvdata->mutex);
	drvdata->addr_idx = val;
	mutex_unlock(&drvdata->mutex);
	return size;
}
static DEVICE_ATTR(addr_idx, S_IRUGO | S_IWUSR, etm_show_addr_idx,
		   etm_store_addr_idx);

static ssize_t etm_show_addr_single(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	unsigned long val;
	uint8_t idx;

	mutex_lock(&drvdata->mutex);
	idx = drvdata->addr_idx;
	if (!(drvdata->addr_type[idx] == ETM_ADDR_TYPE_NONE ||
	      drvdata->addr_type[idx] == ETM_ADDR_TYPE_SINGLE)) {
		mutex_unlock(&drvdata->mutex);
		return -EPERM;
	}

	val = drvdata->addr_val[idx];
	mutex_unlock(&drvdata->mutex);
	return scnprintf(buf, PAGE_SIZE, "%#lx\n", val);
}

static ssize_t etm_store_addr_single(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t size)
{
	unsigned long val;
	uint8_t idx;

	if (sscanf(buf, "%lx", &val) != 1)
		return -EINVAL;

	mutex_lock(&drvdata->mutex);
	idx = drvdata->addr_idx;
	if (!(drvdata->addr_type[idx] == ETM_ADDR_TYPE_NONE ||
	      drvdata->addr_type[idx] == ETM_ADDR_TYPE_SINGLE)) {
		mutex_unlock(&drvdata->mutex);
		return -EPERM;
	}

	drvdata->addr_val[idx] = val;
	drvdata->addr_type[idx] = ETM_ADDR_TYPE_SINGLE;
	mutex_unlock(&drvdata->mutex);
	return size;
}
static DEVICE_ATTR(addr_single, S_IRUGO | S_IWUSR, etm_show_addr_single,
		   etm_store_addr_single);

static ssize_t etm_show_addr_range(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	unsigned long val1, val2;
	uint8_t idx;

	mutex_lock(&drvdata->mutex);
	idx = drvdata->addr_idx;
	if (idx % 2 != 0) {
		mutex_unlock(&drvdata->mutex);
		return -EPERM;
	}
	if (!((drvdata->addr_type[idx] == ETM_ADDR_TYPE_NONE &&
	       drvdata->addr_type[idx + 1] == ETM_ADDR_TYPE_NONE) ||
	      (drvdata->addr_type[idx] == ETM_ADDR_TYPE_RANGE &&
	       drvdata->addr_type[idx + 1] == ETM_ADDR_TYPE_RANGE))) {
		mutex_unlock(&drvdata->mutex);
		return -EPERM;
	}

	val1 = drvdata->addr_val[idx];
	val2 = drvdata->addr_val[idx + 1];
	mutex_unlock(&drvdata->mutex);
	return scnprintf(buf, PAGE_SIZE, "%#lx %#lx\n", val1, val2);
}

static ssize_t etm_store_addr_range(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t size)
{
	unsigned long val1, val2;
	uint8_t idx;

	if (sscanf(buf, "%lx %lx", &val1, &val2) != 2)
		return -EINVAL;
	
	if (val1 > val2)
		return -EINVAL;

	mutex_lock(&drvdata->mutex);
	idx = drvdata->addr_idx;
	if (idx % 2 != 0) {
		mutex_unlock(&drvdata->mutex);
		return -EPERM;
	}
	if (!((drvdata->addr_type[idx] == ETM_ADDR_TYPE_NONE &&
	       drvdata->addr_type[idx + 1] == ETM_ADDR_TYPE_NONE) ||
	      (drvdata->addr_type[idx] == ETM_ADDR_TYPE_RANGE &&
	       drvdata->addr_type[idx + 1] == ETM_ADDR_TYPE_RANGE))) {
		mutex_unlock(&drvdata->mutex);
		return -EPERM;
	}

	drvdata->addr_val[idx] = val1;
	drvdata->addr_type[idx] = ETM_ADDR_TYPE_RANGE;
	drvdata->addr_val[idx + 1] = val2;
	drvdata->addr_type[idx + 1] = ETM_ADDR_TYPE_RANGE;
	drvdata->enable_ctrl1 |= (1 << (idx/2));
	mutex_unlock(&drvdata->mutex);
	return size;
}
static DEVICE_ATTR(addr_range, S_IRUGO | S_IWUSR, etm_show_addr_range,
		   etm_store_addr_range);

static ssize_t etm_show_addr_start(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	unsigned long val;
	uint8_t idx;

	mutex_lock(&drvdata->mutex);
	idx = drvdata->addr_idx;
	if (!(drvdata->addr_type[idx] == ETM_ADDR_TYPE_NONE ||
	      drvdata->addr_type[idx] == ETM_ADDR_TYPE_START)) {
		mutex_unlock(&drvdata->mutex);
		return -EPERM;
	}

	val = drvdata->addr_val[idx];
	mutex_unlock(&drvdata->mutex);
	return scnprintf(buf, PAGE_SIZE, "%#lx\n", val);
}

static ssize_t etm_store_addr_start(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t size)
{
	unsigned long val;
	uint8_t idx;

	if (sscanf(buf, "%lx", &val) != 1)
		return -EINVAL;

	mutex_lock(&drvdata->mutex);
	idx = drvdata->addr_idx;
	if (!(drvdata->addr_type[idx] == ETM_ADDR_TYPE_NONE ||
	      drvdata->addr_type[idx] == ETM_ADDR_TYPE_START)) {
		mutex_unlock(&drvdata->mutex);
		return -EPERM;
	}

	drvdata->addr_val[idx] = val;
	drvdata->addr_type[idx] = ETM_ADDR_TYPE_START;
	drvdata->startstop_ctrl |= (1 << idx);
	drvdata->enable_ctrl1 |= BIT(25);
	mutex_unlock(&drvdata->mutex);
	return size;
}
static DEVICE_ATTR(addr_start, S_IRUGO | S_IWUSR, etm_show_addr_start,
		   etm_store_addr_start);

static ssize_t etm_show_addr_stop(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	unsigned long val;
	uint8_t idx;

	mutex_lock(&drvdata->mutex);
	idx = drvdata->addr_idx;
	if (!(drvdata->addr_type[idx] == ETM_ADDR_TYPE_NONE ||
	      drvdata->addr_type[idx] == ETM_ADDR_TYPE_STOP)) {
		mutex_unlock(&drvdata->mutex);
		return -EPERM;
	}

	val = drvdata->addr_val[idx];
	mutex_unlock(&drvdata->mutex);
	return scnprintf(buf, PAGE_SIZE, "%#lx\n", val);
}

static ssize_t etm_store_addr_stop(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t size)
{
	unsigned long val;
	uint8_t idx;

	if (sscanf(buf, "%lx", &val) != 1)
		return -EINVAL;

	mutex_lock(&drvdata->mutex);
	idx = drvdata->addr_idx;
	if (!(drvdata->addr_type[idx] == ETM_ADDR_TYPE_NONE ||
	      drvdata->addr_type[idx] == ETM_ADDR_TYPE_STOP)) {
		mutex_unlock(&drvdata->mutex);
		return -EPERM;
	}

	drvdata->addr_val[idx] = val;
	drvdata->addr_type[idx] = ETM_ADDR_TYPE_STOP;
	drvdata->startstop_ctrl |= (1 << (idx + 16));
	drvdata->enable_ctrl1 |= BIT(25);
	mutex_unlock(&drvdata->mutex);
	return size;
}
static DEVICE_ATTR(addr_stop, S_IRUGO | S_IWUSR, etm_show_addr_stop,
		   etm_store_addr_stop);

static ssize_t etm_show_addr_acctype(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	unsigned long val;

	mutex_lock(&drvdata->mutex);
	val = drvdata->addr_acctype[drvdata->addr_idx];
	mutex_unlock(&drvdata->mutex);
	return scnprintf(buf, PAGE_SIZE, "%#lx\n", val);
}

static ssize_t etm_store_addr_acctype(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t size)
{
	unsigned long val;

	if (sscanf(buf, "%lx", &val) != 1)
		return -EINVAL;

	mutex_lock(&drvdata->mutex);
	drvdata->addr_acctype[drvdata->addr_idx] = val;
	mutex_unlock(&drvdata->mutex);
	return size;
}
static DEVICE_ATTR(addr_acctype, S_IRUGO | S_IWUSR, etm_show_addr_acctype,
		   etm_store_addr_acctype);

static ssize_t etm_show_cntr_idx(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	unsigned long val = drvdata->addr_idx;
	return scnprintf(buf, PAGE_SIZE, "%#lx\n", val);
}

static ssize_t etm_store_cntr_idx(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t size)
{
	unsigned long val;

	if (sscanf(buf, "%lx", &val) != 1)
		return -EINVAL;
	if (val >= drvdata->nr_cntr)
		return -EINVAL;

	mutex_lock(&drvdata->mutex);
	drvdata->cntr_idx = val;
	mutex_unlock(&drvdata->mutex);
	return size;
}
static DEVICE_ATTR(cntr_idx, S_IRUGO | S_IWUSR, etm_show_cntr_idx,
		   etm_store_cntr_idx);

static ssize_t etm_show_cntr_rld_val(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	unsigned long val;
	mutex_lock(&drvdata->mutex);
	val = drvdata->cntr_rld_val[drvdata->cntr_idx];
	mutex_unlock(&drvdata->mutex);
	return scnprintf(buf, PAGE_SIZE, "%#lx\n", val);
}

static ssize_t etm_store_cntr_rld_val(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t size)
{
	unsigned long val;

	if (sscanf(buf, "%lx", &val) != 1)
		return -EINVAL;

	mutex_lock(&drvdata->mutex);
	drvdata->cntr_rld_val[drvdata->cntr_idx] = val;
	mutex_unlock(&drvdata->mutex);
	return size;
}
static DEVICE_ATTR(cntr_rld_val, S_IRUGO | S_IWUSR, etm_show_cntr_rld_val,
		   etm_store_cntr_rld_val);

static ssize_t etm_show_cntr_event(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	unsigned long val;

	mutex_lock(&drvdata->mutex);
	val = drvdata->cntr_event[drvdata->cntr_idx];
	mutex_unlock(&drvdata->mutex);
	return scnprintf(buf, PAGE_SIZE, "%#lx\n", val);
}

static ssize_t etm_store_cntr_event(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t size)
{
	unsigned long val;

	if (sscanf(buf, "%lx", &val) != 1)
		return -EINVAL;

	mutex_lock(&drvdata->mutex);
	drvdata->cntr_event[drvdata->cntr_idx] = val & ETM_EVENT_MASK;
	mutex_unlock(&drvdata->mutex);
	return size;
}
static DEVICE_ATTR(cntr_event, S_IRUGO | S_IWUSR, etm_show_cntr_event,
		   etm_store_cntr_event);

static ssize_t etm_show_cntr_rld_event(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	unsigned long val;

	mutex_lock(&drvdata->mutex);
	val = drvdata->cntr_rld_event[drvdata->cntr_idx];
	mutex_unlock(&drvdata->mutex);
	return scnprintf(buf, PAGE_SIZE, "%#lx\n", val);
}

static ssize_t etm_store_cntr_rld_event(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t size)
{
	unsigned long val;

	if (sscanf(buf, "%lx", &val) != 1)
		return -EINVAL;

	mutex_lock(&drvdata->mutex);
	drvdata->cntr_rld_event[drvdata->cntr_idx] = val & ETM_EVENT_MASK;
	mutex_unlock(&drvdata->mutex);
	return size;
}
static DEVICE_ATTR(cntr_rld_event, S_IRUGO | S_IWUSR, etm_show_cntr_rld_event,
		   etm_store_cntr_rld_event);

static ssize_t etm_show_cntr_val(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	unsigned long val;

	mutex_lock(&drvdata->mutex);
	val = drvdata->cntr_val[drvdata->cntr_idx];
	mutex_unlock(&drvdata->mutex);
	return scnprintf(buf, PAGE_SIZE, "%#lx\n", val);
}

static ssize_t etm_store_cntr_val(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t size)
{
	unsigned long val;

	if (sscanf(buf, "%lx", &val) != 1)
		return -EINVAL;

	mutex_lock(&drvdata->mutex);
	drvdata->cntr_val[drvdata->cntr_idx] = val;
	mutex_unlock(&drvdata->mutex);
	return size;
}
static DEVICE_ATTR(cntr_val, S_IRUGO | S_IWUSR, etm_show_cntr_val,
		   etm_store_cntr_val);

static ssize_t etm_show_seq_12_event(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	unsigned long val = drvdata->seq_12_event;
	return scnprintf(buf, PAGE_SIZE, "%#lx\n", val);
}

static ssize_t etm_store_seq_12_event(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t size)
{
	unsigned long val;

	if (sscanf(buf, "%lx", &val) != 1)
		return -EINVAL;

	drvdata->seq_12_event = val & ETM_EVENT_MASK;
	return size;
}
static DEVICE_ATTR(seq_12_event, S_IRUGO | S_IWUSR, etm_show_seq_12_event,
		   etm_store_seq_12_event);

static ssize_t etm_show_seq_21_event(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	unsigned long val = drvdata->seq_21_event;
	return scnprintf(buf, PAGE_SIZE, "%#lx\n", val);
}

static ssize_t etm_store_seq_21_event(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t size)
{
	unsigned long val;

	if (sscanf(buf, "%lx", &val) != 1)
		return -EINVAL;

	drvdata->seq_21_event = val & ETM_EVENT_MASK;
	return size;
}
static DEVICE_ATTR(seq_21_event, S_IRUGO | S_IWUSR, etm_show_seq_21_event,
		   etm_store_seq_21_event);

static ssize_t etm_show_seq_23_event(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	unsigned long val = drvdata->seq_23_event;
	return scnprintf(buf, PAGE_SIZE, "%#lx\n", val);
}

static ssize_t etm_store_seq_23_event(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t size)
{
	unsigned long val;

	if (sscanf(buf, "%lx", &val) != 1)
		return -EINVAL;

	drvdata->seq_23_event = val & ETM_EVENT_MASK;
	return size;
}
static DEVICE_ATTR(seq_23_event, S_IRUGO | S_IWUSR, etm_show_seq_23_event,
		   etm_store_seq_23_event);

static ssize_t etm_show_seq_31_event(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	unsigned long val = drvdata->seq_31_event;
	return scnprintf(buf, PAGE_SIZE, "%#lx\n", val);
}

static ssize_t etm_store_seq_31_event(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t size)
{
	unsigned long val;

	if (sscanf(buf, "%lx", &val) != 1)
		return -EINVAL;

	drvdata->seq_31_event = val & ETM_EVENT_MASK;
	return size;
}
static DEVICE_ATTR(seq_31_event, S_IRUGO | S_IWUSR, etm_show_seq_31_event,
		   etm_store_seq_31_event);

static ssize_t etm_show_seq_32_event(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	unsigned long val = drvdata->seq_32_event;
	return scnprintf(buf, PAGE_SIZE, "%#lx\n", val);
}

static ssize_t etm_store_seq_32_event(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t size)
{
	unsigned long val;

	if (sscanf(buf, "%lx", &val) != 1)
		return -EINVAL;

	drvdata->seq_32_event = val & ETM_EVENT_MASK;
	return size;
}
static DEVICE_ATTR(seq_32_event, S_IRUGO | S_IWUSR, etm_show_seq_32_event,
		   etm_store_seq_32_event);

static ssize_t etm_show_seq_13_event(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	unsigned long val = drvdata->seq_13_event;
	return scnprintf(buf, PAGE_SIZE, "%#lx\n", val);
}

static ssize_t etm_store_seq_13_event(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t size)
{
	unsigned long val;

	if (sscanf(buf, "%lx", &val) != 1)
		return -EINVAL;

	drvdata->seq_13_event = val & ETM_EVENT_MASK;
	return size;
}
static DEVICE_ATTR(seq_13_event, S_IRUGO | S_IWUSR, etm_show_seq_13_event,
		   etm_store_seq_13_event);

static ssize_t etm_show_seq_curr_state(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	unsigned long val = drvdata->seq_curr_state;
	return scnprintf(buf, PAGE_SIZE, "%#lx\n", val);
}

static ssize_t etm_store_seq_curr_state(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t size)
{
	unsigned long val;

	if (sscanf(buf, "%lx", &val) != 1)
		return -EINVAL;
	if (val > ETM_SEQ_STATE_MAX_VAL)
		return -EINVAL;

	drvdata->seq_curr_state = val;
	return size;
}
static DEVICE_ATTR(seq_curr_state, S_IRUGO | S_IWUSR, etm_show_seq_curr_state,
		   etm_store_seq_curr_state);

static ssize_t etm_show_ctxid_idx(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	unsigned long val = drvdata->ctxid_idx;
	return scnprintf(buf, PAGE_SIZE, "%#lx\n", val);
}

static ssize_t etm_store_ctxid_idx(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t size)
{
	unsigned long val;

	if (sscanf(buf, "%lx", &val) != 1)
		return -EINVAL;
	if (val >= drvdata->nr_ctxid_cmp)
		return -EINVAL;

	mutex_lock(&drvdata->mutex);
	drvdata->ctxid_idx = val;
	mutex_unlock(&drvdata->mutex);
	return size;
}
static DEVICE_ATTR(ctxid_idx, S_IRUGO | S_IWUSR, etm_show_ctxid_idx,
		   etm_store_ctxid_idx);

static ssize_t etm_show_ctxid_val(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	unsigned long val;

	mutex_lock(&drvdata->mutex);
	val = drvdata->ctxid_val[drvdata->ctxid_idx];
	mutex_unlock(&drvdata->mutex);
	return scnprintf(buf, PAGE_SIZE, "%#lx\n", val);
}

static ssize_t etm_store_ctxid_val(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t size)
{
	unsigned long val;

	if (sscanf(buf, "%lx", &val) != 1)
		return -EINVAL;

	mutex_lock(&drvdata->mutex);
	drvdata->ctxid_val[drvdata->ctxid_idx] = val;
	mutex_unlock(&drvdata->mutex);
	return size;
}
static DEVICE_ATTR(ctxid_val, S_IRUGO | S_IWUSR, etm_show_ctxid_val,
		   etm_store_ctxid_val);

static ssize_t etm_show_ctxid_mask(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	unsigned long val = drvdata->ctxid_mask;
	return scnprintf(buf, PAGE_SIZE, "%#lx\n", val);
}

static ssize_t etm_store_ctxid_mask(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t size)
{
	unsigned long val;

	if (sscanf(buf, "%lx", &val) != 1)
		return -EINVAL;

	drvdata->ctxid_mask = val;
	return size;
}
static DEVICE_ATTR(ctxid_mask, S_IRUGO | S_IWUSR, etm_show_ctxid_mask,
		   etm_store_ctxid_mask);

static ssize_t etm_show_sync_freq(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	unsigned long val = drvdata->sync_freq;
	return scnprintf(buf, PAGE_SIZE, "%#lx\n", val);
}

static ssize_t etm_store_sync_freq(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t size)
{
	unsigned long val;

	if (sscanf(buf, "%lx", &val) != 1)
		return -EINVAL;

	drvdata->sync_freq = val & ETM_SYNC_MASK;
	return size;
}
static DEVICE_ATTR(sync_freq, S_IRUGO | S_IWUSR, etm_show_sync_freq,
		   etm_store_sync_freq);

static ssize_t etm_show_timestamp_event(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	unsigned long val = drvdata->timestamp_event;
	return scnprintf(buf, PAGE_SIZE, "%#lx\n", val);
}

static ssize_t etm_store_timestamp_event(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t size)
{
	unsigned long val;

	if (sscanf(buf, "%lx", &val) != 1)
		return -EINVAL;

	drvdata->timestamp_event = val & ETM_EVENT_MASK;
	return size;
}
static DEVICE_ATTR(timestamp_event, S_IRUGO | S_IWUSR, etm_show_timestamp_event,
		   etm_store_timestamp_event);

static struct attribute *etm_attrs[] = {
	&dev_attr_nr_addr_cmp.attr,
	&dev_attr_nr_cntr.attr,
	&dev_attr_nr_ctxid_cmp.attr,
	&dev_attr_reset.attr,
	&dev_attr_mode.attr,
	&dev_attr_trigger_event.attr,
	&dev_attr_enable_event.attr,
	&dev_attr_fifofull_level.attr,
	&dev_attr_addr_idx.attr,
	&dev_attr_addr_single.attr,
	&dev_attr_addr_range.attr,
	&dev_attr_addr_start.attr,
	&dev_attr_addr_stop.attr,
	&dev_attr_addr_acctype.attr,
	&dev_attr_cntr_idx.attr,
	&dev_attr_cntr_rld_val.attr,
	&dev_attr_cntr_event.attr,
	&dev_attr_cntr_rld_event.attr,
	&dev_attr_cntr_val.attr,
	&dev_attr_seq_12_event.attr,
	&dev_attr_seq_21_event.attr,
	&dev_attr_seq_23_event.attr,
	&dev_attr_seq_31_event.attr,
	&dev_attr_seq_32_event.attr,
	&dev_attr_seq_13_event.attr,
	&dev_attr_seq_curr_state.attr,
	&dev_attr_ctxid_idx.attr,
	&dev_attr_ctxid_val.attr,
	&dev_attr_ctxid_mask.attr,
	&dev_attr_sync_freq.attr,
	&dev_attr_timestamp_event.attr,
	NULL,
};

static struct attribute_group etm_attr_grp = {
	.attrs = etm_attrs,
};

static int __devinit etm_sysfs_init(void)
{
	int ret;

	drvdata->kobj = kobject_create_and_add("etm", qdss_get_modulekobj());
	if (!drvdata->kobj) {
		dev_err(drvdata->dev, "failed to create ETM sysfs kobject\n");
		ret = -ENOMEM;
		goto err_create;
	}

	ret = sysfs_create_file(drvdata->kobj, &dev_attr_enabled.attr);
	if (ret) {
		dev_err(drvdata->dev, "failed to create ETM sysfs enabled"
		" attribute\n");
		goto err_file;
	}

	if (sysfs_create_group(drvdata->kobj, &etm_attr_grp))
		dev_err(drvdata->dev, "failed to create ETM sysfs group\n");

	return 0;
err_file:
	kobject_put(drvdata->kobj);
err_create:
	return ret;
}

static void __devexit etm_sysfs_exit(void)
{
	sysfs_remove_group(drvdata->kobj, &etm_attr_grp);
	sysfs_remove_file(drvdata->kobj, &dev_attr_enabled.attr);
	kobject_put(drvdata->kobj);
}

static bool __devinit etm_arch_supported(uint8_t arch)
{
	switch (arch) {
	case PFT_ARCH_V1_1:
		break;
	default:
		return false;
	}
	return true;
}

static int __devinit etm_init_arch_data(void)
{
	int ret;
	
	int cpu = 0;
	uint32_t etmidr;
	uint32_t etmccr;

	
	etm_os_unlock(NULL);
	smp_call_function(etm_os_unlock, NULL, 1);
	ETM_UNLOCK(cpu);
	
	etm_clr_pwrdwn(cpu);
	etm_set_prog(cpu);

	
	etmidr = etm_readl(drvdata, cpu, ETMIDR);
	drvdata->arch = BMVAL(etmidr, 4, 11);
	if (etm_arch_supported(drvdata->arch) == false) {
		ret = -EINVAL;
		goto err;
	}

	etmccr = etm_readl(drvdata, cpu, ETMCCR);
	drvdata->nr_addr_cmp = BMVAL(etmccr, 0, 3) * 2;
	drvdata->nr_cntr = BMVAL(etmccr, 13, 15);
	drvdata->nr_ext_inp = BMVAL(etmccr, 17, 19);
	drvdata->nr_ext_out = BMVAL(etmccr, 20, 22);
	drvdata->nr_ctxid_cmp = BMVAL(etmccr, 24, 25);

	
	etm_set_pwrdwn(cpu);
	ETM_LOCK(cpu);

	return 0;
err:
	return ret;
}

static void __devinit etm_init_default_data(void)
{
	int i;

	drvdata->trigger_event = 0x406F;
	drvdata->enable_event = 0x6F;
	drvdata->enable_ctrl1 = 0x1;
	drvdata->fifofull_level	= 0x28;
	if (drvdata->nr_addr_cmp >= 2) {
		drvdata->addr_val[0] = (uint32_t) _stext;
		drvdata->addr_val[1] = (uint32_t) _etext;
		drvdata->addr_type[0] = ETM_ADDR_TYPE_RANGE;
		drvdata->addr_type[1] = ETM_ADDR_TYPE_RANGE;
	}
	for (i = 0; i < drvdata->nr_cntr; i++) {
		drvdata->cntr_event[i] = 0x406F;
		drvdata->cntr_rld_event[i] = 0x406F;
	}
	drvdata->seq_12_event = 0x406F;
	drvdata->seq_21_event = 0x406F;
	drvdata->seq_23_event = 0x406F;
	drvdata->seq_31_event = 0x406F;
	drvdata->seq_32_event = 0x406F;
	drvdata->seq_13_event = 0x406F;
	drvdata->sync_freq = 0x80;
	drvdata->timestamp_event = 0x406F;

	
	if (cpu_is_krait_v1()) {
		drvdata->mode = (ETM_MODE_EXCLUDE | ETM_MODE_CYCACC);
		drvdata->ctrl = 0x1000;
		drvdata->enable_ctrl1 = 0x1000000;
		for (i = 0; i < drvdata->nr_addr_cmp; i++) {
			drvdata->addr_val[i] = 0x0;
			drvdata->addr_acctype[i] = 0x0;
			drvdata->addr_type[i] = ETM_ADDR_TYPE_NONE;
		}
	}
}

static int __devinit etm_probe(struct platform_device *pdev)
{
	int ret;
	struct resource *res;

	
	if (cpu_is_krait_v3()) {
		dev_info(&pdev->dev, "ETM: failing probe for Krait v3\n");
		return -EINVAL;
	}

	drvdata = kzalloc(sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata) {
		ret = -ENOMEM;
		goto err_kzalloc_drvdata;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		ret = -EINVAL;
		goto err_res;
	}

	drvdata->base = ioremap_nocache(res->start, resource_size(res));
	if (!drvdata->base) {
		ret = -EINVAL;
		goto err_ioremap;
	}

	drvdata->dev = &pdev->dev;

	mutex_init(&drvdata->mutex);
	wake_lock_init(&drvdata->wake_lock, WAKE_LOCK_SUSPEND, "msm_etm");
	pm_qos_add_request(&drvdata->qos_req, PM_QOS_CPU_DMA_LATENCY,
			   PM_QOS_DEFAULT_VALUE);
	drvdata->src = qdss_get("msm_etm");
	if (IS_ERR(drvdata->src)) {
		ret = PTR_ERR(drvdata->src);
		goto err_qdssget;
	}

	drvdata->clk = clk_get(drvdata->dev, "core_clk");
	if (IS_ERR(drvdata->clk)) {
		ret = PTR_ERR(drvdata->clk);
		goto err_clk_get;
	}

	ret = clk_set_rate(drvdata->clk, CORESIGHT_CLK_RATE_TRACE);
	if (ret)
		goto err_clk_rate;

	ret = clk_prepare_enable(drvdata->clk);
	if (ret)
		goto err_clk_enable;

	ret = etm_init_arch_data();
	if (ret)
		goto err_arch;

	etm_init_default_data();

	ret = etm_sysfs_init();
	if (ret)
		goto err_sysfs;

	drvdata->enabled = false;

	clk_disable_unprepare(drvdata->clk);

	dev_info(drvdata->dev, "ETM initialized\n");

	if (etm_boot_enable)
		etm_enable();

	return 0;

err_sysfs:
err_arch:
	clk_disable_unprepare(drvdata->clk);
err_clk_enable:
err_clk_rate:
	clk_put(drvdata->clk);
err_clk_get:
	qdss_put(drvdata->src);
err_qdssget:
	pm_qos_remove_request(&drvdata->qos_req);
	wake_lock_destroy(&drvdata->wake_lock);
	mutex_destroy(&drvdata->mutex);
	iounmap(drvdata->base);
err_ioremap:
err_res:
	kfree(drvdata);
err_kzalloc_drvdata:
	dev_err(drvdata->dev, "ETM init failed\n");
	return ret;
}

static int __devexit etm_remove(struct platform_device *pdev)
{
	if (drvdata->enabled)
		etm_disable();
	etm_sysfs_exit();
	clk_put(drvdata->clk);
	qdss_put(drvdata->src);
	pm_qos_remove_request(&drvdata->qos_req);
	wake_lock_destroy(&drvdata->wake_lock);
	mutex_destroy(&drvdata->mutex);
	iounmap(drvdata->base);
	kfree(drvdata);

	return 0;
}

static struct of_device_id etm_match[] = {
	{.compatible = "qcom,msm-etm"},
	{}
};

static struct platform_driver etm_driver = {
	.probe          = etm_probe,
	.remove         = __devexit_p(etm_remove),
	.driver         = {
		.name   = "msm_etm",
		.owner	= THIS_MODULE,
		.of_match_table = etm_match,
	},
};

int __init etm_init(void)
{
	return platform_driver_register(&etm_driver);
}
module_init(etm_init);

void __exit etm_exit(void)
{
	platform_driver_unregister(&etm_driver);
}
module_exit(etm_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("CoreSight Program Flow Trace driver");
