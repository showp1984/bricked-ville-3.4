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

#define pr_fmt(fmt) "%s: " fmt, __func__

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/cpufreq.h>
#include <linux/cpu.h>
#include <linux/regulator/consumer.h>
#include <linux/platform_device.h>

#include <asm/mach-types.h>
#include <asm/cpu.h>

#include <mach/board.h>
#include <mach/msm_iomap.h>
#include <mach/rpm-regulator.h>
#include <mach/msm_bus.h>
#include <mach/msm_bus_board.h>
#include <mach/socinfo.h>
#include <mach/msm-krait-l2-accessors.h>
#include <mach/rpm-regulator.h>

#include "acpuclock.h"
#include "pm.h"
#include <mach/board_htc.h>

#ifdef pr_err
#undef pr_err
#endif
#define pr_err(fmt, args...) \
	printk(KERN_ERR "[ACPU] " pr_fmt(fmt), ## args)

#ifdef pr_warn
#undef pr_warn
#endif
#define pr_warn(fmt, args...) \
	printk(KERN_WARNING "[ACPU] " pr_fmt(fmt), ## args)

#ifdef pr_info
#undef pr_info
#endif
#define pr_info(fmt, args...) \
	printk(KERN_INFO "[ACPU] " pr_fmt(fmt), ## args)

#if defined(DEBUG)
#ifdef pr_debug
#undef pr_debug
#endif
#define pr_debug(fmt, args...) \
	printk(KERN_DEBUG "[ACPU] " pr_fmt(fmt), ## args)
#endif

#define CPU_FOOT_PRINT_MAGIC				0xACBDFE00
#define CPU_FOOT_PRINT_BASE_CPU0_VIRT		(MSM_KERNEL_FOOTPRINT_BASE + 0x0)
static void set_acpuclk_foot_print(unsigned cpu, unsigned state)
{
	unsigned *status = (unsigned *)(CPU_FOOT_PRINT_BASE_CPU0_VIRT + 0x6C) + cpu;
	*status = (CPU_FOOT_PRINT_MAGIC | state);
	mb();
}

static void set_acpuclk_cpu_freq_foot_print(unsigned cpu, unsigned khz)
{
	unsigned *status = (unsigned *)(CPU_FOOT_PRINT_BASE_CPU0_VIRT + 0x58) + cpu;
	*status = khz;
	mb();
}

static void set_acpuclk_L2_freq_foot_print(unsigned khz)
{
	unsigned *status = (unsigned *)(CPU_FOOT_PRINT_BASE_CPU0_VIRT + 0x68);
	*status = khz;
	mb();
}

#define PLL_8			 0
#define HFPLL			-1
#define QSB			-2

#define PRI_SRC_SEL_SEC_SRC	0
#define PRI_SRC_SEL_HFPLL	1
#define PRI_SRC_SEL_HFPLL_DIV2	2
#define SEC_SRC_SEL_QSB		0
#define SEC_SRC_SEL_AUX		2

#define HFPLL_MODE		0x00
#define HFPLL_CONFIG_CTL	0x04
#define HFPLL_L_VAL		0x08
#define HFPLL_M_VAL		0x0C
#define HFPLL_N_VAL		0x10
#define HFPLL_DROOP_CTL		0x14

#define L2CPMR_IADDR		0x500
#define L2CPUCPMR_IADDR		0x501

#define STBY_KHZ		1

#define HFPLL_LOW_VDD_PLL_L_MAX	0x28

#define SECCLKAGD		BIT(4)

#define QFPROM_PTE_EFUSE_ADDR	(MSM_QFPROM_BASE + 0x00C0)

#define LVL_NONE	RPM_VREG_CORNER_NONE
#define LVL_LOW	RPM_VREG_CORNER_LOW
#define LVL_NOM	RPM_VREG_CORNER_NOMINAL
#define LVL_HIGH	RPM_VREG_CORNER_HIGH

enum scalables {
	CPU0 = 0,
	CPU1,
	CPU2,
	CPU3,
	L2,
	NUM_SCALABLES
};

enum vregs {
	VREG_CORE,
	VREG_MEM,
	VREG_DIG,
	VREG_HFPLL_A,
	VREG_HFPLL_B,
	NUM_VREG
};

enum hfpll_vdd_levels {
	HFPLL_VDD_NONE,
	HFPLL_VDD_LOW,
	HFPLL_VDD_NOM
};

enum pvs {
	PVS_SLOW,
	PVS_NOM,
	PVS_FAST,
	PVS_FASTER,
	NUM_PVS
};

struct vreg {
	const char name[15];
	const unsigned int max_vdd;
	const int rpm_vreg_voter;
	const int rpm_vreg_id;
	struct regulator *reg;
	unsigned int cur_vdd;
};

struct core_speed {
	unsigned int		khz;
	int			src;
	unsigned int		pri_src_sel;
	unsigned int		sec_src_sel;
	unsigned int		pll_l_val;
};

struct l2_level {
	struct core_speed	speed;
	unsigned int		vdd_dig;
	unsigned int		vdd_mem;
	unsigned int		bw_level;
};

struct acpu_level {
	unsigned int		use_for_scaling;
	struct core_speed	speed;
	struct l2_level		*l2_level;
	unsigned int		vdd_core;
};

struct scalable {
	void * __iomem const hfpll_base;
	void * __iomem const aux_clk_sel;
	const uint32_t l2cpmr_iaddr;
	struct core_speed *current_speed;
	struct l2_level *l2_vote;
	struct vreg vreg[NUM_VREG];
	unsigned int *hfpll_vdd_tbl;
	bool regulators_initialized;
	bool clocks_initialized;
};

static unsigned int hfpll_vdd_tbl_8960[] = {
	[HFPLL_VDD_NONE] = 0,
	[HFPLL_VDD_LOW]  = 850000,
	[HFPLL_VDD_NOM]  = 1050000
};

static unsigned int hfpll_vdd_tbl_8064[] = {
	[HFPLL_VDD_NONE] = 0,
	[HFPLL_VDD_LOW]  = 945000,
	[HFPLL_VDD_NOM]  = 1050000
};

static unsigned int hfpll_vdd_dig_tbl_8930[] = {
	[HFPLL_VDD_NONE] = LVL_NONE,
	[HFPLL_VDD_LOW]  = LVL_LOW,
	[HFPLL_VDD_NOM]  = LVL_NOM
};

static struct scalable scalable_8960[] = {
	[CPU0] = {
			.hfpll_base      = MSM_HFPLL_BASE + 0x200,
			.aux_clk_sel     = MSM_ACC0_BASE  + 0x014,
			.l2cpmr_iaddr    = L2CPUCPMR_IADDR,
			.vreg[VREG_CORE] = { "krait0",     1300000 },
			.vreg[VREG_MEM]  = { "krait0_mem", 1150000,
					     RPM_VREG_VOTER1,
					     RPM_VREG_ID_PM8921_L24 },
			.vreg[VREG_DIG]  = { "krait0_dig", 1150000,
					     RPM_VREG_VOTER1,
					     RPM_VREG_ID_PM8921_S3 },
			.vreg[VREG_HFPLL_A] = { "hfpll0_s8", 2100000,
					     RPM_VREG_VOTER1,
					     RPM_VREG_ID_PM8921_S8 },
			.vreg[VREG_HFPLL_B] = { "hfpll0_l23", 1800000,
					     RPM_VREG_VOTER1,
					     RPM_VREG_ID_PM8921_L23 },
		},
	[CPU1] = {
			.hfpll_base      = MSM_HFPLL_BASE + 0x300,
			.aux_clk_sel     = MSM_ACC1_BASE  + 0x014,
			.l2cpmr_iaddr    = L2CPUCPMR_IADDR,
			.vreg[VREG_CORE] = { "krait1",     1300000 },
			.vreg[VREG_MEM]  = { "krait1_mem", 1150000,
					     RPM_VREG_VOTER2,
					     RPM_VREG_ID_PM8921_L24 },
			.vreg[VREG_DIG]  = { "krait1_dig", 1150000,
					     RPM_VREG_VOTER2,
					     RPM_VREG_ID_PM8921_S3 },
			.vreg[VREG_HFPLL_A] = { "hfpll1_s8", 2100000,
					     RPM_VREG_VOTER2,
					     RPM_VREG_ID_PM8921_S8 },
			.vreg[VREG_HFPLL_B] = { "hfpll1_l23", 1800000,
					     RPM_VREG_VOTER2,
					     RPM_VREG_ID_PM8921_L23 },
		},
	[L2] = {
			.hfpll_base   = MSM_HFPLL_BASE    + 0x400,
			.hfpll_vdd_tbl = hfpll_vdd_tbl_8960,
			.aux_clk_sel  = MSM_APCS_GCC_BASE + 0x028,
			.l2cpmr_iaddr = L2CPMR_IADDR,
			.vreg[VREG_HFPLL_A] = { "hfpll_l2_s8", 2100000,
					     RPM_VREG_VOTER6,
					     RPM_VREG_ID_PM8921_S8 },
			.vreg[VREG_HFPLL_B] = { "hfpll_l2_l23", 1800000,
					     RPM_VREG_VOTER6,
					     RPM_VREG_ID_PM8921_L23 },
		},
};

static DEFINE_MUTEX(driver_lock);
static DEFINE_SPINLOCK(l2_lock);

static struct scalable scalable_8064[] = {
	[CPU0] = {
			.hfpll_base      = MSM_HFPLL_BASE + 0x200,
			.aux_clk_sel     = MSM_ACC0_BASE  + 0x014,
			.l2cpmr_iaddr    = L2CPUCPMR_IADDR,
			.vreg[VREG_CORE] = { "krait0",     1300000 },
			.vreg[VREG_MEM]  = { "krait0_mem", 1150000,
					     RPM_VREG_VOTER1,
					     RPM_VREG_ID_PM8921_L24 },
			.vreg[VREG_DIG]  = { "krait0_dig", 1150000,
					     RPM_VREG_VOTER1,
					     RPM_VREG_ID_PM8921_S3 },
			.vreg[VREG_HFPLL_B] = { "hfpll0", 1800000,
					     RPM_VREG_VOTER1,
					     RPM_VREG_ID_PM8921_LVS7 },
		},
	[CPU1] = {
			.hfpll_base      = MSM_HFPLL_BASE + 0x240,
			.aux_clk_sel     = MSM_ACC1_BASE  + 0x014,
			.l2cpmr_iaddr    = L2CPUCPMR_IADDR,
			.vreg[VREG_CORE] = { "krait1",     1300000 },
			.vreg[VREG_MEM]  = { "krait1_mem", 1150000,
					     RPM_VREG_VOTER2,
					     RPM_VREG_ID_PM8921_L24 },
			.vreg[VREG_DIG]  = { "krait1_dig", 1150000,
					     RPM_VREG_VOTER2,
					     RPM_VREG_ID_PM8921_S3 },
			.vreg[VREG_HFPLL_B] = { "hfpll1", 1800000,
					     RPM_VREG_VOTER2,
					     RPM_VREG_ID_PM8921_LVS7 },
		},
	[CPU2] = {
			.hfpll_base      = MSM_HFPLL_BASE + 0x280,
			.aux_clk_sel     = MSM_ACC2_BASE  + 0x014,
			.l2cpmr_iaddr    = L2CPUCPMR_IADDR,
			.vreg[VREG_CORE] = { "krait2",     1300000 },
			.vreg[VREG_MEM]  = { "krait2_mem", 1150000,
					     RPM_VREG_VOTER4,
					     RPM_VREG_ID_PM8921_L24 },
			.vreg[VREG_DIG]  = { "krait2_dig", 1150000,
					     RPM_VREG_VOTER4,
					     RPM_VREG_ID_PM8921_S3 },
			.vreg[VREG_HFPLL_B] = { "hfpll2", 1800000,
					     RPM_VREG_VOTER4,
					     RPM_VREG_ID_PM8921_LVS7 },
		},
	[CPU3] = {
			.hfpll_base      = MSM_HFPLL_BASE + 0x2C0,
			.aux_clk_sel     = MSM_ACC3_BASE  + 0x014,
			.l2cpmr_iaddr    = L2CPUCPMR_IADDR,
			.vreg[VREG_CORE] = { "krait3",     1300000 },
			.vreg[VREG_MEM]  = { "krait3_mem", 1150000,
					     RPM_VREG_VOTER5,
					     RPM_VREG_ID_PM8921_L24 },
			.vreg[VREG_DIG]  = { "krait3_dig", 1150000,
					     RPM_VREG_VOTER5,
					     RPM_VREG_ID_PM8921_S3 },
			.vreg[VREG_HFPLL_B] = { "hfpll3", 1800000,
					     RPM_VREG_VOTER5,
					     RPM_VREG_ID_PM8921_LVS7 },
		},
	[L2] = {
			.hfpll_base   = MSM_HFPLL_BASE    + 0x300,
			.hfpll_vdd_tbl = hfpll_vdd_tbl_8064,
			.aux_clk_sel  = MSM_APCS_GCC_BASE + 0x028,
			.l2cpmr_iaddr = L2CPMR_IADDR,
			.vreg[VREG_HFPLL_B] = { "hfpll_l2", 1800000,
					     RPM_VREG_VOTER6,
					     RPM_VREG_ID_PM8921_LVS7 },
		},
};

static struct scalable scalable_8930[] = {
	[CPU0] = {
			.hfpll_base      = MSM_HFPLL_BASE + 0x200,
			.aux_clk_sel     = MSM_ACC0_BASE  + 0x014,
			.l2cpmr_iaddr    = L2CPUCPMR_IADDR,
			.vreg[VREG_CORE] = { "krait0",     1300000 },
			.vreg[VREG_MEM]  = { "krait0_mem", 1150000,
					     RPM_VREG_VOTER1,
					     RPM_VREG_ID_PM8038_L24 },
			.vreg[VREG_DIG]  = { "krait0_dig", LVL_HIGH,
					     RPM_VREG_VOTER1,
					     RPM_VREG_ID_PM8038_VDD_DIG_CORNER
					   },
			.vreg[VREG_HFPLL_B] = { "hfpll0", 1800000,
					     RPM_VREG_VOTER1,
					     RPM_VREG_ID_PM8038_L23 },
		},
	[CPU1] = {
			.hfpll_base      = MSM_HFPLL_BASE + 0x300,
			.aux_clk_sel     = MSM_ACC1_BASE  + 0x014,
			.l2cpmr_iaddr    = L2CPUCPMR_IADDR,
			.vreg[VREG_CORE] = { "krait1",     1300000 },
			.vreg[VREG_MEM]  = { "krait1_mem", 1150000,
					     RPM_VREG_VOTER2,
					     RPM_VREG_ID_PM8038_L24 },
			.vreg[VREG_DIG]  = { "krait1_dig", LVL_HIGH,
					     RPM_VREG_VOTER2,
					     RPM_VREG_ID_PM8038_VDD_DIG_CORNER
					   },
			.vreg[VREG_HFPLL_B] = { "hfpll1", 1800000,
					     RPM_VREG_VOTER2,
					     RPM_VREG_ID_PM8038_L23 },
		},
	[L2] = {
			.hfpll_base   = MSM_HFPLL_BASE    + 0x400,
			.hfpll_vdd_tbl = hfpll_vdd_dig_tbl_8930,
			.aux_clk_sel  = MSM_APCS_GCC_BASE + 0x028,
			.l2cpmr_iaddr = L2CPMR_IADDR,
			.vreg[VREG_HFPLL_B] = { "hfpll_l2", 1800000,
					     RPM_VREG_VOTER6,
					     RPM_VREG_ID_PM8038_L23 },
		},
};

static struct scalable scalable_8627[] = {
	[CPU0] = {
			.hfpll_base      = MSM_HFPLL_BASE + 0x200,
			.aux_clk_sel     = MSM_ACC0_BASE  + 0x014,
			.l2cpmr_iaddr    = L2CPUCPMR_IADDR,
			.vreg[VREG_CORE] = { "krait0",     1300000 },
			.vreg[VREG_MEM]  = { "krait0_mem", 1150000,
					     RPM_VREG_VOTER1,
					     RPM_VREG_ID_PM8038_L24 },
			.vreg[VREG_DIG]  = { "krait0_dig", LVL_HIGH,
					     RPM_VREG_VOTER1,
					     RPM_VREG_ID_PM8038_VDD_DIG_CORNER
					   },
			.vreg[VREG_HFPLL_B] = { "hfpll0", 1800000,
					     RPM_VREG_VOTER1,
					     RPM_VREG_ID_PM8038_L23 },
		},
	[CPU1] = {
			.hfpll_base      = MSM_HFPLL_BASE + 0x300,
			.aux_clk_sel     = MSM_ACC1_BASE  + 0x014,
			.l2cpmr_iaddr    = L2CPUCPMR_IADDR,
			.vreg[VREG_CORE] = { "krait1",     1300000 },
			.vreg[VREG_MEM]  = { "krait1_mem", 1150000,
					     RPM_VREG_VOTER2,
					     RPM_VREG_ID_PM8038_L24 },
			.vreg[VREG_DIG]  = { "krait1_dig", LVL_HIGH,
					     RPM_VREG_VOTER2,
					     RPM_VREG_ID_PM8038_VDD_DIG_CORNER
					   },
			.vreg[VREG_HFPLL_B] = { "hfpll1", 1800000,
					     RPM_VREG_VOTER2,
					     RPM_VREG_ID_PM8038_L23 },
		},
	[L2] = {
			.hfpll_base   = MSM_HFPLL_BASE    + 0x400,
			.hfpll_vdd_tbl = hfpll_vdd_dig_tbl_8930,
			.aux_clk_sel  = MSM_APCS_GCC_BASE + 0x028,
			.l2cpmr_iaddr = L2CPMR_IADDR,
			.vreg[VREG_HFPLL_B] = { "hfpll_l2", 1800000,
					     RPM_VREG_VOTER6,
					     RPM_VREG_ID_PM8038_L23 },
		},
};

static struct l2_level *l2_freq_tbl;
static struct acpu_level *acpu_freq_tbl;
static int l2_freq_tbl_size;
static struct scalable *scalable;
#define SCALABLE_TO_CPU(sc) ((sc) - scalable)

#define BW_MBPS(_bw) \
	{ \
		.vectors = (struct msm_bus_vectors[]){ \
			{\
				.src = MSM_BUS_MASTER_AMPSS_M0, \
				.dst = MSM_BUS_SLAVE_EBI_CH0, \
				.ib = (_bw) * 1000000UL, \
				.ab = (_bw) *  100000UL, \
			}, \
			{ \
				.src = MSM_BUS_MASTER_AMPSS_M1, \
				.dst = MSM_BUS_SLAVE_EBI_CH0, \
				.ib = (_bw) * 1000000UL, \
				.ab = (_bw) *  100000UL, \
			}, \
		}, \
		.num_paths = 2, \
	}
static struct msm_bus_paths bw_level_tbl[] = {
	[0] =  BW_MBPS(640), 
	[1] = BW_MBPS(1064), 
	[2] = BW_MBPS(1600), 
	[3] = BW_MBPS(2128), 
	[4] = BW_MBPS(3200), 
	[5] = BW_MBPS(3600), 
	[6] = BW_MBPS(3936), 
	[7] = BW_MBPS(4264), 
};

static struct msm_bus_scale_pdata bus_client_pdata = {
	.usecase = bw_level_tbl,
	.num_usecases = ARRAY_SIZE(bw_level_tbl),
	.active_only = 1,
	.name = "acpuclock",
};

static uint32_t bus_perf_client;

#define L2(x) (&l2_freq_tbl_8960_kraitv1[(x)])
static struct l2_level l2_freq_tbl_8960_kraitv1[] = {
	[0]  = { {STBY_KHZ, QSB,   0, 0, 0x00 }, 1050000, 1050000, 0 },
	[1]  = { {  384000, PLL_8, 0, 2, 0x00 }, 1050000, 1050000, 1 },
	[2]  = { {  432000, HFPLL, 2, 0, 0x20 }, 1050000, 1050000, 1 },
	[3]  = { {  486000, HFPLL, 2, 0, 0x24 }, 1050000, 1050000, 1 },
	[4]  = { {  540000, HFPLL, 2, 0, 0x28 }, 1050000, 1050000, 1 },
	[5]  = { {  594000, HFPLL, 1, 0, 0x16 }, 1050000, 1050000, 2 },
	[6]  = { {  648000, HFPLL, 1, 0, 0x18 }, 1050000, 1050000, 2 },
	[7]  = { {  702000, HFPLL, 1, 0, 0x1A }, 1050000, 1050000, 2 },
	[8]  = { {  756000, HFPLL, 1, 0, 0x1C }, 1150000, 1150000, 2 },
	[9]  = { {  810000, HFPLL, 1, 0, 0x1E }, 1150000, 1150000, 3 },
	[10] = { {  864000, HFPLL, 1, 0, 0x20 }, 1150000, 1150000, 3 },
	[11] = { {  918000, HFPLL, 1, 0, 0x22 }, 1150000, 1150000, 3 },
};

static struct acpu_level acpu_freq_tbl_8960_kraitv1_slow[] = {
	{ 0, {STBY_KHZ, QSB,   0, 0, 0x00 }, L2(0),   900000 },
	{ 1, {  384000, PLL_8, 0, 2, 0x00 }, L2(1),   900000 },
	{ 1, {  432000, HFPLL, 2, 0, 0x20 }, L2(6),   925000 },
	{ 1, {  486000, HFPLL, 2, 0, 0x24 }, L2(6),   925000 },
	{ 1, {  540000, HFPLL, 2, 0, 0x28 }, L2(6),   937500 },
	{ 1, {  594000, HFPLL, 1, 0, 0x16 }, L2(6),   962500 },
	{ 1, {  648000, HFPLL, 1, 0, 0x18 }, L2(6),   987500 },
	{ 1, {  702000, HFPLL, 1, 0, 0x1A }, L2(6),  1000000 },
	{ 1, {  756000, HFPLL, 1, 0, 0x1C }, L2(11), 1025000 },
	{ 1, {  810000, HFPLL, 1, 0, 0x1E }, L2(11), 1062500 },
	{ 1, {  864000, HFPLL, 1, 0, 0x20 }, L2(11), 1062500 },
	{ 1, {  918000, HFPLL, 1, 0, 0x22 }, L2(11), 1087500 },
	{ 0, { 0 } }
};

static struct acpu_level acpu_freq_tbl_8960_kraitv1_nom_fast[] = {
	{ 0, {STBY_KHZ, QSB,   0, 0, 0x00 }, L2(0),   862500 },
	{ 1, {  384000, PLL_8, 0, 2, 0x00 }, L2(1),   862500 },
	{ 1, {  432000, HFPLL, 2, 0, 0x20 }, L2(6),   862500 },
	{ 1, {  486000, HFPLL, 2, 0, 0x24 }, L2(6),   887500 },
	{ 1, {  540000, HFPLL, 2, 0, 0x28 }, L2(6),   900000 },
	{ 1, {  594000, HFPLL, 1, 0, 0x16 }, L2(6),   925000 },
	{ 1, {  648000, HFPLL, 1, 0, 0x18 }, L2(6),   925000 },
	{ 1, {  702000, HFPLL, 1, 0, 0x1A }, L2(6),   937500 },
	{ 1, {  756000, HFPLL, 1, 0, 0x1C }, L2(11),  962500 },
	{ 1, {  810000, HFPLL, 1, 0, 0x1E }, L2(11), 1012500 },
	{ 1, {  864000, HFPLL, 1, 0, 0x20 }, L2(11), 1025000 },
	{ 1, {  918000, HFPLL, 1, 0, 0x22 }, L2(11), 1025000 },
	{ 0, { 0 } }
};

#undef L2
#define L2(x) (&l2_freq_tbl_8960_kraitv2[(x)])
static struct l2_level l2_freq_tbl_8960_kraitv2[] = {
	[0]  = { {STBY_KHZ, QSB,   0, 0, 0x00 }, 1050000, 1050000, 0 },
	[1]  = { {  384000, PLL_8, 0, 2, 0x00 }, 1050000, 1050000, 1 },
	[2]  = { {  432000, HFPLL, 2, 0, 0x20 }, 1050000, 1050000, 2 },
	[3]  = { {  486000, HFPLL, 2, 0, 0x24 }, 1050000, 1050000, 2 },
	[4]  = { {  540000, HFPLL, 2, 0, 0x28 }, 1050000, 1050000, 2 },
	[5]  = { {  594000, HFPLL, 1, 0, 0x16 }, 1050000, 1050000, 2 },
	[6]  = { {  648000, HFPLL, 1, 0, 0x18 }, 1050000, 1050000, 4 },
	[7]  = { {  702000, HFPLL, 1, 0, 0x1A }, 1050000, 1050000, 4 },
	[8]  = { {  756000, HFPLL, 1, 0, 0x1C }, 1150000, 1150000, 4 },
	[9]  = { {  810000, HFPLL, 1, 0, 0x1E }, 1150000, 1150000, 4 },
	[10] = { {  864000, HFPLL, 1, 0, 0x20 }, 1150000, 1150000, 4 },
	[11] = { {  918000, HFPLL, 1, 0, 0x22 }, 1150000, 1150000, 6 },
	[12] = { {  972000, HFPLL, 1, 0, 0x24 }, 1150000, 1150000, 6 },
	[13] = { { 1026000, HFPLL, 1, 0, 0x26 }, 1150000, 1150000, 6 },
	[14] = { { 1080000, HFPLL, 1, 0, 0x28 }, 1150000, 1150000, 6 },
	[15] = { { 1134000, HFPLL, 1, 0, 0x2A }, 1150000, 1150000, 6 },
	[16] = { { 1188000, HFPLL, 1, 0, 0x2C }, 1150000, 1150000, 6 },
	[17] = { { 1242000, HFPLL, 1, 0, 0x2E }, 1150000, 1150000, 6 },
	[18] = { { 1296000, HFPLL, 1, 0, 0x30 }, 1150000, 1150000, 6 },
	[19] = { { 1350000, HFPLL, 1, 0, 0x32 }, 1150000, 1150000, 6 },
};

static struct acpu_level acpu_freq_tbl_8960_kraitv2_slow[] = {
	{ 0, { STBY_KHZ, QSB,   0, 0, 0x00 }, L2(0),   950000 },
	{ 1, {   384000, PLL_8, 0, 2, 0x00 }, L2(1),   950000 },
	{ 0, {   432000, HFPLL, 2, 0, 0x20 }, L2(7),   975000 },
	{ 1, {   486000, HFPLL, 2, 0, 0x24 }, L2(7),   975000 },
	{ 0, {   540000, HFPLL, 2, 0, 0x28 }, L2(7),  1000000 },
	{ 1, {   594000, HFPLL, 1, 0, 0x16 }, L2(7),  1000000 },
	{ 0, {   648000, HFPLL, 1, 0, 0x18 }, L2(7),  1025000 },
	{ 1, {   702000, HFPLL, 1, 0, 0x1A }, L2(7),  1025000 },
	{ 0, {   756000, HFPLL, 1, 0, 0x1C }, L2(7),  1075000 },
	{ 1, {   810000, HFPLL, 1, 0, 0x1E }, L2(7),  1075000 },
	{ 0, {   864000, HFPLL, 1, 0, 0x20 }, L2(7),  1100000 },
	{ 1, {   918000, HFPLL, 1, 0, 0x22 }, L2(7),  1100000 },
	{ 0, {   972000, HFPLL, 1, 0, 0x24 }, L2(7),  1125000 },
	{ 1, {  1026000, HFPLL, 1, 0, 0x26 }, L2(7),  1125000 },
	{ 0, {  1080000, HFPLL, 1, 0, 0x28 }, L2(19), 1175000 },
	{ 1, {  1134000, HFPLL, 1, 0, 0x2A }, L2(19), 1175000 },
	{ 0, {  1188000, HFPLL, 1, 0, 0x2C }, L2(19), 1200000 },
	{ 1, {  1242000, HFPLL, 1, 0, 0x2E }, L2(19), 1200000 },
	{ 0, {  1296000, HFPLL, 1, 0, 0x30 }, L2(19), 1225000 },
	{ 1, {  1350000, HFPLL, 1, 0, 0x32 }, L2(19), 1225000 },
	{ 0, {  1404000, HFPLL, 1, 0, 0x34 }, L2(19), 1237500 },
	{ 1, {  1458000, HFPLL, 1, 0, 0x36 }, L2(19), 1237500 },
	{ 1, {  1512000, HFPLL, 1, 0, 0x38 }, L2(19), 1250000 },
	{ 0, { 0 } }
};

static struct acpu_level acpu_freq_tbl_8960_kraitv2_nom[] = {
	{ 0, { STBY_KHZ, QSB,   0, 0, 0x00 }, L2(0),   900000 },
	{ 1, {   384000, PLL_8, 0, 2, 0x00 }, L2(1),   900000 },
	{ 0, {   432000, HFPLL, 2, 0, 0x20 }, L2(7),   925000 },
	{ 1, {   486000, HFPLL, 2, 0, 0x24 }, L2(7),   925000 },
	{ 0, {   540000, HFPLL, 2, 0, 0x28 }, L2(7),   950000 },
	{ 1, {   594000, HFPLL, 1, 0, 0x16 }, L2(7),   950000 },
	{ 0, {   648000, HFPLL, 1, 0, 0x18 }, L2(7),   975000 },
	{ 1, {   702000, HFPLL, 1, 0, 0x1A }, L2(7),   975000 },
	{ 0, {   756000, HFPLL, 1, 0, 0x1C }, L2(7),  1025000 },
	{ 1, {   810000, HFPLL, 1, 0, 0x1E }, L2(7),  1025000 },
	{ 0, {   864000, HFPLL, 1, 0, 0x20 }, L2(7),  1050000 },
	{ 1, {   918000, HFPLL, 1, 0, 0x22 }, L2(7),  1050000 },
	{ 0, {   972000, HFPLL, 1, 0, 0x24 }, L2(7),  1075000 },
	{ 1, {  1026000, HFPLL, 1, 0, 0x26 }, L2(7),  1075000 },
	{ 0, {  1080000, HFPLL, 1, 0, 0x28 }, L2(19), 1125000 },
	{ 1, {  1134000, HFPLL, 1, 0, 0x2A }, L2(19), 1125000 },
	{ 0, {  1188000, HFPLL, 1, 0, 0x2C }, L2(19), 1150000 },
	{ 1, {  1242000, HFPLL, 1, 0, 0x2E }, L2(19), 1150000 },
	{ 0, {  1296000, HFPLL, 1, 0, 0x30 }, L2(19), 1175000 },
	{ 1, {  1350000, HFPLL, 1, 0, 0x32 }, L2(19), 1175000 },
	{ 0, {  1404000, HFPLL, 1, 0, 0x34 }, L2(19), 1187500 },
	{ 1, {  1458000, HFPLL, 1, 0, 0x36 }, L2(19), 1187500 },
	{ 1, {  1512000, HFPLL, 1, 0, 0x38 }, L2(19), 1200000 },
	{ 0, { 0 } }
};

static struct acpu_level acpu_freq_tbl_8960_kraitv2_fast[] = {
	{ 0, { STBY_KHZ, QSB,   0, 0, 0x00 }, L2(0),   850000 },
	{ 1, {   384000, PLL_8, 0, 2, 0x00 }, L2(1),   850000 },
	{ 0, {   432000, HFPLL, 2, 0, 0x20 }, L2(7),   875000 },
	{ 1, {   486000, HFPLL, 2, 0, 0x24 }, L2(7),   875000 },
	{ 0, {   540000, HFPLL, 2, 0, 0x28 }, L2(7),   900000 },
	{ 1, {   594000, HFPLL, 1, 0, 0x16 }, L2(7),   900000 },
	{ 0, {   648000, HFPLL, 1, 0, 0x18 }, L2(7),   925000 },
	{ 1, {   702000, HFPLL, 1, 0, 0x1A }, L2(7),   925000 },
	{ 0, {   756000, HFPLL, 1, 0, 0x1C }, L2(7),   975000 },
	{ 1, {   810000, HFPLL, 1, 0, 0x1E }, L2(7),   975000 },
	{ 0, {   864000, HFPLL, 1, 0, 0x20 }, L2(7),  1000000 },
	{ 1, {   918000, HFPLL, 1, 0, 0x22 }, L2(7),  1000000 },
	{ 0, {   972000, HFPLL, 1, 0, 0x24 }, L2(7),  1025000 },
	{ 1, {  1026000, HFPLL, 1, 0, 0x26 }, L2(7),  1025000 },
	{ 0, {  1080000, HFPLL, 1, 0, 0x28 }, L2(19), 1075000 },
	{ 1, {  1134000, HFPLL, 1, 0, 0x2A }, L2(19), 1075000 },
	{ 0, {  1188000, HFPLL, 1, 0, 0x2C }, L2(19), 1100000 },
	{ 1, {  1242000, HFPLL, 1, 0, 0x2E }, L2(19), 1100000 },
	{ 0, {  1296000, HFPLL, 1, 0, 0x30 }, L2(19), 1125000 },
	{ 1, {  1350000, HFPLL, 1, 0, 0x32 }, L2(19), 1125000 },
	{ 0, {  1404000, HFPLL, 1, 0, 0x34 }, L2(19), 1137500 },
	{ 1, {  1458000, HFPLL, 1, 0, 0x36 }, L2(19), 1137500 },
	{ 1, {  1512000, HFPLL, 1, 0, 0x38 }, L2(19), 1150000 },
	{ 0, { 0 } }
};

#undef L2
#define L2(x) (&l2_freq_tbl_8064[(x)])
static struct l2_level l2_freq_tbl_8064[] = {
	[0]  = { {STBY_KHZ, QSB,   0, 0, 0x00 }, 1050000, 1050000, 0 },
	[1]  = { {  384000, PLL_8, 0, 2, 0x00 }, 1050000, 1050000, 1 },
	[2]  = { {  432000, HFPLL, 2, 0, 0x20 }, 1050000, 1050000, 2 },
	[3]  = { {  486000, HFPLL, 2, 0, 0x24 }, 1050000, 1050000, 2 },
	[4]  = { {  540000, HFPLL, 2, 0, 0x28 }, 1050000, 1050000, 2 },
	[5]  = { {  594000, HFPLL, 1, 0, 0x16 }, 1050000, 1050000, 2 },
	[6]  = { {  648000, HFPLL, 1, 0, 0x18 }, 1050000, 1050000, 4 },
	[7]  = { {  702000, HFPLL, 1, 0, 0x1A }, 1050000, 1050000, 4 },
	[8]  = { {  756000, HFPLL, 1, 0, 0x1C }, 1150000, 1150000, 4 },
	[9]  = { {  810000, HFPLL, 1, 0, 0x1E }, 1150000, 1150000, 4 },
	[10] = { {  864000, HFPLL, 1, 0, 0x20 }, 1150000, 1150000, 4 },
	[11] = { {  918000, HFPLL, 1, 0, 0x22 }, 1150000, 1150000, 7 },
	[12] = { {  972000, HFPLL, 1, 0, 0x24 }, 1150000, 1150000, 7 },
	[13] = { { 1026000, HFPLL, 1, 0, 0x26 }, 1150000, 1150000, 7 },
	[14] = { { 1080000, HFPLL, 1, 0, 0x28 }, 1150000, 1150000, 7 },
	[15] = { { 1134000, HFPLL, 1, 0, 0x2A }, 1150000, 1150000, 7 },
};

static struct acpu_level acpu_freq_tbl_8064_slow[] = {
	{ 0, { STBY_KHZ, QSB,   0, 0, 0x00 }, L2(0),   950000 },
	{ 1, {   384000, PLL_8, 0, 2, 0x00 }, L2(1),   950000 },
	{ 0, {   432000, HFPLL, 2, 0, 0x20 }, L2(7),   975000 },
	{ 1, {   486000, HFPLL, 2, 0, 0x24 }, L2(7),   975000 },
	{ 0, {   540000, HFPLL, 2, 0, 0x28 }, L2(7),  1000000 },
	{ 1, {   594000, HFPLL, 1, 0, 0x16 }, L2(7),  1000000 },
	{ 0, {   648000, HFPLL, 1, 0, 0x18 }, L2(7),  1025000 },
	{ 1, {   702000, HFPLL, 1, 0, 0x1A }, L2(7),  1025000 },
	{ 0, {   756000, HFPLL, 1, 0, 0x1C }, L2(7),  1075000 },
	{ 1, {   810000, HFPLL, 1, 0, 0x1E }, L2(7),  1075000 },
	{ 0, {   864000, HFPLL, 1, 0, 0x20 }, L2(7),  1100000 },
	{ 1, {   918000, HFPLL, 1, 0, 0x22 }, L2(7),  1100000 },
	{ 0, {   972000, HFPLL, 1, 0, 0x24 }, L2(7),  1125000 },
	{ 1, {  1026000, HFPLL, 1, 0, 0x26 }, L2(7),  1125000 },
	{ 0, {  1080000, HFPLL, 1, 0, 0x28 }, L2(15), 1175000 },
	{ 1, {  1134000, HFPLL, 1, 0, 0x2A }, L2(15), 1175000 },
	{ 0, {  1188000, HFPLL, 1, 0, 0x2C }, L2(15), 1200000 },
	{ 1, {  1242000, HFPLL, 1, 0, 0x2E }, L2(15), 1200000 },
	{ 0, {  1296000, HFPLL, 1, 0, 0x30 }, L2(15), 1225000 },
	{ 1, {  1350000, HFPLL, 1, 0, 0x32 }, L2(15), 1225000 },
	{ 0, {  1404000, HFPLL, 1, 0, 0x34 }, L2(15), 1237500 },
	{ 1, {  1458000, HFPLL, 1, 0, 0x36 }, L2(15), 1237500 },
	{ 1, {  1512000, HFPLL, 1, 0, 0x38 }, L2(15), 1250000 },
	{ 0, { 0 } }
};

static struct acpu_level acpu_freq_tbl_8064_nom[] = {
	{ 0, { STBY_KHZ, QSB,   0, 0, 0x00 }, L2(0),   900000 },
	{ 1, {   384000, PLL_8, 0, 2, 0x00 }, L2(1),   900000 },
	{ 0, {   432000, HFPLL, 2, 0, 0x20 }, L2(7),   925000 },
	{ 1, {   486000, HFPLL, 2, 0, 0x24 }, L2(7),   925000 },
	{ 0, {   540000, HFPLL, 2, 0, 0x28 }, L2(7),   950000 },
	{ 1, {   594000, HFPLL, 1, 0, 0x16 }, L2(7),   950000 },
	{ 0, {   648000, HFPLL, 1, 0, 0x18 }, L2(7),   975000 },
	{ 1, {   702000, HFPLL, 1, 0, 0x1A }, L2(7),   975000 },
	{ 0, {   756000, HFPLL, 1, 0, 0x1C }, L2(7),  1025000 },
	{ 1, {   810000, HFPLL, 1, 0, 0x1E }, L2(7),  1025000 },
	{ 0, {   864000, HFPLL, 1, 0, 0x20 }, L2(7),  1050000 },
	{ 1, {   918000, HFPLL, 1, 0, 0x22 }, L2(7),  1050000 },
	{ 0, {   972000, HFPLL, 1, 0, 0x24 }, L2(7),  1075000 },
	{ 1, {  1026000, HFPLL, 1, 0, 0x26 }, L2(7),  1075000 },
	{ 0, {  1080000, HFPLL, 1, 0, 0x28 }, L2(15), 1125000 },
	{ 1, {  1134000, HFPLL, 1, 0, 0x2A }, L2(15), 1125000 },
	{ 0, {  1188000, HFPLL, 1, 0, 0x2C }, L2(15), 1150000 },
	{ 1, {  1242000, HFPLL, 1, 0, 0x2E }, L2(15), 1150000 },
	{ 0, {  1296000, HFPLL, 1, 0, 0x30 }, L2(15), 1175000 },
	{ 1, {  1350000, HFPLL, 1, 0, 0x32 }, L2(15), 1175000 },
	{ 0, {  1404000, HFPLL, 1, 0, 0x34 }, L2(15), 1187500 },
	{ 1, {  1458000, HFPLL, 1, 0, 0x36 }, L2(15), 1187500 },
	{ 1, {  1512000, HFPLL, 1, 0, 0x38 }, L2(15), 1200000 },
	{ 0, { 0 } }
};

static struct acpu_level acpu_freq_tbl_8064_fast[] = {
	{ 0, { STBY_KHZ, QSB,   0, 0, 0x00 }, L2(0),   850000 },
	{ 1, {   384000, PLL_8, 0, 2, 0x00 }, L2(1),   850000 },
	{ 0, {   432000, HFPLL, 2, 0, 0x20 }, L2(7),   875000 },
	{ 1, {   486000, HFPLL, 2, 0, 0x24 }, L2(7),   875000 },
	{ 0, {   540000, HFPLL, 2, 0, 0x28 }, L2(7),   900000 },
	{ 1, {   594000, HFPLL, 1, 0, 0x16 }, L2(7),   900000 },
	{ 0, {   648000, HFPLL, 1, 0, 0x18 }, L2(7),   925000 },
	{ 1, {   702000, HFPLL, 1, 0, 0x1A }, L2(7),   925000 },
	{ 0, {   756000, HFPLL, 1, 0, 0x1C }, L2(7),   975000 },
	{ 1, {   810000, HFPLL, 1, 0, 0x1E }, L2(7),   975000 },
	{ 0, {   864000, HFPLL, 1, 0, 0x20 }, L2(7),  1000000 },
	{ 1, {   918000, HFPLL, 1, 0, 0x22 }, L2(7),  1000000 },
	{ 0, {   972000, HFPLL, 1, 0, 0x24 }, L2(7),  1025000 },
	{ 1, {  1026000, HFPLL, 1, 0, 0x26 }, L2(7),  1025000 },
	{ 0, {  1080000, HFPLL, 1, 0, 0x28 }, L2(15), 1075000 },
	{ 1, {  1134000, HFPLL, 1, 0, 0x2A }, L2(15), 1075000 },
	{ 0, {  1188000, HFPLL, 1, 0, 0x2C }, L2(15), 1100000 },
	{ 1, {  1242000, HFPLL, 1, 0, 0x2E }, L2(15), 1100000 },
	{ 0, {  1296000, HFPLL, 1, 0, 0x30 }, L2(15), 1125000 },
	{ 1, {  1350000, HFPLL, 1, 0, 0x32 }, L2(15), 1125000 },
	{ 0, {  1404000, HFPLL, 1, 0, 0x34 }, L2(15), 1137500 },
	{ 1, {  1458000, HFPLL, 1, 0, 0x36 }, L2(15), 1137500 },
	{ 1, {  1512000, HFPLL, 1, 0, 0x38 }, L2(15), 1150000 },
	{ 0, { 0 } }
};

#undef L2
#define L2(x) (&l2_freq_tbl_8930[(x)])
static struct l2_level l2_freq_tbl_8930[] = {
	[0]  = { {STBY_KHZ, QSB,   0, 0, 0x00 },  LVL_NOM, 1050000, 0 },
	[1]  = { {  384000, PLL_8, 0, 2, 0x00 },  LVL_NOM, 1050000, 1 },
	[2]  = { {  432000, HFPLL, 2, 0, 0x20 },  LVL_NOM, 1050000, 2 },
	[3]  = { {  486000, HFPLL, 2, 0, 0x24 },  LVL_NOM, 1050000, 2 },
	[4]  = { {  540000, HFPLL, 2, 0, 0x28 },  LVL_NOM, 1050000, 2 },
	[5]  = { {  594000, HFPLL, 1, 0, 0x16 },  LVL_NOM, 1050000, 2 },
	[6]  = { {  648000, HFPLL, 1, 0, 0x18 },  LVL_NOM, 1050000, 4 },
	[7]  = { {  702000, HFPLL, 1, 0, 0x1A },  LVL_NOM, 1050000, 4 },
	[8]  = { {  756000, HFPLL, 1, 0, 0x1C }, LVL_HIGH, 1150000, 4 },
	[9]  = { {  810000, HFPLL, 1, 0, 0x1E }, LVL_HIGH, 1150000, 4 },
	[10] = { {  864000, HFPLL, 1, 0, 0x20 }, LVL_HIGH, 1150000, 4 },
	[11] = { {  918000, HFPLL, 1, 0, 0x22 }, LVL_HIGH, 1150000, 7 },
	[12] = { {  972000, HFPLL, 1, 0, 0x24 }, LVL_HIGH, 1150000, 7 },
	[13] = { { 1026000, HFPLL, 1, 0, 0x26 }, LVL_HIGH, 1150000, 7 },
	[14] = { { 1080000, HFPLL, 1, 0, 0x28 }, LVL_HIGH, 1150000, 7 },
	[15] = { { 1134000, HFPLL, 1, 0, 0x2A }, LVL_HIGH, 1150000, 7 },
	[16] = { { 1188000, HFPLL, 1, 0, 0x2C }, LVL_HIGH, 1150000, 7 },
};

static struct acpu_level acpu_freq_tbl_8930_slow[] = {
	{ 0, { STBY_KHZ, QSB,   0, 0, 0x00 }, L2(0),   950000 },
	{ 1, {   384000, PLL_8, 0, 2, 0x00 }, L2(1),   950000 },
	{ 1, {   432000, HFPLL, 2, 0, 0x20 }, L2(6),   975000 },
	{ 1, {   486000, HFPLL, 2, 0, 0x24 }, L2(6),   975000 },
	{ 1, {   540000, HFPLL, 2, 0, 0x28 }, L2(6),  1000000 },
	{ 1, {   594000, HFPLL, 1, 0, 0x16 }, L2(6),  1000000 },
	{ 1, {   648000, HFPLL, 1, 0, 0x18 }, L2(6),  1025000 },
	{ 1, {   702000, HFPLL, 1, 0, 0x1A }, L2(6),  1025000 },
	{ 1, {   756000, HFPLL, 1, 0, 0x1C }, L2(11), 1075000 },
	{ 1, {   810000, HFPLL, 1, 0, 0x1E }, L2(11), 1075000 },
	{ 1, {   864000, HFPLL, 1, 0, 0x20 }, L2(11), 1100000 },
	{ 1, {   918000, HFPLL, 1, 0, 0x22 }, L2(11), 1100000 },
	{ 1, {   972000, HFPLL, 1, 0, 0x24 }, L2(11), 1125000 },
	{ 1, {  1026000, HFPLL, 1, 0, 0x26 }, L2(11), 1125000 },
	{ 1, {  1080000, HFPLL, 1, 0, 0x28 }, L2(16), 1175000 },
	{ 1, {  1134000, HFPLL, 1, 0, 0x2A }, L2(16), 1175000 },
	{ 1, {  1188000, HFPLL, 1, 0, 0x2C }, L2(16), 1200000 },
	{ 0, { 0 } }
};

static struct acpu_level acpu_freq_tbl_8930_nom[] = {
	{ 0, { STBY_KHZ, QSB,   0, 0, 0x00 }, L2(0),   925000 },
	{ 1, {   384000, PLL_8, 0, 2, 0x00 }, L2(1),   925000 },
	{ 1, {   432000, HFPLL, 2, 0, 0x20 }, L2(6),   950000 },
	{ 1, {   486000, HFPLL, 2, 0, 0x24 }, L2(6),   950000 },
	{ 1, {   540000, HFPLL, 2, 0, 0x28 }, L2(6),   975000 },
	{ 1, {   594000, HFPLL, 1, 0, 0x16 }, L2(6),   975000 },
	{ 1, {   648000, HFPLL, 1, 0, 0x18 }, L2(6),  1000000 },
	{ 1, {   702000, HFPLL, 1, 0, 0x1A }, L2(6),  1000000 },
	{ 1, {   756000, HFPLL, 1, 0, 0x1C }, L2(11), 1050000 },
	{ 1, {   810000, HFPLL, 1, 0, 0x1E }, L2(11), 1050000 },
	{ 1, {   864000, HFPLL, 1, 0, 0x20 }, L2(11), 1075000 },
	{ 1, {   918000, HFPLL, 1, 0, 0x22 }, L2(11), 1075000 },
	{ 1, {   972000, HFPLL, 1, 0, 0x24 }, L2(11), 1100000 },
	{ 1, {  1026000, HFPLL, 1, 0, 0x26 }, L2(11), 1100000 },
	{ 1, {  1080000, HFPLL, 1, 0, 0x28 }, L2(16), 1150000 },
	{ 1, {  1134000, HFPLL, 1, 0, 0x2A }, L2(16), 1150000 },
	{ 1, {  1188000, HFPLL, 1, 0, 0x2C }, L2(16), 1175000 },
	{ 0, { 0 } }
};

static struct acpu_level acpu_freq_tbl_8930_fast[] = {
	{ 0, { STBY_KHZ, QSB,   0, 0, 0x00 }, L2(0),   900000 },
	{ 1, {   384000, PLL_8, 0, 2, 0x00 }, L2(1),   900000 },
	{ 1, {   432000, HFPLL, 2, 0, 0x20 }, L2(6),   900000 },
	{ 1, {   486000, HFPLL, 2, 0, 0x24 }, L2(6),   900000 },
	{ 1, {   540000, HFPLL, 2, 0, 0x28 }, L2(6),   925000 },
	{ 1, {   594000, HFPLL, 1, 0, 0x16 }, L2(6),   925000 },
	{ 1, {   648000, HFPLL, 1, 0, 0x18 }, L2(6),   950000 },
	{ 1, {   702000, HFPLL, 1, 0, 0x1A }, L2(6),   950000 },
	{ 1, {   756000, HFPLL, 1, 0, 0x1C }, L2(11), 1000000 },
	{ 1, {   810000, HFPLL, 1, 0, 0x1E }, L2(11), 1000000 },
	{ 1, {   864000, HFPLL, 1, 0, 0x20 }, L2(11), 1025000 },
	{ 1, {   918000, HFPLL, 1, 0, 0x22 }, L2(11), 1025000 },
	{ 1, {   972000, HFPLL, 1, 0, 0x24 }, L2(11), 1050000 },
	{ 1, {  1026000, HFPLL, 1, 0, 0x26 }, L2(11), 1050000 },
	{ 1, {  1080000, HFPLL, 1, 0, 0x28 }, L2(16), 1100000 },
	{ 1, {  1134000, HFPLL, 1, 0, 0x2A }, L2(16), 1100000 },
	{ 1, {  1188000, HFPLL, 1, 0, 0x2C }, L2(16), 1125000 },
	{ 0, { 0 } }
};
#undef L2
#define L2(x) (&l2_freq_tbl_8627[(x)])
static struct l2_level l2_freq_tbl_8627[] = {
	[0]  = { {STBY_KHZ, QSB,   0, 0, 0x00 },  LVL_NOM, 1050000, 0 },
	[1]  = { {  384000, PLL_8, 0, 2, 0x00 },  LVL_NOM, 1050000, 1 },
	[2]  = { {  432000, HFPLL, 2, 0, 0x20 },  LVL_NOM, 1050000, 1 },
	[3]  = { {  486000, HFPLL, 2, 0, 0x24 },  LVL_NOM, 1050000, 1 },
	[4]  = { {  540000, HFPLL, 2, 0, 0x28 },  LVL_NOM, 1050000, 2 },
	[5]  = { {  594000, HFPLL, 1, 0, 0x16 },  LVL_NOM, 1050000, 2 },
	[6]  = { {  648000, HFPLL, 1, 0, 0x18 },  LVL_NOM, 1050000, 2 },
	[7]  = { {  702000, HFPLL, 1, 0, 0x1A },  LVL_NOM, 1050000, 3 },
	[8]  = { {  756000, HFPLL, 1, 0, 0x1C }, LVL_HIGH, 1150000, 3 },
	[9]  = { {  810000, HFPLL, 1, 0, 0x1E }, LVL_HIGH, 1150000, 3 },
	[10] = { {  864000, HFPLL, 1, 0, 0x20 }, LVL_HIGH, 1150000, 4 },
	[11] = { {  918000, HFPLL, 1, 0, 0x22 }, LVL_HIGH, 1150000, 4 },
	[12] = { {  972000, HFPLL, 1, 0, 0x24 }, LVL_HIGH, 1150000, 4 },
};

static struct acpu_level acpu_freq_tbl_8627[] = {
	{ 0, { STBY_KHZ, QSB,   0, 0, 0x00 }, L2(0),   900000 },
	{ 1, {   384000, PLL_8, 0, 2, 0x00 }, L2(1),   900000 },
	{ 1, {   432000, HFPLL, 2, 0, 0x20 }, L2(5),   925000 },
	{ 1, {   486000, HFPLL, 2, 0, 0x24 }, L2(5),   925000 },
	{ 1, {   540000, HFPLL, 2, 0, 0x28 }, L2(5),   937500 },
	{ 1, {   594000, HFPLL, 1, 0, 0x16 }, L2(5),   962500 },
	{ 1, {   648000, HFPLL, 1, 0, 0x18 }, L2(9),   987500 },
	{ 1, {   702000, HFPLL, 1, 0, 0x1A }, L2(9),  1000000 },
	{ 1, {   756000, HFPLL, 1, 0, 0x1C }, L2(9),  1025000 },
	{ 1, {   810000, HFPLL, 1, 0, 0x1E }, L2(9),  1062500 },
	{ 1, {   864000, HFPLL, 1, 0, 0x20 }, L2(12), 1062500 },
	{ 1, {   918000, HFPLL, 1, 0, 0x22 }, L2(12), 1087500 },
	{ 1, {   972000, HFPLL, 1, 0, 0x24 }, L2(12), 1100000 },
	{ 0, { 0 } }
};

static struct acpu_level *acpu_freq_tbl_8960_v1[NUM_PVS] __initdata = {
	[PVS_SLOW] = acpu_freq_tbl_8960_kraitv1_slow,
	[PVS_NOM] = acpu_freq_tbl_8960_kraitv1_nom_fast,
	[PVS_FAST] = acpu_freq_tbl_8960_kraitv1_nom_fast,
};

static struct acpu_level *acpu_freq_tbl_8960_v2[NUM_PVS] __initdata = {
	[PVS_SLOW] = acpu_freq_tbl_8960_kraitv2_slow,
	[PVS_NOM] = acpu_freq_tbl_8960_kraitv2_nom,
	[PVS_FAST] = acpu_freq_tbl_8960_kraitv2_fast,
};

static struct acpu_level *acpu_freq_tbl_8064[NUM_PVS] __initdata = {
	[PVS_SLOW] = acpu_freq_tbl_8064_slow,
	[PVS_NOM] = acpu_freq_tbl_8064_nom,
	[PVS_FAST] = acpu_freq_tbl_8064_fast,
	[PVS_FASTER] = acpu_freq_tbl_8064_fast,
};

static struct acpu_level *acpu_freq_tbl_8930_pvs[NUM_PVS] __initdata = {
	[PVS_SLOW] = acpu_freq_tbl_8930_slow,
	[PVS_NOM] = acpu_freq_tbl_8930_nom,
	[PVS_FAST] = acpu_freq_tbl_8930_fast,
};

static struct acpu_level *max_acpu_level;

static unsigned long acpuclk_8960_get_rate(int cpu)
{
	return scalable[cpu].current_speed->khz;
}

static int get_pri_clk_src(struct scalable *sc)
{
	uint32_t regval;

	regval = get_l2_indirect_reg(sc->l2cpmr_iaddr);
	return regval & 0x3;
}

static void set_pri_clk_src(struct scalable *sc, uint32_t pri_src_sel)
{
	uint32_t regval;

	regval = get_l2_indirect_reg(sc->l2cpmr_iaddr);
	regval &= ~0x3;
	regval |= (pri_src_sel & 0x3);
	set_l2_indirect_reg(sc->l2cpmr_iaddr, regval);
	
	mb();
	udelay(1);
}

static int get_sec_clk_src(struct scalable *sc)
{
	uint32_t regval;

	regval = get_l2_indirect_reg(sc->l2cpmr_iaddr);
	return (regval >> 2) & 0x3;
}

static void set_sec_clk_src(struct scalable *sc, uint32_t sec_src_sel)
{
	uint32_t regval;

	
	regval = get_l2_indirect_reg(sc->l2cpmr_iaddr);
	regval |= SECCLKAGD;
	set_l2_indirect_reg(sc->l2cpmr_iaddr, regval);

	
	regval &= ~(0x3 << 2);
	regval |= ((sec_src_sel & 0x3) << 2);
	set_l2_indirect_reg(sc->l2cpmr_iaddr, regval);

	
	mb();
	udelay(1);

	
	regval &= ~SECCLKAGD;
	set_l2_indirect_reg(sc->l2cpmr_iaddr, regval);
}

static void hfpll_enable(struct scalable *sc, bool skip_regulators)
{
	int rc;

	if (!skip_regulators) {
		if (cpu_is_msm8960()) {
			rc = rpm_vreg_set_voltage(
					sc->vreg[VREG_HFPLL_A].rpm_vreg_id,
					sc->vreg[VREG_HFPLL_A].rpm_vreg_voter,
					2050000,
					sc->vreg[VREG_HFPLL_A].max_vdd, 0);
			if (rc)
				pr_err("%s regulator enable failed (%d)\n",
					sc->vreg[VREG_HFPLL_A].name, rc);
		}
		rc = rpm_vreg_set_voltage(sc->vreg[VREG_HFPLL_B].rpm_vreg_id,
				sc->vreg[VREG_HFPLL_B].rpm_vreg_voter, 1800000,
				sc->vreg[VREG_HFPLL_B].max_vdd, 0);
		if (rc)
			pr_err("%s regulator enable failed (%d)\n",
				sc->vreg[VREG_HFPLL_B].name, rc);
	}
	
	writel_relaxed(0x2, sc->hfpll_base + HFPLL_MODE);

	mb();
	udelay(10);

	
	writel_relaxed(0x6, sc->hfpll_base + HFPLL_MODE);

	
	mb();
	udelay(60);

	
	writel_relaxed(0x7, sc->hfpll_base + HFPLL_MODE);
}

static void hfpll_disable(struct scalable *sc, bool skip_regulators)
{
	int rc;

	writel_relaxed(0, sc->hfpll_base + HFPLL_MODE);

	if (!skip_regulators) {
		rc = rpm_vreg_set_voltage(sc->vreg[VREG_HFPLL_B].rpm_vreg_id,
				sc->vreg[VREG_HFPLL_B].rpm_vreg_voter, 0,
				0, 0);
		if (rc)
			pr_err("%s regulator enable failed (%d)\n",
				sc->vreg[VREG_HFPLL_B].name, rc);

		if (cpu_is_msm8960()) {
			rc = rpm_vreg_set_voltage(
					sc->vreg[VREG_HFPLL_A].rpm_vreg_id,
					sc->vreg[VREG_HFPLL_A].rpm_vreg_voter,
					0, 0, 0);
			if (rc)
				pr_err("%s regulator enable failed (%d)\n",
					sc->vreg[VREG_HFPLL_A].name, rc);
		}
	}
}

static void hfpll_set_rate(struct scalable *sc, struct core_speed *tgt_s)
{
	writel_relaxed(tgt_s->pll_l_val, sc->hfpll_base + HFPLL_L_VAL);
}

static struct l2_level *compute_l2_level(struct scalable *sc,
					 struct l2_level *vote_l)
{
	struct l2_level *new_l;
	int cpu;

	
	BUG_ON(vote_l >= (l2_freq_tbl + l2_freq_tbl_size));

	
	sc->l2_vote = vote_l;
	new_l = l2_freq_tbl;
	for_each_present_cpu(cpu)
		new_l = max(new_l, scalable[cpu].l2_vote);

	return new_l;
}

static void set_bus_bw(unsigned int bw)
{
	int ret;

	
	if (bw >= ARRAY_SIZE(bw_level_tbl)) {
		pr_err("invalid bandwidth request (%d)\n", bw);
		return;
	}

	
	ret = msm_bus_scale_client_update_request(bus_perf_client, bw);
	if (ret)
		pr_err("bandwidth request failed (%d)\n", ret);
}

static void set_speed(struct scalable *sc, struct core_speed *tgt_s,
		      enum setrate_reason reason)
{
	struct core_speed *strt_s = sc->current_speed;

	if (tgt_s == strt_s)
		return;

	if (strt_s->src == HFPLL && tgt_s->src == HFPLL) {
		set_sec_clk_src(sc, SEC_SRC_SEL_AUX);
		set_pri_clk_src(sc, PRI_SRC_SEL_SEC_SRC);

		
		hfpll_disable(sc, 1);
		hfpll_set_rate(sc, tgt_s);
		hfpll_enable(sc, 1);

		
		set_pri_clk_src(sc, tgt_s->pri_src_sel);
	} else if (strt_s->src == HFPLL && tgt_s->src != HFPLL) {
		if (reason != SETRATE_HOTPLUG || sc == &scalable[L2]) {
			set_sec_clk_src(sc, tgt_s->sec_src_sel);
			set_pri_clk_src(sc, tgt_s->pri_src_sel);
			hfpll_disable(sc, 0);
		} else if (reason == SETRATE_HOTPLUG
			   && msm_pm_verify_cpu_pc(SCALABLE_TO_CPU(sc))) {
			hfpll_disable(sc, 0);
		}
	} else if (strt_s->src != HFPLL && tgt_s->src == HFPLL) {
		if (reason != SETRATE_HOTPLUG || sc == &scalable[L2]) {
			hfpll_set_rate(sc, tgt_s);
			hfpll_enable(sc, 0);
			set_pri_clk_src(sc, tgt_s->pri_src_sel);
		} else if (reason == SETRATE_HOTPLUG
			   && msm_pm_verify_cpu_pc(SCALABLE_TO_CPU(sc))) {
			
			hfpll_set_rate(sc, tgt_s);
			hfpll_enable(sc, 0);
		}
	} else {
		if (reason != SETRATE_HOTPLUG || sc == &scalable[L2])
			set_sec_clk_src(sc, tgt_s->sec_src_sel);
	}

	sc->current_speed = tgt_s;
}

static int increase_vdd(int cpu, unsigned int vdd_core, unsigned int vdd_mem,
			unsigned int vdd_dig, enum setrate_reason reason)
{
	struct scalable *sc = &scalable[cpu];
	int rc = 0;

	if (vdd_mem > sc->vreg[VREG_MEM].cur_vdd) {
		rc = rpm_vreg_set_voltage(sc->vreg[VREG_MEM].rpm_vreg_id,
				sc->vreg[VREG_MEM].rpm_vreg_voter, vdd_mem,
				sc->vreg[VREG_MEM].max_vdd, 0);
		if (rc) {
			pr_err("%s increase failed (%d)\n",
				sc->vreg[VREG_MEM].name, rc);
			return rc;
		}
		 sc->vreg[VREG_MEM].cur_vdd = vdd_mem;
	}

	
	if (vdd_dig > sc->vreg[VREG_DIG].cur_vdd) {
		rc = rpm_vreg_set_voltage(sc->vreg[VREG_DIG].rpm_vreg_id,
				sc->vreg[VREG_DIG].rpm_vreg_voter, vdd_dig,
				sc->vreg[VREG_DIG].max_vdd, 0);
		if (rc) {
			pr_err("%s increase failed (%d)\n",
				sc->vreg[VREG_DIG].name, rc);
			return rc;
		}
		sc->vreg[VREG_DIG].cur_vdd = vdd_dig;
	}

	if (vdd_core > sc->vreg[VREG_CORE].cur_vdd
						&& reason != SETRATE_HOTPLUG) {
		rc = regulator_set_voltage(sc->vreg[VREG_CORE].reg, vdd_core,
					   sc->vreg[VREG_CORE].max_vdd);
		if (rc) {
			pr_err("%s increase failed (%d)\n",
				sc->vreg[VREG_CORE].name, rc);
			return rc;
		}
		sc->vreg[VREG_CORE].cur_vdd = vdd_core;
	}

	return rc;
}

static void decrease_vdd(int cpu, unsigned int vdd_core, unsigned int vdd_mem,
			 unsigned int vdd_dig, enum setrate_reason reason)
{
	struct scalable *sc = &scalable[cpu];
	int ret;

	if (vdd_core < sc->vreg[VREG_CORE].cur_vdd
					&& reason != SETRATE_HOTPLUG) {
		ret = regulator_set_voltage(sc->vreg[VREG_CORE].reg, vdd_core,
					    sc->vreg[VREG_CORE].max_vdd);
		if (ret) {
			pr_err("%s decrease failed (%d)\n",
			       sc->vreg[VREG_CORE].name, ret);
			return;
		}
		sc->vreg[VREG_CORE].cur_vdd = vdd_core;
	}

	
	if (vdd_dig < sc->vreg[VREG_DIG].cur_vdd) {
		ret = rpm_vreg_set_voltage(sc->vreg[VREG_DIG].rpm_vreg_id,
				sc->vreg[VREG_DIG].rpm_vreg_voter, vdd_dig,
				sc->vreg[VREG_DIG].max_vdd, 0);
		if (ret) {
			pr_err("%s decrease failed (%d)\n",
				sc->vreg[VREG_DIG].name, ret);
			return;
		}
		sc->vreg[VREG_DIG].cur_vdd = vdd_dig;
	}

	if (vdd_mem < sc->vreg[VREG_MEM].cur_vdd) {
		ret = rpm_vreg_set_voltage(sc->vreg[VREG_MEM].rpm_vreg_id,
				sc->vreg[VREG_MEM].rpm_vreg_voter, vdd_mem,
				sc->vreg[VREG_MEM].max_vdd, 0);
		if (ret) {
			pr_err("%s decrease failed (%d)\n",
				sc->vreg[VREG_MEM].name, ret);
			return;
		}
		 sc->vreg[VREG_MEM].cur_vdd = vdd_mem;
	}
}

static unsigned int calculate_vdd_mem(struct acpu_level *tgt)
{
	return tgt->l2_level->vdd_mem;
}

static unsigned int calculate_vdd_dig(struct acpu_level *tgt)
{
	unsigned int pll_vdd_dig;

	if (tgt->l2_level->speed.src != HFPLL)
		pll_vdd_dig = scalable[L2].hfpll_vdd_tbl[HFPLL_VDD_NONE];
	else if (tgt->l2_level->speed.pll_l_val > HFPLL_LOW_VDD_PLL_L_MAX)
		pll_vdd_dig = scalable[L2].hfpll_vdd_tbl[HFPLL_VDD_NOM];
	else
		pll_vdd_dig = scalable[L2].hfpll_vdd_tbl[HFPLL_VDD_LOW];

	return max(tgt->l2_level->vdd_dig, pll_vdd_dig);
}

static unsigned int calculate_vdd_core(struct acpu_level *tgt)
{
	return tgt->vdd_core;
}

static int acpuclk_8960_set_rate(int cpu, unsigned long rate,
				 enum setrate_reason reason)
{
	struct core_speed *strt_acpu_s, *tgt_acpu_s;
	struct l2_level *tgt_l2_l;
	struct acpu_level *tgt;
	unsigned int vdd_mem, vdd_dig, vdd_core;
	unsigned long flags;
	int rc = 0;

	if (reason == SETRATE_CPUFREQ) {
		if (cpu_is_offline(cpu)) {
			pr_info("don't change frequency for an offline CPU%d\n", cpu);
			return 0;
		} else if (smp_processor_id() != cpu) {
			pr_info("CPU%d don't change frequency for CPU%d\n", smp_processor_id(), cpu);
			return 0;
		}
	}

	set_acpuclk_foot_print(cpu, 0x1);

	if (cpu > num_possible_cpus())
		return -EINVAL;

	if (reason == SETRATE_CPUFREQ || reason == SETRATE_HOTPLUG)
		mutex_lock(&driver_lock);

	set_acpuclk_foot_print(cpu, 0x2);

	strt_acpu_s = scalable[cpu].current_speed;

	
	if (rate == strt_acpu_s->khz)
		goto out;

	
	for (tgt = acpu_freq_tbl; tgt->speed.khz != 0; tgt++) {
		if (tgt->speed.khz == rate) {
			tgt_acpu_s = &tgt->speed;
			break;
		}
	}
	if (tgt->speed.khz == 0) {
		rc = -EINVAL;
		goto out;
	}

	
	vdd_mem  = calculate_vdd_mem(tgt);
	vdd_dig  = calculate_vdd_dig(tgt);
	vdd_core = calculate_vdd_core(tgt);

	
	if (reason == SETRATE_CPUFREQ || reason == SETRATE_HOTPLUG) {
		rc = increase_vdd(cpu, vdd_core, vdd_mem, vdd_dig, reason);
		if (rc)
			goto out;
	}

	pr_debug("Switching from ACPU%d rate %u KHz -> %u KHz\n",
		cpu, strt_acpu_s->khz, tgt_acpu_s->khz);
	udelay(60);
	set_acpuclk_foot_print(cpu, 0x3);

	if (reason == SETRATE_CPUFREQ) {
		if (cpu_is_offline(cpu)) {
			pr_info("don't set_speed for an offline CPU%d\n", cpu);
			goto out;
		} else if (smp_processor_id() != cpu) {
			pr_info("CPU%d don't set_speed for CPU%d\n", smp_processor_id(), cpu);
			goto out;
		}
	}

	
	set_speed(&scalable[cpu], tgt_acpu_s, reason);

	set_acpuclk_cpu_freq_foot_print(cpu, tgt_acpu_s->khz);
	set_acpuclk_foot_print(cpu, 0x4);

	spin_lock_irqsave(&l2_lock, flags);
	tgt_l2_l = compute_l2_level(&scalable[cpu], tgt->l2_level);
	set_speed(&scalable[L2], &tgt_l2_l->speed, reason);

	set_acpuclk_L2_freq_foot_print(tgt_l2_l->speed.khz);
	set_acpuclk_foot_print(cpu, 0x5);

	spin_unlock_irqrestore(&l2_lock, flags);

	
	if (reason == SETRATE_PC || reason == SETRATE_SWFI)
		goto out;

	
	set_bus_bw(tgt_l2_l->bw_level);

	set_acpuclk_foot_print(cpu, 0x6);

	
	decrease_vdd(cpu, vdd_core, vdd_mem, vdd_dig, reason);

	set_acpuclk_foot_print(cpu, 0x7);

	pr_debug("ACPU%d speed change complete\n", cpu);

out:
	if (reason == SETRATE_CPUFREQ || reason == SETRATE_HOTPLUG)
		mutex_unlock(&driver_lock);

	set_acpuclk_foot_print(cpu, 0x8);

	return rc;
}

static void __cpuinit hfpll_init(struct scalable *sc, struct core_speed *tgt_s)
{
	pr_debug("Initializing HFPLL%d\n", sc - scalable);

	
	hfpll_disable(sc, 1);

	
	writel_relaxed(0x7845C665, sc->hfpll_base + HFPLL_CONFIG_CTL);
	writel_relaxed(0, sc->hfpll_base + HFPLL_M_VAL);
	writel_relaxed(1, sc->hfpll_base + HFPLL_N_VAL);

	
	writel_relaxed(0x0108C000, sc->hfpll_base + HFPLL_DROOP_CTL);

	
	hfpll_set_rate(sc, tgt_s);
	hfpll_enable(sc, 0);
}

static void __cpuinit regulator_init(int cpu, struct acpu_level *lvl)
{
	int ret;
	struct scalable *sc = &scalable[cpu];
	unsigned int vdd_mem, vdd_dig, vdd_core;

	vdd_mem = calculate_vdd_mem(lvl);
	vdd_dig = calculate_vdd_dig(lvl);

	
	ret = rpm_vreg_set_voltage(sc->vreg[VREG_MEM].rpm_vreg_id,
			sc->vreg[VREG_MEM].rpm_vreg_voter, vdd_mem,
			sc->vreg[VREG_MEM].max_vdd, 0);
	if (ret) {
		pr_err("%s initialization failed (%d)\n",
			sc->vreg[VREG_MEM].name, ret);
		BUG();
	}
	sc->vreg[VREG_MEM].cur_vdd  = vdd_mem;

	
	ret = rpm_vreg_set_voltage(sc->vreg[VREG_DIG].rpm_vreg_id,
			sc->vreg[VREG_DIG].rpm_vreg_voter, vdd_dig,
			sc->vreg[VREG_DIG].max_vdd, 0);
	if (ret) {
		pr_err("%s initialization failed (%d)\n",
			sc->vreg[VREG_DIG].name, ret);
		BUG();
	}
	sc->vreg[VREG_DIG].cur_vdd  = vdd_dig;

	
	sc->vreg[VREG_CORE].reg = regulator_get(NULL,
				  sc->vreg[VREG_CORE].name);
	if (IS_ERR(sc->vreg[VREG_CORE].reg)) {
		pr_err("regulator_get(%s) failed (%ld)\n",
		       sc->vreg[VREG_CORE].name,
		       PTR_ERR(sc->vreg[VREG_CORE].reg));
		BUG();
	}
	vdd_core = calculate_vdd_core(lvl);
	ret = regulator_set_voltage(sc->vreg[VREG_CORE].reg, vdd_core,
				    sc->vreg[VREG_CORE].max_vdd);
	if (ret) {
		pr_err("%s initialization failed (%d)\n",
			sc->vreg[VREG_CORE].name, ret);
		BUG();
	}
	sc->vreg[VREG_CORE].cur_vdd = vdd_core;
	ret = regulator_enable(sc->vreg[VREG_CORE].reg);
	if (ret) {
		pr_err("regulator_enable(%s) failed (%d)\n",
		       sc->vreg[VREG_CORE].name, ret);
		BUG();
	}
	sc->regulators_initialized = true;
}

static void __cpuinit init_clock_sources(struct scalable *sc,
				      struct core_speed *tgt_s)
{
	uint32_t regval;

	
	writel_relaxed(0x3, sc->aux_clk_sel);

	
	set_sec_clk_src(sc, SEC_SRC_SEL_AUX);
	set_pri_clk_src(sc, PRI_SRC_SEL_SEC_SRC);
	hfpll_init(sc, tgt_s);

	
	regval = get_l2_indirect_reg(sc->l2cpmr_iaddr);
	regval &= ~(0x3 << 6);
	set_l2_indirect_reg(sc->l2cpmr_iaddr, regval);

	
	set_sec_clk_src(sc, tgt_s->sec_src_sel);
	set_pri_clk_src(sc, tgt_s->pri_src_sel);
	sc->current_speed = tgt_s;
}

static void __cpuinit per_cpu_init(void *data)
{
	int cpu = smp_processor_id();

	init_clock_sources(&scalable[cpu], &max_acpu_level->speed);
	scalable[cpu].l2_vote = max_acpu_level->l2_level;
	scalable[cpu].clocks_initialized = true;
}

static void __init bus_init(unsigned int init_bw)
{
	int ret;

	bus_perf_client = msm_bus_scale_register_client(&bus_client_pdata);
	if (!bus_perf_client) {
		pr_err("unable to register bus client\n");
		BUG();
	}

	ret = msm_bus_scale_client_update_request(bus_perf_client, init_bw);
	if (ret)
		pr_err("initial bandwidth request failed (%d)\n", ret);
}

#ifdef CONFIG_CPU_FREQ_MSM
static struct cpufreq_frequency_table freq_table[NR_CPUS][30];

static void __init cpufreq_table_init(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		int i, freq_cnt = 0;
		
		for (i = 0; acpu_freq_tbl[i].speed.khz != 0
				&& freq_cnt < ARRAY_SIZE(*freq_table); i++) {
			if (acpu_freq_tbl[i].use_for_scaling) {
				freq_table[cpu][freq_cnt].index = freq_cnt;
				freq_table[cpu][freq_cnt].frequency
					= acpu_freq_tbl[i].speed.khz;
				freq_cnt++;
			}
		}
		
		BUG_ON(acpu_freq_tbl[i].speed.khz != 0);

		freq_table[cpu][freq_cnt].index = freq_cnt;
		freq_table[cpu][freq_cnt].frequency = CPUFREQ_TABLE_END;

		pr_info("CPU%d: %d scaling frequencies supported.\n",
			cpu, freq_cnt);

		
		cpufreq_frequency_table_get_attr(freq_table[cpu], cpu);
	}
}
#else
static void __init cpufreq_table_init(void) {}
#endif

#define HOT_UNPLUG_KHZ STBY_KHZ
static int __cpuinit acpuclock_cpu_callback(struct notifier_block *nfb,
					    unsigned long action, void *hcpu)
{
	static int prev_khz[NR_CPUS];
	static int prev_pri_src[NR_CPUS];
	static int prev_sec_src[NR_CPUS];
	int cpu = (int)hcpu;

	switch (action) {
	case CPU_DYING:
	case CPU_DYING_FROZEN:
		if (cpu_is_krait_v1()) {
			prev_sec_src[cpu] = get_sec_clk_src(&scalable[cpu]);
			prev_pri_src[cpu] = get_pri_clk_src(&scalable[cpu]);
			set_sec_clk_src(&scalable[cpu], SEC_SRC_SEL_QSB);
			set_pri_clk_src(&scalable[cpu], PRI_SRC_SEL_SEC_SRC);
		}
		break;
	case CPU_DEAD:
	case CPU_DEAD_FROZEN:
		prev_khz[cpu] = acpuclk_8960_get_rate(cpu);
		
	case CPU_UP_CANCELED:
	case CPU_UP_CANCELED_FROZEN:
		if (scalable[cpu].clocks_initialized)
			acpuclk_8960_set_rate(cpu, HOT_UNPLUG_KHZ,
					      SETRATE_HOTPLUG);
		break;
	case CPU_UP_PREPARE:
	case CPU_UP_PREPARE_FROZEN:
		if (scalable[cpu].clocks_initialized)
			acpuclk_8960_set_rate(cpu, prev_khz[cpu],
					      SETRATE_HOTPLUG);
		if (!scalable[cpu].regulators_initialized)
			regulator_init(cpu, max_acpu_level);
		break;
	case CPU_STARTING:
	case CPU_STARTING_FROZEN:
		if (!scalable[cpu].clocks_initialized) {
			per_cpu_init(NULL);
		} else if (cpu_is_krait_v1()) {
			set_sec_clk_src(&scalable[cpu], prev_sec_src[cpu]);
			set_pri_clk_src(&scalable[cpu], prev_pri_src[cpu]);
		}
		break;
	default:
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block __cpuinitdata acpuclock_cpu_notifier = {
	.notifier_call = acpuclock_cpu_callback,
};

static const int krait_needs_vmin(void)
{
	unsigned int cpuid = read_cpuid_id();

	pr_info("%s: cpuid is 0x%x\n", __func__, cpuid);
	switch (cpuid) {
	case 0x511F04D0:
	case 0x511F04D1:
	case 0x510F06F0:
		return 1;
	default:
		return 0;
	};
}

static void kraitv2_apply_vmin(struct acpu_level *tbl)
{
	for (; tbl->speed.khz != 0; tbl++)
		if (tbl->vdd_core < 1150000)
			tbl->vdd_core = 1150000;
}

static enum pvs __init get_pvs(void)
{
	uint32_t pte_efuse, pvs;
	unsigned int kernel_flag = get_kernel_flag();

	
	if(kernel_flag & KERNEL_FLAG_PVS_SLOW_CPU){
		pr_info("ACPU PVS: Force SLOW by writeconfig\n");
		return PVS_SLOW;
	}
	else if (kernel_flag & KERNEL_FLAG_PVS_NOM_CPU){
		pr_info("ACPU PVS: Force NOMINAL by writeconfig\n");
		return PVS_NOM;
	}
	else if (kernel_flag & KERNEL_FLAG_PVS_FAST_CPU){
		pr_info("ACPU PVS: Force FAST by writeconfig\n");
		return PVS_FAST;
	}

	pte_efuse = readl_relaxed(QFPROM_PTE_EFUSE_ADDR);
	pvs = (pte_efuse >> 10) & 0x7;
	if (pvs == 0x7)
		pvs = (pte_efuse >> 13) & 0x7;

	switch (pvs) {
	case 0x0:
	case 0x7:
		pr_info("ACPU PVS: Slow\n");
		return PVS_SLOW;
	case 0x1:
		pr_info("ACPU PVS: Nominal\n");
		return PVS_NOM;
	case 0x3:
		pr_info("ACPU PVS: Fast\n");
		return PVS_FAST;
	case 0x4:
		if (cpu_is_apq8064()) {
			pr_info("ACPU PVS: Faster\n");
			return  PVS_FASTER;
		}
	default:
		pr_warn("ACPU PVS: Unknown. Defaulting to slow\n");
		return PVS_SLOW;
	}
}

static void __init select_freq_plan(void)
{
	struct acpu_level *l;

	
	if (cpu_is_msm8960()) {
		enum pvs pvs_id = get_pvs();

		scalable = scalable_8960;
		if (cpu_is_krait_v1()) {
			acpu_freq_tbl = acpu_freq_tbl_8960_v1[pvs_id];
			l2_freq_tbl = l2_freq_tbl_8960_kraitv1;
			l2_freq_tbl_size = ARRAY_SIZE(l2_freq_tbl_8960_kraitv1);
		} else {
			acpu_freq_tbl = acpu_freq_tbl_8960_v2[pvs_id];
			l2_freq_tbl = l2_freq_tbl_8960_kraitv2;
			l2_freq_tbl_size = ARRAY_SIZE(l2_freq_tbl_8960_kraitv2);
		}
	} else if (cpu_is_apq8064()) {
		enum pvs pvs_id = get_pvs();

		scalable = scalable_8064;
		acpu_freq_tbl = acpu_freq_tbl_8064[pvs_id];
		l2_freq_tbl = l2_freq_tbl_8064;
		l2_freq_tbl_size = ARRAY_SIZE(l2_freq_tbl_8064);
	} else if (cpu_is_msm8627()) {
		scalable = scalable_8627;
		acpu_freq_tbl = acpu_freq_tbl_8627;
		l2_freq_tbl = l2_freq_tbl_8627;
		l2_freq_tbl_size = ARRAY_SIZE(l2_freq_tbl_8627);
	} else if (cpu_is_msm8930() || cpu_is_msm8930aa()) {
		enum pvs pvs_id = get_pvs();

		scalable = scalable_8930;
		acpu_freq_tbl = acpu_freq_tbl_8930_pvs[pvs_id];
		l2_freq_tbl = l2_freq_tbl_8930;
		l2_freq_tbl_size = ARRAY_SIZE(l2_freq_tbl_8930);
	} else {
		BUG();
	}
	BUG_ON(!acpu_freq_tbl);
	if (krait_needs_vmin()) {
		pr_info("Applying min 1.15v fix for Krait Errata 26\n");
		kraitv2_apply_vmin(acpu_freq_tbl);
	} else {
		pr_info("No need to apply min 1.15v fix for Krait Errata 26\n");
	}

	
	for (l = acpu_freq_tbl; l->speed.khz != 0; l++)
#ifdef CONFIG_ARCH_APQ8064
		if (l->use_for_scaling && (l->speed.khz <= 918000))
#else
		if (l->use_for_scaling)
#endif
			max_acpu_level = l;
	BUG_ON(!max_acpu_level);
	pr_info("Max ACPU freq: %u KHz\n", max_acpu_level->speed.khz);
}

static struct acpuclk_data acpuclk_8960_data = {
	.set_rate = acpuclk_8960_set_rate,
	.get_rate = acpuclk_8960_get_rate,
	.power_collapse_khz = STBY_KHZ,
	.wait_for_irq_khz = STBY_KHZ,
};

static int __init acpuclk_8960_probe(struct platform_device *pdev)
{
	int cpu;

	select_freq_plan();
#ifdef CONFIG_ARCH_APQ8064
#else
	for_each_online_cpu(cpu)
		regulator_init(cpu, max_acpu_level);
#endif
	bus_init(max_acpu_level->l2_level->bw_level);

	init_clock_sources(&scalable[L2], &max_acpu_level->l2_level->speed);
	on_each_cpu(per_cpu_init, NULL, true);
#ifdef CONFIG_ARCH_APQ8064
	for_each_online_cpu(cpu)
		regulator_init(cpu, max_acpu_level);
#endif
	cpufreq_table_init();

	acpuclk_register(&acpuclk_8960_data);
	register_hotcpu_notifier(&acpuclock_cpu_notifier);

	return 0;
}

static struct platform_driver acpuclk_8960_driver = {
	.driver = {
		.name = "acpuclk-8960",
		.owner = THIS_MODULE,
	},
};

static int __init acpuclk_8960_init(void)
{
	return platform_driver_probe(&acpuclk_8960_driver, acpuclk_8960_probe);
}
device_initcall(acpuclk_8960_init);
