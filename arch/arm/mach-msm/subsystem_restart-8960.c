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

#define pr_fmt(fmt) "subsys-restart: %s(): " fmt, __func__

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/delay.h>
#include <linux/list.h>
#include <linux/io.h>
#include <linux/kthread.h>
#include <linux/time.h>
#include <linux/wakelock.h>
#include <linux/suspend.h>

#include <asm/current.h>

#include <mach/peripheral-loader.h>
#include <mach/scm.h>
#include <mach/socinfo.h>
#include <mach/subsystem_notif.h>
#include <mach/subsystem_restart.h>
#include <mach/board_htc.h>
#include <mach/msm_smsm.h>

#include "smd_private.h"
#include <mach/htc_restart_handler.h>


#if defined(CONFIG_ARCH_APQ8064)
  #define EXTERNAL_MODEM "external_modem"
  #define SZ_DIAG_ERR_MSG 	0xC8

  extern char *get_mdm_errmsg(void);
#endif


#if defined(pr_debug)
#undef pr_debug
#endif
#define pr_debug(x...) do {				\
			printk(KERN_DEBUG "[SSR] "x);		\
	} while (0)

#if defined(pr_warning)
#undef pr_warning
#endif
#define pr_warning(x...) do {				\
			printk(KERN_WARNING "[SSR] "x);		\
	} while (0)

#if defined(pr_info)
#undef pr_info
#endif
#define pr_info(x...) do {				\
			printk(KERN_INFO "[SSR] "x);		\
	} while (0)

#if defined(pr_err)
#undef pr_err
#endif
#define pr_err(x...) do {				\
			printk(KERN_ERR "[SSR] "x);		\
	} while (0)

struct subsys_soc_restart_order {
	const char * const *subsystem_list;
	int count;

	struct mutex shutdown_lock;
	struct mutex powerup_lock;
	struct subsys_data *subsys_ptrs[];
};

struct restart_wq_data {
	struct subsys_data *subsys;
	struct wake_lock ssr_wake_lock;
	char wlname[64];
	int use_restart_order;
	struct work_struct work;
};

struct restart_log {
	struct timeval time;
	struct subsys_data *subsys;
	struct list_head list;
};

static int restart_level;
static int enable_ramdumps;
struct workqueue_struct *ssr_wq;

static wait_queue_head_t subsystem_restart_wq;
static char subsystem_restart_reason[256];

static enum {
	SUBSYSTEM_RESTART_STATE_NONE,
		SUBSYSTEM_RESTART_STATE_ACTION,
} subsystem_restart_state;

static LIST_HEAD(restart_log_list);
static LIST_HEAD(subsystem_list);
static DEFINE_SPINLOCK(subsystem_list_lock);
static DEFINE_MUTEX(soc_order_reg_lock);
static DEFINE_MUTEX(restart_log_mutex);


#define DEFINE_SINGLE_RESTART_ORDER(name, order)		\
	static struct subsys_soc_restart_order __##name = {	\
		.subsystem_list = order,			\
		.count = ARRAY_SIZE(order),			\
		.subsys_ptrs = {[ARRAY_SIZE(order)] = NULL}	\
	};							\
	static struct subsys_soc_restart_order *name[] = {      \
		&__##name,					\
	}

static const char * const _order_8x60_all[] = {
	"external_modem",  "modem", "lpass"
};
DEFINE_SINGLE_RESTART_ORDER(orders_8x60_all, _order_8x60_all);

static const char * const _order_8x60_modems[] = {"external_modem", "modem"};
DEFINE_SINGLE_RESTART_ORDER(orders_8x60_modems, _order_8x60_modems);

static const char * const order_8960[] = {"modem", "lpass"};
static const char * const order_8960_sglte[] = {"external_modem",
						"modem"};

static struct subsys_soc_restart_order restart_orders_8960_one = {
	.subsystem_list = order_8960,
	.count = ARRAY_SIZE(order_8960),
	.subsys_ptrs = {[ARRAY_SIZE(order_8960)] = NULL}
	};

static struct subsys_soc_restart_order restart_orders_8960_fusion_sglte = {
	.subsystem_list = order_8960_sglte,
	.count = ARRAY_SIZE(order_8960_sglte),
	.subsys_ptrs = {[ARRAY_SIZE(order_8960_sglte)] = NULL}
	};

static struct subsys_soc_restart_order *restart_orders_8960[] = {
	&restart_orders_8960_one,
	};

static struct subsys_soc_restart_order *restart_orders_8960_sglte[] = {
	&restart_orders_8960_fusion_sglte,
	};

static struct subsys_soc_restart_order **restart_orders;
static int n_restart_orders;

module_param(enable_ramdumps, int, S_IRUGO | S_IWUSR);

static struct subsys_soc_restart_order *_update_restart_order(
		struct subsys_data *subsys);

int get_restart_level()
{
	return restart_level;
}
EXPORT_SYMBOL(get_restart_level);

int ssr_have_set_restart_reason;

void ssr_set_restart_reason(const char *reason)
{
	set_restart_to_ramdump(reason);
	ssr_have_set_restart_reason = 1;
}
EXPORT_SYMBOL(ssr_set_restart_reason);

static int restart_level_set(const char *val, struct kernel_param *kp)
{
	int ret;
	int old_val = restart_level;

	if (cpu_is_msm9615()) {
		pr_err("Only Phase 1 subsystem restart is supported\n");
		return -EINVAL;
	}

	ret = param_set_int(val, kp);
	if (ret)
		return ret;

	switch (restart_level) {

	case RESET_SOC:
	case RESET_SUBSYS_COUPLED:
	case RESET_SUBSYS_INDEPENDENT:
		pr_info("Phase %d behavior activated.\n", restart_level);
	break;

	default:
		restart_level = old_val;
		return -EINVAL;
	break;

	}
	return 0;
}

module_param_call(restart_level, restart_level_set, param_get_int,
			&restart_level, 0644);

static struct subsys_data *_find_subsystem(const char *subsys_name)
{
	struct subsys_data *subsys;
	unsigned long flags;

	spin_lock_irqsave(&subsystem_list_lock, flags);
	list_for_each_entry(subsys, &subsystem_list, list)
		if (!strncmp(subsys->name, subsys_name,
				SUBSYS_NAME_MAX_LENGTH)) {
			spin_unlock_irqrestore(&subsystem_list_lock, flags);
			return subsys;
		}
	spin_unlock_irqrestore(&subsystem_list_lock, flags);

	return NULL;
}

static struct subsys_soc_restart_order *_update_restart_order(
		struct subsys_data *subsys)
{
	int i, j;

	if (!subsys)
		return NULL;

	if (!subsys->name)
		return NULL;

	mutex_lock(&soc_order_reg_lock);
	for (j = 0; j < n_restart_orders; j++) {
		for (i = 0; i < restart_orders[j]->count; i++)
			if (!strncmp(restart_orders[j]->subsystem_list[i],
				subsys->name, SUBSYS_NAME_MAX_LENGTH)) {

					restart_orders[j]->subsys_ptrs[i] =
						subsys;
					mutex_unlock(&soc_order_reg_lock);
					return restart_orders[j];
			}
	}

	mutex_unlock(&soc_order_reg_lock);

	return NULL;
}

static void _send_notification_to_order(struct subsys_data
			**restart_list, int count,
			enum subsys_notif_type notif_type)
{
	int i;

	for (i = 0; i < count; i++)
		if (restart_list[i])
			subsys_notif_queue_notification(
				restart_list[i]->notif_handle, notif_type);
}

static int max_restarts;
module_param(max_restarts, int, 0644);

static long max_history_time = 3600;
module_param(max_history_time, long, 0644);

static void do_epoch_check(struct subsys_data *subsys)
{
	int n = 0;
	struct timeval *time_first = NULL, *curr_time;
	struct restart_log *r_log, *temp;
	static int max_restarts_check;
	static long max_history_time_check;

	mutex_lock(&restart_log_mutex);

	max_restarts_check = max_restarts;
	max_history_time_check = max_history_time;

	
	if (!max_restarts_check)
		goto out;

	r_log = kmalloc(sizeof(struct restart_log), GFP_KERNEL);
	if (!r_log)
		goto out;
	r_log->subsys = subsys;
	do_gettimeofday(&r_log->time);
	curr_time = &r_log->time;
	INIT_LIST_HEAD(&r_log->list);

	list_add_tail(&r_log->list, &restart_log_list);

	list_for_each_entry_safe(r_log, temp, &restart_log_list, list) {

		if ((curr_time->tv_sec - r_log->time.tv_sec) >
				max_history_time_check) {

			pr_debug("Deleted node with restart_time = %ld\n",
					r_log->time.tv_sec);
			list_del(&r_log->list);
			kfree(r_log);
			continue;
		}
		if (!n) {
			time_first = &r_log->time;
			pr_debug("Time_first: %ld\n", time_first->tv_sec);
		}
		n++;
		pr_debug("Restart_time: %ld\n", r_log->time.tv_sec);
	}

	if (time_first && n >= max_restarts_check) {
		if ((curr_time->tv_sec - time_first->tv_sec) <
				max_history_time_check)
			panic("Subsystems have crashed %d times in less than "
				"%ld seconds!", max_restarts_check,
				max_history_time_check);
	}

out:
	mutex_unlock(&restart_log_mutex);
}

static void subsystem_restart_wq_func(struct work_struct *work)
{
	struct restart_wq_data *r_work = container_of(work,
						struct restart_wq_data, work);
	struct subsys_data **restart_list;
	struct subsys_data *subsys = r_work->subsys;
	struct subsys_soc_restart_order *soc_restart_order = NULL;

	struct mutex *powerup_lock;
	struct mutex *shutdown_lock;

	int i;
	int restart_list_count = 0;

	if (r_work->use_restart_order)
		soc_restart_order = subsys->restart_order;

	if (!soc_restart_order) {
		restart_list = subsys->single_restart_list;
		restart_list_count = 1;
		powerup_lock = &subsys->powerup_lock;
		shutdown_lock = &subsys->shutdown_lock;
	} else {
		restart_list = soc_restart_order->subsys_ptrs;
		restart_list_count = soc_restart_order->count;
		powerup_lock = &soc_restart_order->powerup_lock;
		shutdown_lock = &soc_restart_order->shutdown_lock;
	}

	pr_debug("[%p]: Attempting to get shutdown lock!\n", current);

	if (!mutex_trylock(shutdown_lock))
		goto out;

	pr_debug("[%p]: Attempting to get powerup lock!\n", current);

	if (!mutex_trylock(powerup_lock))
		panic("%s[%p]: Subsystem died during powerup!",
						__func__, current);

	do_epoch_check(subsys);

	mutex_lock(&soc_order_reg_lock);

	pr_debug("[%p]: Starting restart sequence for %s\n", current,
			r_work->subsys->name);

	_send_notification_to_order(restart_list,
				restart_list_count,
				SUBSYS_BEFORE_SHUTDOWN);

	for (i = 0; i < restart_list_count; i++) {

		if (!restart_list[i])
			continue;

		pr_info("[%p]: Shutting down %s\n", current,
			restart_list[i]->name);

		if (restart_list[i]->shutdown(subsys) < 0)
			panic("subsys-restart: %s[%p]: Failed to shutdown %s!",
				__func__, current, restart_list[i]->name);
	}

	_send_notification_to_order(restart_list, restart_list_count,
				SUBSYS_AFTER_SHUTDOWN);

	mutex_unlock(shutdown_lock);

	
	for (i = 0; i < restart_list_count; i++) {
		if (!restart_list[i])
			continue;

		if (restart_list[i]->ramdump)
			if (restart_list[i]->ramdump(enable_ramdumps,
							subsys) < 0)
				pr_warn("%s[%p]: Ramdump failed.\n",
						restart_list[i]->name, current);
	}

	_send_notification_to_order(restart_list,
			restart_list_count,
			SUBSYS_BEFORE_POWERUP);

	for (i = restart_list_count - 1; i >= 0; i--) {

		if (!restart_list[i])
			continue;

		pr_info("[%p]: Powering up %s\n", current,
					restart_list[i]->name);

		if (restart_list[i]->powerup(subsys) < 0)
			panic("%s[%p]: Failed to powerup %s!", __func__,
				current, restart_list[i]->name);
	}

	_send_notification_to_order(restart_list,
				restart_list_count,
				SUBSYS_AFTER_POWERUP);

	pr_info("[%p]: Restart sequence for %s completed.\n",
			current, r_work->subsys->name);

	mutex_unlock(powerup_lock);

	mutex_unlock(&soc_order_reg_lock);

	pr_debug("[%p]: Released powerup lock!\n", current);

out:
	wake_unlock(&r_work->ssr_wake_lock);
	wake_lock_destroy(&r_work->ssr_wake_lock);
	kfree(r_work);
}

static void __subsystem_restart(struct subsys_data *subsys)
{
	struct restart_wq_data *data = NULL;
	int rc;

	pr_info("Restarting %s [level=%d]!\n", subsys->name,
				restart_level);

	if (!strncmp(subsys->name, "modem",
				SUBSYS_NAME_MAX_LENGTH)) {
		smd_diag_ssr(subsystem_restart_reason);
	}
	else {
		sprintf(subsystem_restart_reason, "%s fatal", subsys->name);
		pr_info("%s: %s\n", __func__, subsystem_restart_reason);
	}

	subsystem_restart_state = SUBSYSTEM_RESTART_STATE_ACTION;
	wake_up(&subsystem_restart_wq);

	data = kzalloc(sizeof(struct restart_wq_data), GFP_ATOMIC);
	if (!data)
		panic("%s: Unable to allocate memory to restart %s.",
		      __func__, subsys->name);

	data->subsys = subsys;

	if (restart_level != RESET_SUBSYS_INDEPENDENT)
		data->use_restart_order = 1;

	snprintf(data->wlname, sizeof(data->wlname), "ssr(%s)", subsys->name);
	wake_lock_init(&data->ssr_wake_lock, WAKE_LOCK_SUSPEND, data->wlname);
	wake_lock(&data->ssr_wake_lock);

	INIT_WORK(&data->work, subsystem_restart_wq_func);
	rc = queue_work(ssr_wq, &data->work);
	if (rc < 0)
		panic("%s: Unable to schedule work to restart %s (%d).",
		     __func__, subsys->name, rc);
}

int subsystem_restart(const char *subsys_name)
{
	struct subsys_data *subsys;
	char restart_reason[256];

	if (!subsys_name) {
		pr_err("Invalid subsystem name.\n");
		return -EINVAL;
	}

	pr_info("Restart sequence requested for %s, restart_level = %d.\n",
		subsys_name, restart_level);

	subsys = _find_subsystem(subsys_name);

	if (!subsys) {
		pr_warn("Unregistered subsystem %s!\n", subsys_name);
		return -EINVAL;
	}

	if (restart_level != RESET_SOC) {
		
#if defined(CONFIG_MSM_SSR_INDEPENDENT)
		if ((!subsys->enable_ssr) ||
				((get_radio_flag() & 0x8) && (!(get_kernel_flag() & KERNEL_FLAG_ENABLE_SSR_MODEM)) && (!strncmp(subsys_name, "modem", SUBSYS_NAME_MAX_LENGTH)))) {
#else
		if (!subsys->enable_ssr) {
#endif
			restart_level = RESET_SOC;
			pr_warn("%s: %s did not enable SSR, reset SOC instead.\n",
					__func__, subsys_name);
		}
	}


	switch (restart_level) {

	case RESET_SUBSYS_COUPLED:
	case RESET_SUBSYS_INDEPENDENT:
		__subsystem_restart(subsys);
		break;

	case RESET_SOC:
		if (!strncmp(subsys_name, "modem",
					SUBSYS_NAME_MAX_LENGTH)) {
			smd_diag();
		}

		if (!ssr_have_set_restart_reason) {
			sprintf(restart_reason, "%s fatal", subsys_name);
			ssr_set_restart_reason(restart_reason);
		}

		panic("subsys-restart: Resetting the SoC - %s crashed.",
				subsys->name);
		break;

	default:
		panic("subsys-restart: Unknown restart level!\n");
	break;

	}

	return 0;
}
EXPORT_SYMBOL(subsystem_restart);

int ssr_register_subsystem(struct subsys_data *subsys)
{
	unsigned long flags;

	if (!subsys)
		goto err;

	if (!subsys->name)
		goto err;

	if (!subsys->powerup || !subsys->shutdown)
		goto err;

	subsys->notif_handle = subsys_notif_add_subsys(subsys->name);
	subsys->restart_order = _update_restart_order(subsys);
	subsys->single_restart_list[0] = subsys;

	mutex_init(&subsys->shutdown_lock);
	mutex_init(&subsys->powerup_lock);

	spin_lock_irqsave(&subsystem_list_lock, flags);
	list_add(&subsys->list, &subsystem_list);
	spin_unlock_irqrestore(&subsystem_list_lock, flags);

	return 0;

err:
	return -EINVAL;
}
EXPORT_SYMBOL(ssr_register_subsystem);

static int ssr_panic_handler(struct notifier_block *this,
				unsigned long event, void *ptr)
{
	struct subsys_data *subsys;

	list_for_each_entry(subsys, &subsystem_list, list)
		if (subsys->crash_shutdown)
			subsys->crash_shutdown(subsys);
	return NOTIFY_DONE;
}

static struct notifier_block panic_nb = {
	.notifier_call  = ssr_panic_handler,
};

static int __init ssr_init_soc_restart_orders(void)
{
	int i;

	atomic_notifier_chain_register(&panic_notifier_list,
			&panic_nb);

	if (cpu_is_msm8x60()) {
		for (i = 0; i < ARRAY_SIZE(orders_8x60_all); i++) {
			mutex_init(&orders_8x60_all[i]->powerup_lock);
			mutex_init(&orders_8x60_all[i]->shutdown_lock);
		}

		for (i = 0; i < ARRAY_SIZE(orders_8x60_modems); i++) {
			mutex_init(&orders_8x60_modems[i]->powerup_lock);
			mutex_init(&orders_8x60_modems[i]->shutdown_lock);
		}

		restart_orders = orders_8x60_all;
		n_restart_orders = ARRAY_SIZE(orders_8x60_all);
	}

	if (cpu_is_msm8960() || cpu_is_msm8930() || cpu_is_msm8930aa() ||
	    cpu_is_msm9615() || cpu_is_apq8064() || cpu_is_msm8627()) {
		if (socinfo_get_platform_subtype() == PLATFORM_SUBTYPE_SGLTE) {
			restart_orders = restart_orders_8960_sglte;
			n_restart_orders =
				ARRAY_SIZE(restart_orders_8960_sglte);
		} else {
			restart_orders = restart_orders_8960;
			n_restart_orders = ARRAY_SIZE(restart_orders_8960);
		}
		for (i = 0; i < n_restart_orders; i++) {
			mutex_init(&restart_orders[i]->powerup_lock);
			mutex_init(&restart_orders[i]->shutdown_lock);
		}
	}

	if (restart_orders == NULL || n_restart_orders < 1) {
		WARN_ON(1);
		return -EINVAL;
	}

	return 0;
}

static ssize_t subsystem_restart_reason_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	char *s = buf;
	int ret;

	ret = wait_event_interruptible(subsystem_restart_wq,
			subsystem_restart_state != SUBSYSTEM_RESTART_STATE_NONE);
	printk(KERN_INFO "%s: state = %d", __func__, subsystem_restart_state);
	if (ret && subsystem_restart_state == SUBSYSTEM_RESTART_STATE_NONE)
		return ret;
	else
	{
		subsystem_restart_state = SUBSYSTEM_RESTART_STATE_NONE;
		s += sprintf(buf, subsystem_restart_reason);
	}

	printk(KERN_INFO "%s: state = %d, buf = %s", __func__, subsystem_restart_state, buf);
	return s - buf;
}
static ssize_t subsystem_restart_modem_trigger_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	char *s = buf;
	int ret = 0;

	if (get_kernel_flag() & KERNEL_FLAG_ENABLE_SSR_MODEM)
	{
		pr_info("%s: trigger modem restart\n", __func__);

		ret = smsm_change_state_ssr(SMSM_APPS_STATE, 0, SMSM_RESET, KERNEL_FLAG_ENABLE_SSR_MODEM);

		if(ret == 0)
			pr_info("%s: set smsm state SMS_RESET success\n", __func__);
		else
		{
			pr_info("%s: set smsm state SMS_RESET faild => ret = %d\n", __func__, ret);
			s += sprintf(buf, "Failed");
			return s - buf;
		}
		s += sprintf(buf, "Success");
	}
	else
	{
		pr_info("%s: kernel_flag = 0x%X\n", __func__, (int)get_kernel_flag());
		s += sprintf(buf, "Please check whether kernel flag 6 is 0x800");
	}

	return s - buf;
}

static ssize_t subsystem_restart_wcnss_trigger_show(struct kobject *kobj,
				      struct kobj_attribute *attr, char *buf)
{
	char *s = buf;
	int ret = 0;

       if (get_kernel_flag() & KERNEL_FLAG_ENABLE_SSR_WCNSS)
       {
		pr_info("%s: trigger wcnss restart\n", __func__);

		ret = smsm_change_state_ssr(SMSM_APPS_STATE, 0, SMSM_RESET, KERNEL_FLAG_ENABLE_SSR_WCNSS);

		if(ret == 0)
			pr_info("%s: set smsm state SMSM_RESET success\n", __func__);
		else
		{
			pr_info("%s: set smsm state SMSM_RESET faild => ret = %d\n", __func__, ret);
			s += sprintf(buf, "Failed");
			return s - buf;
		}
		s += sprintf(buf, "Success");
       }
	else
	{
		pr_info("%s: kernel_flag = 0x%X\n", __func__, (int)get_kernel_flag());
		s += sprintf(buf, "Please check whether kernel flag 6 is 0x1000");
	}

	return s - buf;
}

#define subsystem_restart_ro_attr(_name) \
	static struct kobj_attribute _name##_attr = {  \
		.attr   = {                             \
			.name = __stringify(_name),     \
			.mode = 0444,                   \
		},                                      \
		.show   = _name##_show,                 \
		.store  = NULL,         \
	}

subsystem_restart_ro_attr(subsystem_restart_reason);
subsystem_restart_ro_attr(subsystem_restart_modem_trigger);
subsystem_restart_ro_attr(subsystem_restart_wcnss_trigger);

static struct attribute *g[] = {
	&subsystem_restart_reason_attr.attr,
	&subsystem_restart_modem_trigger_attr.attr,
	&subsystem_restart_wcnss_trigger_attr.attr,
	NULL,
};

static struct attribute_group attr_group = {
	.attrs = g,
};

static int __init subsys_restart_init(void)
{
	int ret = 0;
	struct kobject *properties_kobj;

	ssr_have_set_restart_reason = 0;

#if defined(CONFIG_MSM_SSR_INDEPENDENT)
	if (board_mfg_mode() == 0)
		restart_level = RESET_SUBSYS_INDEPENDENT;
	else
		restart_level = RESET_SOC;
#else
	restart_level = RESET_SOC;
#endif

	if (get_kernel_flag() & (KERNEL_FLAG_ENABLE_SSR_MODEM | KERNEL_FLAG_ENABLE_SSR_WCNSS))
		restart_level = RESET_SUBSYS_INDEPENDENT;

	pr_info("%s: restart_level set to %d, board_mfg_mode %d\n", __func__, restart_level, board_mfg_mode());

	init_waitqueue_head(&subsystem_restart_wq);
	subsystem_restart_state = SUBSYSTEM_RESTART_STATE_NONE;

	properties_kobj = kobject_create_and_add("subsystem_restart_properties", NULL);
	if (properties_kobj) {
		ret = sysfs_create_group(properties_kobj, &attr_group);
		if (ret) {
			pr_err("subsys_restart_init: sysfs_create_group failed\n");
			return ret;
		}
	}

	ssr_wq = alloc_workqueue("ssr_wq", 0, 0);

	if (!ssr_wq)
		panic("Couldn't allocate workqueue for subsystem restart.\n");

	ret = ssr_init_soc_restart_orders();

	return ret;
}

arch_initcall(subsys_restart_init);

MODULE_DESCRIPTION("Subsystem Restart Driver");
MODULE_LICENSE("GPL v2");
