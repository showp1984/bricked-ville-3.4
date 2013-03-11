#ifndef __ARCH_ARM_MACH_MSM_HTC_UTIL_H
#define __ARCH_ARM_MACH_MSM_HTC_UTIL_H

#ifdef CONFIG_HTC_UTIL
void htc_pm_monitor_init(void);
void htc_monitor_init(void);
void htc_idle_stat_add(int sleep_mode, u32 time);
void htc_xo_block_clks_count(void);
#else
static inline void htc_pm_monitor_init(void)
{
        return;
}
static inline void htc_monitor_init(void)
{
        return;
}
static inline void htc_idle_stat_add(int sleep_mode, u32 time)
{
        return;
}
static inline void htc_xo_block_clks_count(void)
{
        return;
}
#endif

#endif
