#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/hrtimer.h>
#include <linux/cpu.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/cpufreq.h>
#include <linux/notifier.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/sched.h>
#include <linux/spinlock.h>

#include <mach/cpuidle.h>

struct sleep_data {
	atomic_t timer_val_ms;
	atomic_t timer_expired;
	struct attribute_group *attr_group;
	struct kobject *kobj;
	struct notifier_block notifier;
	struct work_struct work;
};

struct sleep_data core_sleep_info;
DEFINE_PER_CPU(struct hrtimer, core_sleep_timer);
struct workqueue_struct *adaptive_wq;

static void idle_enter(int cpu)
{
	struct sleep_data *sleep_info = &core_sleep_info;
	struct hrtimer *timer = &per_cpu(core_sleep_timer, cpu);

	if (sleep_info)
		hrtimer_cancel(timer);
}

static void idle_exit(int cpu)
{
	struct sleep_data *sleep_info = &core_sleep_info;
	struct hrtimer *timer = &per_cpu(core_sleep_timer, cpu);

	if (sleep_info && cpu_online(cpu) && cpu_active(cpu)) {
		if (atomic_read(&sleep_info->timer_val_ms) != INT_MAX &&
			atomic_read(&sleep_info->timer_val_ms) &&
			!atomic_read(&sleep_info->timer_expired))
			hrtimer_start(timer,
				ktime_set(0,
				atomic_read(&sleep_info->timer_val_ms) * NSEC_PER_MSEC),
				HRTIMER_MODE_REL_PINNED);
	}
}

static int msm_idle_stats_notified(struct notifier_block *nb,
	unsigned long val, void *v)
{
	if (val == MSM_CPUIDLE_STATE_EXIT)
		idle_exit(smp_processor_id());
	else
		idle_enter(smp_processor_id());

	return 0;
}

static void notify_uspace_work_fn(struct work_struct *work)
{
	struct sleep_data *sleep_info = &core_sleep_info;

	
	sysfs_notify(sleep_info->kobj, NULL, "timer_expired");
}

static enum hrtimer_restart timer_func(struct hrtimer *handle)
{
	struct sleep_data *sleep_info = &core_sleep_info;
	struct hrtimer *timer = &per_cpu(core_sleep_timer, smp_processor_id());

	if (!atomic_read(&sleep_info->timer_expired)) {
		atomic_set(&sleep_info->timer_expired, 1);
	}
	queue_work(adaptive_wq, &sleep_info->work);

	if (timer == handle) {
		hrtimer_forward_now(handle, ktime_set(0,
			atomic_read(&sleep_info->timer_val_ms) * NSEC_PER_MSEC));
		return HRTIMER_RESTART;
	}
	else
		return HRTIMER_NORESTART;
}

static ssize_t show_timer_val_ms(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	int val = 0;
	struct sleep_data *sleep_info = NULL;

	sleep_info = &core_sleep_info;
	val = atomic_read(&sleep_info->timer_val_ms);
	atomic_sub(val, &sleep_info->timer_val_ms);

	return sprintf(buf, "%d\n", val);
}

static ssize_t store_timer_val_ms(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	int val = 0;
	struct sleep_data *sleep_info = NULL;

	sleep_info = &core_sleep_info;
	sscanf(buf, "%du", &val);
	atomic_set(&sleep_info->timer_val_ms, val);

	return count;
}

static ssize_t show_timer_expired(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	int val = 0;
	struct sleep_data *sleep_info = NULL;

	sleep_info = &core_sleep_info;
	val = atomic_read(&sleep_info->timer_expired);
	atomic_set(&sleep_info->timer_expired, 0);

	return sprintf(buf, "%d\n", val);
}

#define MSM_SLEEP_RO_ATTRIB(att) ({ \
		struct attribute *attrib = NULL; \
		struct kobj_attribute *ptr = NULL; \
		ptr = kzalloc(sizeof(struct kobj_attribute), GFP_KERNEL); \
		if (ptr) { \
			ptr->attr.name = #att; \
			ptr->attr.mode = S_IRUGO; \
			ptr->show = show_##att; \
			ptr->store = NULL; \
			attrib = &ptr->attr; \
		} \
		attrib; })

#define MSM_SLEEP_RW_ATTRIB(att) ({ \
		struct attribute *attrib = NULL; \
		struct kobj_attribute *ptr = NULL; \
		ptr = kzalloc(sizeof(struct kobj_attribute), GFP_KERNEL); \
		if (ptr) { \
			ptr->attr.name = #att; \
			ptr->attr.mode = S_IWUSR; \
			ptr->show = show_##att; \
			ptr->store = store_##att; \
			attrib = &ptr->attr; \
		} \
		attrib; })


static int add_sysfs_objects(struct sleep_data *sleep_info)
{
	int err = 0;
	int i = 0;
	const int attr_count = 3;

	struct attribute **attribs =
		kzalloc(sizeof(struct attribute *) * attr_count, GFP_KERNEL);

	if (!attribs)
		return -ENOMEM;

	atomic_set(&sleep_info->timer_expired, 0);
	atomic_set(&sleep_info->timer_val_ms, INT_MAX);

	attribs[0] = MSM_SLEEP_RW_ATTRIB(timer_val_ms);
	attribs[1] = MSM_SLEEP_RO_ATTRIB(timer_expired);
	attribs[2] = NULL;

	for (i = 0; i < attr_count - 1 ; i++) {
		if (!attribs[i])
			goto release_attribs;
	}

	sleep_info->attr_group = kzalloc(sizeof(struct attribute_group),
						GFP_KERNEL);
	if (!sleep_info->attr_group)
		goto release_attribs;
	sleep_info->attr_group->attrs = attribs;
	sleep_info->kobj = kobject_create_and_add("sleep-stats",
			&get_cpu_device(0)->kobj);
	if (!sleep_info->kobj)
		goto release_attr_group;

	err = sysfs_create_group(sleep_info->kobj, sleep_info->attr_group);
	if (err)
		kobject_put(sleep_info->kobj);
	else
		kobject_uevent(sleep_info->kobj, KOBJ_ADD);

	if (!err)
		return err;

release_attr_group:
	kfree(sleep_info->attr_group);
release_attribs:
	for (i = 0; i < attr_count - 1 ; i++)
		if (attribs[i])
			kfree(attribs[i]);
	kfree(attribs);

	return -ENOMEM;
}

static void remove_sysfs_objects(struct sleep_data *sleep_info)
{
	if (sleep_info->kobj)
		sysfs_remove_group(sleep_info->kobj, sleep_info->attr_group);

	kfree(sleep_info->attr_group);
	kfree(sleep_info->kobj);
}

static int __init msm_sleep_info_init(void)
{
	int err = 0, cpu = 0;
	struct sleep_data *sleep_info = NULL;
	struct hrtimer *timer;

	printk(KERN_INFO "msm_sleep_stats: Initializing sleep stats ");
	sleep_info = &core_sleep_info;
	adaptive_wq = create_singlethread_workqueue("adaptive");
	INIT_WORK(&sleep_info->work, notify_uspace_work_fn);

	sleep_info->notifier.notifier_call = msm_idle_stats_notified;

	for_each_possible_cpu(cpu) {
		timer = &per_cpu(core_sleep_timer, cpu);
		hrtimer_init(timer,  CLOCK_MONOTONIC,
			HRTIMER_MODE_REL);
		timer->function = timer_func;
		err = msm_cpuidle_register_notifier(cpu,
					&sleep_info->notifier);
		if (err) {
			pr_err("%s: failed to register idle notification\n", __func__);
		}
	}

	
	err = add_sysfs_objects(sleep_info);
	if (err) {
		printk(KERN_INFO "msm_sleep_stats: Failed to initialize sleep stats");
		remove_sysfs_objects(sleep_info);
	}

	return 0;
}
late_initcall(msm_sleep_info_init);

