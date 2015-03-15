/*
 *  drivers/cpufreq/cpufreq_elementalx.c
 *
 *  Copyright (C)  2001 Russell King
 *            (C)  2003 Venkatesh Pallipadi <venkatesh.pallipadi@intel.com>
 *                      Jun Nakajima <jun.nakajima@intel.com>
 *            (C)  2015 Aaron Segaert <asegaert@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/slab.h>
#include <linux/fb.h>
#include <linux/msm_kgsl.h>
#include "cpufreq_governor.h"

/* elementalx governor macros */
#define DEF_FREQUENCY_UP_THRESHOLD		(90)
#define DEF_FREQUENCY_DOWN_DIFFERENTIAL		(20)
#define DEF_ACTIVE_FLOOR_FREQ			(960000)
#define DEF_GBOOST_MIN_FREQ			(1728000)
#define DEF_MAX_SCREEN_OFF_FREQ			(2265000)
#define MIN_SAMPLING_RATE			(10000)
#define DEF_SAMPLING_DOWN_FACTOR		(8)
#define MAX_SAMPLING_DOWN_FACTOR		(20)
#define FREQ_NEED_BURST(x)			(x < 800000 ? 1 : 0)
#define MAX(x,y)				(x > y ? x : y)
#define MIN(x,y)				(x < y ? x : y)

static DEFINE_PER_CPU(struct ex_cpu_dbs_info_s, ex_cpu_dbs_info);

static unsigned int up_threshold_level[2] __read_mostly = {95, 85};

static struct ex_governor_data {
	unsigned int active_floor_freq;
	unsigned int max_screen_off_freq;
	unsigned int prev_load;
	unsigned int g_count;
	bool suspended;
	struct notifier_block notif;
} ex_data = {
	.active_floor_freq = DEF_ACTIVE_FLOOR_FREQ,
	.max_screen_off_freq = DEF_MAX_SCREEN_OFF_FREQ,
	.prev_load = 0,
	.g_count = 0,
	.suspended = false
};

static inline unsigned int ex_freq_increase(struct cpufreq_policy *p, unsigned int freq, int cpu)
{
	if (freq > p->max) {
		return p->max;
	} 
	
	else if (ex_data.g_count > 30) {
		freq = MAX(freq, ex_data.active_floor_freq);
	} 

	else if (ex_data.suspended) {
		freq = MIN(freq, ex_data.max_screen_off_freq);
	}

	return freq;
}

static void ex_check_cpu(int cpu, unsigned int load)
{
	struct ex_cpu_dbs_info_s *dbs_info = &per_cpu(ex_cpu_dbs_info, cpu);
	struct cpufreq_policy *policy = dbs_info->cdbs.cur_policy;
	struct dbs_data *dbs_data = policy->governor_data;
	struct ex_dbs_tuners *ex_tuners = dbs_data->tuners;
	unsigned int max_load_freq = 0, freq_next = 0;
	unsigned int j, avg_load, cur_freq, target_freq = 0;

	cpufreq_notify_utilization(policy, load);

	cur_freq = policy->cur;

	for_each_cpu(j, policy->cpus) {
		if (load > max_load_freq)
			max_load_freq = load * policy->cur;
	}
	avg_load = (ex_data.prev_load + load) >> 1;

	if (ex_tuners->gboost) {
		if (ex_data.g_count < 200 && graphics_boost < 4)
			++ex_data.g_count;
		else if (ex_data.g_count > 1)
			--ex_data.g_count;
	}

	//gboost mode
	if (ex_tuners->gboost && ex_data.g_count > 90) {
				
		if (avg_load > 40 + (graphics_boost * 10)) {
			freq_next = policy->max;
		} else {
			freq_next = policy->max * avg_load / 100;
			freq_next = MAX(freq_next, ex_tuners->gboost_min_freq);
		}

		target_freq = ex_freq_increase(policy, freq_next, cpu);

		__cpufreq_driver_target(policy, target_freq, CPUFREQ_RELATION_H);

		goto finished;
	} 

	//normal mode
	if (max_load_freq > up_threshold_level[1] * cur_freq) {

		dbs_info->down_floor = 0;

		if (FREQ_NEED_BURST(cur_freq) &&
				load > up_threshold_level[0]) {
			freq_next = policy->max;
		}
		
		else if (avg_load > up_threshold_level[0]) {
			freq_next = cur_freq + 1200000;
		}
		
		else if (avg_load <= up_threshold_level[1]) {
			freq_next = cur_freq;
		}
	
		else {		
			if (load > up_threshold_level[0]) {
				freq_next = cur_freq + 600000;
			}
		
			else {
				freq_next = cur_freq + 300000;
			}
		}

		target_freq = ex_freq_increase(policy, freq_next, cpu);

		__cpufreq_driver_target(policy, target_freq, CPUFREQ_RELATION_H);

		goto finished;
	}

	if (++dbs_info->down_floor > ex_tuners->sampling_down_factor)
		dbs_info->down_floor = 0;

	if (cur_freq == policy->min){
		goto finished;
	}

	if (max_load_freq <
	    (ex_tuners->up_threshold - ex_tuners->down_differential) *
	     cur_freq) {

		freq_next = max_load_freq /
				(ex_tuners->up_threshold -
				 ex_tuners->down_differential);

		if (dbs_info->down_floor)
			freq_next = MAX(freq_next, ex_data.active_floor_freq);
		else
			freq_next = MAX(freq_next, policy->min);

		__cpufreq_driver_target(policy, freq_next,
			CPUFREQ_RELATION_L);
	}

finished:
	ex_data.prev_load = load;
	return;
}

static void ex_dbs_timer(struct work_struct *work)
{
	struct ex_cpu_dbs_info_s *dbs_info = container_of(work,
			struct ex_cpu_dbs_info_s, cdbs.work.work);
	unsigned int cpu = dbs_info->cdbs.cur_policy->cpu;
	struct ex_cpu_dbs_info_s *core_dbs_info = &per_cpu(ex_cpu_dbs_info,
			cpu);
	struct dbs_data *dbs_data = dbs_info->cdbs.cur_policy->governor_data;
	struct ex_dbs_tuners *ex_tuners = dbs_data->tuners;
	int delay = delay_for_sampling_rate(ex_tuners->sampling_rate);
	bool modify_all = true;

	mutex_lock(&core_dbs_info->cdbs.timer_mutex);
	if (!need_load_eval(&core_dbs_info->cdbs, ex_tuners->sampling_rate))
		modify_all = false;
	else
		dbs_check_cpu(dbs_data, cpu);

	gov_queue_work(dbs_data, dbs_info->cdbs.cur_policy, delay, modify_all);
	mutex_unlock(&core_dbs_info->cdbs.timer_mutex);
}

static int fb_notifier_callback(struct notifier_block *this,
				unsigned long event, void *data)
{
	struct fb_event *evdata = data;
	int *blank;

	if (evdata && evdata->data && event == FB_EVENT_BLANK) {
		blank = evdata->data;
		switch (*blank) {
			case FB_BLANK_UNBLANK:
				//display on
				ex_data.suspended = false;
				break;
			case FB_BLANK_POWERDOWN:
			case FB_BLANK_HSYNC_SUSPEND:
			case FB_BLANK_VSYNC_SUSPEND:
			case FB_BLANK_NORMAL:
				//display off
				ex_data.suspended = true;
				break;
		}
	}

	return NOTIFY_OK;
}

/************************** sysfs interface ************************/
static struct common_dbs_data ex_dbs_cdata;

static ssize_t store_sampling_rate(struct dbs_data *dbs_data, const char *buf,
		size_t count)
{
	struct ex_dbs_tuners *ex_tuners = dbs_data->tuners;
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1)
		return -EINVAL;

	ex_tuners->sampling_rate = max(input, dbs_data->min_sampling_rate);
	return count;
}

static ssize_t store_up_threshold(struct dbs_data *dbs_data, const char *buf,
		size_t count)
{
	struct ex_dbs_tuners *ex_tuners = dbs_data->tuners;
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1 || input > 100 || input <= ex_tuners->down_differential)
		return -EINVAL;

	ex_tuners->up_threshold = input;
	return count;
}

static ssize_t store_down_differential(struct dbs_data *dbs_data,
		const char *buf, size_t count)
{
	struct ex_dbs_tuners *ex_tuners = dbs_data->tuners;
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1 || input > 100 || input >= ex_tuners->up_threshold)
		return -EINVAL;

	ex_tuners->down_differential = input;
	return count;
}

static ssize_t store_gboost(struct dbs_data *dbs_data, const char *buf,
		size_t count)
{
	struct ex_dbs_tuners *ex_tuners = dbs_data->tuners;
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1 || input > 1)
		return -EINVAL;

	if (input == 0)
		ex_data.g_count = 0;

	ex_tuners->gboost = input;
	return count;
}

static ssize_t store_gboost_min_freq(struct dbs_data *dbs_data,
		const char *buf, size_t count)
{
	struct ex_dbs_tuners *ex_tuners = dbs_data->tuners;
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1)
		return -EINVAL;

	ex_tuners->gboost_min_freq = input;
	return count;
}

static ssize_t store_active_floor_freq(struct dbs_data *dbs_data,
		const char *buf, size_t count)
{
	struct ex_dbs_tuners *ex_tuners = dbs_data->tuners;
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1)
		return -EINVAL;

	ex_tuners->active_floor_freq = input;
	ex_data.active_floor_freq = ex_tuners->active_floor_freq;
	return count;
}

static ssize_t store_max_screen_off_freq(struct dbs_data *dbs_data,
		const char *buf, size_t count)
{
	struct ex_dbs_tuners *ex_tuners = dbs_data->tuners;
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1)
		return -EINVAL;

	if (input == 0)
		input = UINT_MAX;

	ex_tuners->max_screen_off_freq = input;
	ex_data.max_screen_off_freq = ex_tuners->max_screen_off_freq;
	return count;
}

static ssize_t store_sampling_down_factor(struct dbs_data *dbs_data,
		const char *buf, size_t count)
{
	struct ex_dbs_tuners *ex_tuners = dbs_data->tuners;
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1 || input > MAX_SAMPLING_DOWN_FACTOR || input < 0)
		return -EINVAL;

	ex_tuners->sampling_down_factor = input;
	return count;
}

show_store_one(ex, sampling_rate);
show_store_one(ex, up_threshold);
show_store_one(ex, down_differential);
show_store_one(ex, gboost);
show_store_one(ex, gboost_min_freq);
show_store_one(ex, active_floor_freq);
show_store_one(ex, max_screen_off_freq);
show_store_one(ex, sampling_down_factor);
declare_show_sampling_rate_min(ex);

gov_sys_pol_attr_rw(sampling_rate);
gov_sys_pol_attr_rw(up_threshold);
gov_sys_pol_attr_rw(down_differential);
gov_sys_pol_attr_rw(gboost);
gov_sys_pol_attr_rw(gboost_min_freq);
gov_sys_pol_attr_rw(active_floor_freq);
gov_sys_pol_attr_rw(max_screen_off_freq);
gov_sys_pol_attr_rw(sampling_down_factor);
gov_sys_pol_attr_ro(sampling_rate_min);

static struct attribute *dbs_attributes_gov_sys[] = {
	&sampling_rate_min_gov_sys.attr,
	&sampling_rate_gov_sys.attr,
	&up_threshold_gov_sys.attr,
	&down_differential_gov_sys.attr,
	&gboost_gov_sys.attr,
	&gboost_min_freq_gov_sys.attr,
	&active_floor_freq_gov_sys.attr,
	&max_screen_off_freq_gov_sys.attr,
	&sampling_down_factor_gov_sys.attr,
	NULL
};

static struct attribute_group ex_attr_group_gov_sys = {
	.attrs = dbs_attributes_gov_sys,
	.name = "elementalx",
};

static struct attribute *dbs_attributes_gov_pol[] = {
	&sampling_rate_min_gov_pol.attr,
	&sampling_rate_gov_pol.attr,
	&up_threshold_gov_pol.attr,
	&down_differential_gov_pol.attr,
	&gboost_gov_pol.attr,
	&gboost_min_freq_gov_pol.attr,
	&active_floor_freq_gov_pol.attr,
	&max_screen_off_freq_gov_pol.attr,
	&sampling_down_factor_gov_pol.attr,
	NULL
};

static struct attribute_group ex_attr_group_gov_pol = {
	.attrs = dbs_attributes_gov_pol,
	.name = "elementalx",
};

/************************** sysfs end ************************/

static int ex_init(struct dbs_data *dbs_data)
{
	struct ex_dbs_tuners *tuners;

	tuners = kzalloc(sizeof(*tuners), GFP_KERNEL);
	if (!tuners) {
		pr_err("%s: kzalloc failed\n", __func__);
		return -ENOMEM;
	}

	tuners->up_threshold = DEF_FREQUENCY_UP_THRESHOLD;
	tuners->down_differential = DEF_FREQUENCY_DOWN_DIFFERENTIAL;
	tuners->ignore_nice_load = 0;
	tuners->gboost = 1;
	tuners->gboost_min_freq = DEF_GBOOST_MIN_FREQ;
	tuners->active_floor_freq = DEF_ACTIVE_FLOOR_FREQ;
	tuners->max_screen_off_freq = DEF_MAX_SCREEN_OFF_FREQ;
	tuners->sampling_down_factor = DEF_SAMPLING_DOWN_FACTOR;

	dbs_data->tuners = tuners;
	dbs_data->min_sampling_rate = MIN_SAMPLING_RATE;

	ex_data.notif.notifier_call = fb_notifier_callback;
	if (fb_register_client(&ex_data.notif))
		pr_err("%s: Failed to register fb_notifier\n", __func__);

	mutex_init(&dbs_data->mutex);
	return 0;
}

static void ex_exit(struct dbs_data *dbs_data)
{
	fb_unregister_client(&ex_data.notif);
	kfree(dbs_data->tuners);
}

define_get_cpu_dbs_routines(ex_cpu_dbs_info);

static struct common_dbs_data ex_dbs_cdata = {
	.governor = GOV_ELEMENTALX,
	.attr_group_gov_sys = &ex_attr_group_gov_sys,
	.attr_group_gov_pol = &ex_attr_group_gov_pol,
	.get_cpu_cdbs = get_cpu_cdbs,
	.get_cpu_dbs_info_s = get_cpu_dbs_info_s,
	.gov_dbs_timer = ex_dbs_timer,
	.gov_check_cpu = ex_check_cpu,
	.init = ex_init,
	.exit = ex_exit,
};

static int ex_cpufreq_governor_dbs(struct cpufreq_policy *policy,
				   unsigned int event)
{
	return cpufreq_governor_dbs(policy, &ex_dbs_cdata, event);
}

#ifndef CONFIG_CPU_FREQ_DEFAULT_GOV_ELEMENTALX
static
#endif
struct cpufreq_governor cpufreq_gov_elementalx = {
	.name			= "elementalx",
	.governor		= ex_cpufreq_governor_dbs,
	.max_transition_latency	= TRANSITION_LATENCY_LIMIT,
	.owner			= THIS_MODULE,
};

static int __init cpufreq_gov_dbs_init(void)
{
	return cpufreq_register_governor(&cpufreq_gov_elementalx);
}

static void __exit cpufreq_gov_dbs_exit(void)
{
	cpufreq_unregister_governor(&cpufreq_gov_elementalx);
}

MODULE_AUTHOR("Aaron Segaert <asegaert@gmail.com>");
MODULE_DESCRIPTION("'cpufreq_elementalx' - multiphase cpufreq governor");
MODULE_LICENSE("GPL");

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_ELEMENTALX
fs_initcall(cpufreq_gov_dbs_init);
#else
module_init(cpufreq_gov_dbs_init);
#endif
module_exit(cpufreq_gov_dbs_exit);
