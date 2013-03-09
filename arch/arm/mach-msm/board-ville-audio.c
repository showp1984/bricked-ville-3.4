/* Copyright (c) 2011-2012, Code Aurora Forum. All rights reserved.
 *
 * HTC: ville machine driver which defines board-specific data
 * Copy from sound/soc/msm/msm8960.c
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

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/mfd/pm8xxx/pm8921.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <sound/core.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/pcm.h>
#include <sound/q6asm.h>
#include <mach/htc_acoustic_8960.h>
#include <sound/jack.h>
#include <asm/mach-types.h>
#include <mach/socinfo.h>
#include <linux/mfd/wcd9xxx/core.h>
#include "../../../sound/soc/codecs/wcd9310.h"
#include "../sound/soc/msm/msm-pcm-routing.h"
#include "board-ville.h"
#include <asm/setup.h>
#include "../sound/soc/msm/msm-pcm-routing.h"

#include <mach/board.h>

static atomic_t q6_effect_mode = ATOMIC_INIT(-1);

#define top_spk_pamp_gpio	(PM8921_GPIO_PM_TO_SYS(19))
#define bottom_spk_pamp_gpio	(PM8921_GPIO_PM_TO_SYS(18))
#define MSM8960_SPK_ON 1
#define MSM8960_SPK_OFF 0

#define msm8960_SLIM_0_RX_MAX_CHANNELS		2
#define msm8960_SLIM_0_TX_MAX_CHANNELS		4

#define SAMPLE_RATE_8KHZ 8000
#define SAMPLE_RATE_16KHZ 16000

#define BOTTOM_SPK_AMP_POS	0x1
#define BOTTOM_SPK_AMP_NEG	0x2
#define TOP_SPK_AMP_POS		0x4
#define TOP_SPK_AMP_NEG		0x8

#define GPIO_AUX_PCM_DOUT 63
#define GPIO_AUX_PCM_DIN 64
#define GPIO_AUX_PCM_SYNC 65
#define GPIO_AUX_PCM_CLK 66

#define TABLA_EXT_CLK_RATE 12288000

static int msm8960_spk_control;
static int msm8960_ext_bottom_spk_pamp;
static int msm8960_ext_top_spk_pamp;
static int msm8960_slim_0_rx_ch = 1;
static int msm8960_slim_0_tx_ch = 1;

static int msm8960_btsco_rate = SAMPLE_RATE_8KHZ;
static int msm8960_btsco_ch = 1;

static int msm8960_auxpcm_rate = SAMPLE_RATE_8KHZ;

static struct clk *codec_clk;
static int clk_users;

static struct snd_soc_jack hs_jack;
static struct snd_soc_jack button_jack;

static int msm8960_enable_codec_ext_clk(struct snd_soc_codec *codec, int enable,
					bool dapm);

static struct mutex cdc_mclk_mutex;

static void msm8960_ext_spk_power_amp_off(u32);

static void msm8960_enable_ext_spk_amp_gpio(u32 spk_amp_gpio)
{
	int ret = 0;

	struct pm_gpio param = {
		.direction      = PM_GPIO_DIR_OUT,
		.output_buffer  = PM_GPIO_OUT_BUF_CMOS,
		.output_value   = 1,
		.pull      = PM_GPIO_PULL_NO,
		.vin_sel	= PM_GPIO_VIN_S4,
		.out_strength   = PM_GPIO_STRENGTH_MED,
		.function       = PM_GPIO_FUNC_NORMAL,
	};

	if (spk_amp_gpio == bottom_spk_pamp_gpio) {

		ret = gpio_request(bottom_spk_pamp_gpio, "BOTTOM_SPK_AMP");
		if (ret) {
			pr_err("%s: Error requesting BOTTOM SPK AMP GPIO %u\n",
				__func__, bottom_spk_pamp_gpio);
			return;
		}
		ret = pm8xxx_gpio_config(bottom_spk_pamp_gpio, &param);
		if (ret)
			pr_err("%s: Failed to configure Bottom Spk Ampl"
				" gpio %u\n", __func__, bottom_spk_pamp_gpio);
		else {
			pr_info("%s: enable Bottom spkr amp gpio\n", __func__);
			gpio_direction_output(bottom_spk_pamp_gpio, 1);
		}

	} else if (spk_amp_gpio == top_spk_pamp_gpio) {

		ret = gpio_request(top_spk_pamp_gpio, "TOP_SPK_AMP");
		if (ret) {
			pr_err("%s: Error requesting GPIO %d\n", __func__,
				top_spk_pamp_gpio);
			return;
		}
		ret = pm8xxx_gpio_config(top_spk_pamp_gpio, &param);
		if (ret)
			pr_err("%s: Failed to configure Top Spk Ampl"
				" gpio %u\n", __func__, top_spk_pamp_gpio);
		else {
			pr_info("%s: enable Top spkr amp gpio\n", __func__);
			gpio_direction_output(top_spk_pamp_gpio, 1);
		}
	} else {
		pr_err("%s: ERROR : Invalid External Speaker Ampl GPIO."
			" gpio = %u\n", __func__, spk_amp_gpio);
		return;
	}
}

static void msm8960_ext_spk_power_amp_on(u32 spk)
{
	if (spk & (BOTTOM_SPK_AMP_POS | BOTTOM_SPK_AMP_NEG)) {

		if ((msm8960_ext_bottom_spk_pamp & BOTTOM_SPK_AMP_POS) &&
			(msm8960_ext_bottom_spk_pamp & BOTTOM_SPK_AMP_NEG)) {

			pr_debug("%s() External Bottom Speaker Ampl already "
				"turned on. spk = 0x%08x\n", __func__, spk);
			return;
		}

		msm8960_ext_bottom_spk_pamp |= spk;

		if ((msm8960_ext_bottom_spk_pamp & BOTTOM_SPK_AMP_POS) &&
			(msm8960_ext_bottom_spk_pamp & BOTTOM_SPK_AMP_NEG)) {

			msm8960_enable_ext_spk_amp_gpio(bottom_spk_pamp_gpio);
			pr_debug("%s: slepping 4 ms after turning on external "
				" Bottom Speaker Ampl\n", __func__);
			usleep_range(4000, 4000);
		}

	} else if  (spk & (TOP_SPK_AMP_POS | TOP_SPK_AMP_NEG)) {

		if ((msm8960_ext_top_spk_pamp & TOP_SPK_AMP_POS) &&
			(msm8960_ext_top_spk_pamp & TOP_SPK_AMP_NEG)) {

			pr_debug("%s() External Top Speaker Ampl already"
				"turned on. spk = 0x%08x\n", __func__, spk);
			return;
		}

		msm8960_ext_top_spk_pamp |= spk;

		if ((msm8960_ext_top_spk_pamp & TOP_SPK_AMP_POS) &&
			(msm8960_ext_top_spk_pamp & TOP_SPK_AMP_NEG)) {

			msm8960_enable_ext_spk_amp_gpio(top_spk_pamp_gpio);
			pr_debug("%s: sleeping 4 ms after turning on "
				" external HAC Ampl\n", __func__);
			usleep_range(4000, 4000);
		}
	} else  {

		pr_err("%s: ERROR : Invalid External Speaker Ampl. spk = 0x%08x\n",
			__func__, spk);
		return;
	}
}

static void msm8960_ext_spk_power_amp_off(u32 spk)
{
	pr_info("%s, spk = %d\n", __func__, spk);

	if (spk & (BOTTOM_SPK_AMP_POS | BOTTOM_SPK_AMP_NEG)) {

		if (!msm8960_ext_bottom_spk_pamp)
			return;

		gpio_direction_output(bottom_spk_pamp_gpio, 0);
		gpio_free(bottom_spk_pamp_gpio);
		msm8960_ext_bottom_spk_pamp = 0;

		pr_debug("%s: sleeping 4 ms after turning off external Bottom"
			" Speaker Ampl\n", __func__);

		usleep_range(4000, 4000);

	} else if (spk & (TOP_SPK_AMP_POS | TOP_SPK_AMP_NEG)) {

		if (!msm8960_ext_top_spk_pamp)
			return;

		gpio_direction_output(top_spk_pamp_gpio, 0);
		gpio_free(top_spk_pamp_gpio);
		msm8960_ext_top_spk_pamp = 0;

		pr_debug("%s: sleeping 4 ms after turning off external Top"
			" Speaker Ampl\n", __func__);

		usleep_range(4000, 4000);
	} else  {

		pr_err("%s: ERROR : Invalid Ext Spk Ampl. spk = 0x%08x\n",
			__func__, spk);
		return;
	}
}

static void msm8960_ext_control(struct snd_soc_codec *codec)
{
	struct snd_soc_dapm_context *dapm = &codec->dapm;

	mutex_lock(&dapm->codec->mutex);

	pr_debug("%s: msm8960_spk_control = %d", __func__, msm8960_spk_control);
	if (msm8960_spk_control == MSM8960_SPK_ON) {
		snd_soc_dapm_enable_pin(dapm, "Ext Spk Bottom Pos");
		snd_soc_dapm_enable_pin(dapm, "Ext Spk Bottom Neg");
		snd_soc_dapm_enable_pin(dapm, "Ext Spk Top Pos");
		snd_soc_dapm_enable_pin(dapm, "Ext Spk Top Neg");
	} else {
		snd_soc_dapm_disable_pin(dapm, "Ext Spk Bottom Pos");
		snd_soc_dapm_disable_pin(dapm, "Ext Spk Bottom Neg");
		snd_soc_dapm_disable_pin(dapm, "Ext Spk Top Pos");
		snd_soc_dapm_disable_pin(dapm, "Ext Spk Top Neg");
	}

	snd_soc_dapm_sync(dapm);
	mutex_unlock(&dapm->codec->mutex);
}

static int msm8960_get_spk(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: msm8960_spk_control = %d", __func__, msm8960_spk_control);
	ucontrol->value.integer.value[0] = msm8960_spk_control;
	return 0;
}
static int msm8960_set_spk(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);

	pr_info("%s()\n", __func__);
	if (msm8960_spk_control == ucontrol->value.integer.value[0])
		return 0;

	msm8960_spk_control = ucontrol->value.integer.value[0];
	msm8960_ext_control(codec);
	return 1;
}
static int msm8960_spkramp_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *k, int event)
{
	pr_info("%s() %x\n", __func__, SND_SOC_DAPM_EVENT_ON(event));

	if (SND_SOC_DAPM_EVENT_ON(event)) {
		if (!strncmp(w->name, "Ext Spk Bottom Pos", 18))
			msm8960_ext_spk_power_amp_on(BOTTOM_SPK_AMP_POS);
		else if (!strncmp(w->name, "Ext Spk Bottom Neg", 18))
			msm8960_ext_spk_power_amp_on(BOTTOM_SPK_AMP_NEG);
		else if (!strncmp(w->name, "Ext Spk Top Pos", 15))
			msm8960_ext_spk_power_amp_on(TOP_SPK_AMP_POS);
		else if  (!strncmp(w->name, "Ext Spk Top Neg", 15))
			msm8960_ext_spk_power_amp_on(TOP_SPK_AMP_NEG);
		else {
			pr_err("%s() Invalid Speaker Widget = %s\n",
					__func__, w->name);
			return -EINVAL;
		}

	} else {
		if (!strncmp(w->name, "Ext Spk Bottom Pos", 18))
			msm8960_ext_spk_power_amp_off(BOTTOM_SPK_AMP_POS);
		else if (!strncmp(w->name, "Ext Spk Bottom Neg", 18))
			msm8960_ext_spk_power_amp_off(BOTTOM_SPK_AMP_NEG);
		else if (!strncmp(w->name, "Ext Spk Top Pos", 15))
			msm8960_ext_spk_power_amp_off(TOP_SPK_AMP_POS);
		else if  (!strncmp(w->name, "Ext Spk Top Neg", 15))
			msm8960_ext_spk_power_amp_off(TOP_SPK_AMP_NEG);
		else {
			pr_err("%s() Invalid Speaker Widget = %s\n",
					__func__, w->name);
			return -EINVAL;
		}
	}
	return 0;
}

static int msm8960_enable_codec_ext_clk(struct snd_soc_codec *codec, int enable,
					bool dapm)
{
	int r = 0;
	pr_info("%s: enable = %d\n", __func__, enable);

	mutex_lock(&cdc_mclk_mutex);
	if (enable) {
		clk_users++;
		pr_info("%s: clk_users = %d\n", __func__, clk_users);
		if (clk_users == 1) {
			if (codec_clk) {
				clk_set_rate(codec_clk, TABLA_EXT_CLK_RATE);
				clk_prepare_enable(codec_clk);
				tabla_mclk_enable(codec, 1, dapm);
			} else {
				pr_err("%s: Error setting Tabla MCLK\n",
				       __func__);
				clk_users--;
				r = -EINVAL;
			}
		}
	} else {
		if (clk_users > 0) {
			clk_users--;
			pr_info("%s: clk_users = %d\n", __func__, clk_users);
			if (clk_users == 0) {
				pr_info("%s: disabling MCLK. clk_users = %d\n",
					 __func__, clk_users);
				tabla_mclk_enable(codec, 0, dapm);
				clk_disable_unprepare(codec_clk);
			}
		} else {
			pr_err("%s: Error releasing Tabla MCLK\n", __func__);
			r = -EINVAL;
		}
	}
	mutex_unlock(&cdc_mclk_mutex);
	return r;
}

static int msm8960_mclk_event(struct snd_soc_dapm_widget *w,
		struct snd_kcontrol *kcontrol, int event)
{
	pr_debug("%s: event = %d\n", __func__, event);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		return msm8960_enable_codec_ext_clk(w->codec, 1, true);
	case SND_SOC_DAPM_POST_PMD:
		return msm8960_enable_codec_ext_clk(w->codec, 0, true);
	}
	return 0;
}

static const struct snd_kcontrol_new earamp_switch_controls =
	SOC_DAPM_SINGLE("Switch", 0, 0, 1, 0);

static const struct snd_kcontrol_new spkamp_switch_controls =
	SOC_DAPM_SINGLE("Switch", 0, 0, 1, 0);

static const struct snd_soc_dapm_widget ville_dapm_widgets[] = {
	SND_SOC_DAPM_MIXER("Lineout Mixer", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_MIXER("SPK AMP EN", SND_SOC_NOPM, 0, 0, &spkamp_switch_controls, 1),
	SND_SOC_DAPM_MIXER("EAR AMP EN", SND_SOC_NOPM, 0, 0, &earamp_switch_controls, 1),
};

static const struct snd_soc_dapm_widget msm8960_dapm_widgets[] = {
	SND_SOC_DAPM_SUPPLY("MCLK",  SND_SOC_NOPM, 0, 0,
	msm8960_mclk_event, SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_SPK("Ext Spk Bottom Pos", msm8960_spkramp_event),
	SND_SOC_DAPM_SPK("Ext Spk Bottom Neg", msm8960_spkramp_event),

	SND_SOC_DAPM_SPK("Ext Spk Top Pos", msm8960_spkramp_event),
	SND_SOC_DAPM_SPK("Ext Spk Top Neg", msm8960_spkramp_event),

	SND_SOC_DAPM_MIC("Handset Mic", NULL),
	SND_SOC_DAPM_MIC("Headset Mic", NULL),
	SND_SOC_DAPM_MIC("Back Mic", NULL),
	SND_SOC_DAPM_MIC("Digital Mic1", NULL),
	SND_SOC_DAPM_MIC("ANCRight Headset Mic", NULL),
	SND_SOC_DAPM_MIC("ANCLeft Headset Mic", NULL),

	SND_SOC_DAPM_MIC("Digital Mic1", NULL),
	SND_SOC_DAPM_MIC("Digital Mic2", NULL),
	SND_SOC_DAPM_MIC("Digital Mic3", NULL),
	SND_SOC_DAPM_MIC("Digital Mic4", NULL),
	SND_SOC_DAPM_MIC("Digital Mic5", NULL),
	SND_SOC_DAPM_MIC("Digital Mic6", NULL),

};

static const struct snd_soc_dapm_route tabla_1_x_audio_map[] = {

	{"Lineout Mixer", NULL, "LINEOUT2"},
	{"Lineout Mixer", NULL, "LINEOUT1"},
};

static const struct snd_soc_dapm_route tabla_2_x_audio_map[] = {

	{"Lineout Mixer", NULL, "LINEOUT3"},
	{"Lineout Mixer", NULL, "LINEOUT1"},
};

static const struct snd_soc_dapm_route common_audio_map[] = {

	{"RX_BIAS", NULL, "MCLK"},
	{"LDO_H", NULL, "MCLK"},

	
	{"Ext Spk Bottom Pos", NULL, "SPK AMP EN"},
	{"Ext Spk Bottom Neg", NULL, "SPK AMP EN"},
	{"SPK AMP EN", "Switch", "Lineout Mixer"},

	
	{"Ext Spk Top Pos", NULL, "EAR AMP EN"},
	{"Ext Spk Top Neg", NULL, "EAR AMP EN"},
	{"EAR AMP EN", "Switch", "Lineout Mixer"},

	
	{"AMIC1", NULL, "MIC BIAS1 External"},
	{"MIC BIAS1 External", NULL, "Handset Mic"},

	{"AMIC2", NULL, "MIC BIAS2 External"},
	{"MIC BIAS2 External", NULL, "Headset Mic"},

	{"AMIC3", NULL, "MIC BIAS3 External"},
	{"MIC BIAS3 External", NULL, "Back Mic"},

	{"HEADPHONE", NULL, "LDO_H"},

};

static const char *spk_function[] = {"Off", "On"};
static const char *slim0_rx_ch_text[] = {"One", "Two"};
static const char *slim0_tx_ch_text[] = {"One", "Two", "Three", "Four"};

static const struct soc_enum msm8960_enum[] = {
	SOC_ENUM_SINGLE_EXT(2, spk_function),
	SOC_ENUM_SINGLE_EXT(2, slim0_rx_ch_text),
	SOC_ENUM_SINGLE_EXT(4, slim0_tx_ch_text),
};

static const char *btsco_rate_text[] = {"8000", "16000"};
static const struct soc_enum msm8960_btsco_enum[] = {
		SOC_ENUM_SINGLE_EXT(2, btsco_rate_text),
};

static const char *auxpcm_rate_text[] = {"rate_8000", "rate_16000"};
static const struct soc_enum msm8960_auxpcm_enum[] = {
		SOC_ENUM_SINGLE_EXT(2, auxpcm_rate_text),
};

static int msm8960_slim_0_rx_ch_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: msm8960_slim_0_rx_ch  = %d\n", __func__,
			msm8960_slim_0_rx_ch);
	ucontrol->value.integer.value[0] = msm8960_slim_0_rx_ch - 1;
	return 0;
}

static int msm8960_slim_0_rx_ch_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	msm8960_slim_0_rx_ch = ucontrol->value.integer.value[0] + 1;

	pr_debug("%s: msm8960_slim_0_rx_ch = %d\n", __func__,
			msm8960_slim_0_rx_ch);
	return 1;
}

static int msm8960_slim_0_tx_ch_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: msm8960_slim_0_tx_ch  = %d\n", __func__,
			msm8960_slim_0_tx_ch);
	ucontrol->value.integer.value[0] = msm8960_slim_0_tx_ch - 1;
	return 0;
}

static int msm8960_slim_0_tx_ch_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	msm8960_slim_0_tx_ch = ucontrol->value.integer.value[0] + 1;

	pr_debug("%s: msm8960_slim_0_tx_ch = %d\n", __func__,
			msm8960_slim_0_tx_ch);
	return 1;
}


static int msm8960_btsco_rate_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: msm8960_btsco_rate  = %d", __func__, msm8960_btsco_rate);
	ucontrol->value.integer.value[0] = msm8960_btsco_rate;
	return 0;
}

static int msm8960_btsco_rate_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	switch (ucontrol->value.integer.value[0]) {
	case 0:
		msm8960_btsco_rate = SAMPLE_RATE_8KHZ;
		break;
	case 1:
		msm8960_btsco_rate = SAMPLE_RATE_16KHZ;
		break;
	default:
		msm8960_btsco_rate = SAMPLE_RATE_8KHZ;
		break;
	}
	pr_debug("%s: msm8960_btsco_rate = %d\n", __func__, msm8960_btsco_rate);
	return 0;
}

static int msm8960_auxpcm_rate_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: msm8960_auxpcm_rate  = %d", __func__,
		msm8960_auxpcm_rate);
	ucontrol->value.integer.value[0] = msm8960_auxpcm_rate;
	return 0;
}

static int msm8960_auxpcm_rate_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	switch (ucontrol->value.integer.value[0]) {
	case 0:
		msm8960_auxpcm_rate = SAMPLE_RATE_8KHZ;
		break;
	case 1:
		msm8960_auxpcm_rate = SAMPLE_RATE_16KHZ;
		break;
	default:
		msm8960_auxpcm_rate = SAMPLE_RATE_8KHZ;
		break;
	}
	pr_info("%s: msm8960_auxpcm_rate = %d"
		"ucontrol->value.integer.value[0] = %d\n", __func__,
		msm8960_auxpcm_rate,
		(int)ucontrol->value.integer.value[0]);
	return 0;
}

static const struct snd_kcontrol_new tabla_msm8960_controls[] = {
	SOC_ENUM_EXT("Speaker Function", msm8960_enum[0], msm8960_get_spk,
		msm8960_set_spk),
	SOC_ENUM_EXT("SLIM_0_RX Channels", msm8960_enum[1],
		msm8960_slim_0_rx_ch_get, msm8960_slim_0_rx_ch_put),
	SOC_ENUM_EXT("SLIM_0_TX Channels", msm8960_enum[2],
		msm8960_slim_0_tx_ch_get, msm8960_slim_0_tx_ch_put),
	SOC_ENUM_EXT("Internal BTSCO SampleRate", msm8960_btsco_enum[0],
		msm8960_btsco_rate_get, msm8960_btsco_rate_put),
	SOC_ENUM_EXT("AUX PCM SampleRate", msm8960_auxpcm_enum[0],
		msm8960_auxpcm_rate_get, msm8960_auxpcm_rate_put),
};

#if 0 
static const struct snd_kcontrol_new tabla_xbvol_controls[] = {
         SOC_SINGLE_TLV("LINEOUT1XB Volume", TABLA_A_RX_LINE_1_GAIN, 0, 12, 1,
                 line_gain),
         SOC_SINGLE_TLV("LINEOUT3XB Volume", TABLA_A_RX_LINE_3_GAIN, 0, 12, 1,
                 line_gain),
};
#endif

static int msm8960_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	int ret = 0;
	unsigned int rx_ch[SLIM_MAX_RX_PORTS], tx_ch[SLIM_MAX_TX_PORTS];
	unsigned int rx_ch_cnt = 0, tx_ch_cnt = 0;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {

		pr_info("%s: %s  rx_dai_id = %d  num_ch = %d\n", __func__,
			codec_dai->name, codec_dai->id, msm8960_slim_0_rx_ch);

		ret = snd_soc_dai_get_channel_map(codec_dai,
				&tx_ch_cnt, tx_ch, &rx_ch_cnt , rx_ch);
		if (ret < 0) {
			pr_err("%s: failed to get codec chan map\n", __func__);
			goto end;
		}

		ret = snd_soc_dai_set_channel_map(cpu_dai, 0, 0,
				msm8960_slim_0_rx_ch, rx_ch);
		if (ret < 0) {
			pr_err("%s: failed to set cpu chan map\n", __func__);
			goto end;
		}
		ret = snd_soc_dai_set_channel_map(codec_dai, 0, 0,
				msm8960_slim_0_rx_ch, rx_ch);
		if (ret < 0) {
			pr_err("%s: failed to set codec channel map\n",
								__func__);
			goto end;
		}
	} else {

		pr_info("%s: %s  tx_dai_id = %d  num_ch = %d\n", __func__,
			codec_dai->name, codec_dai->id, msm8960_slim_0_tx_ch);

		ret = snd_soc_dai_get_channel_map(codec_dai,
				&tx_ch_cnt, tx_ch, &rx_ch_cnt , rx_ch);
		if (ret < 0) {
			pr_err("%s: failed to get codec chan map\n", __func__);
			goto end;
		}
		ret = snd_soc_dai_set_channel_map(cpu_dai,
				msm8960_slim_0_tx_ch, tx_ch, 0 , 0);
		if (ret < 0) {
			pr_err("%s: failed to set cpu chan map\n", __func__);
			goto end;
		}
		ret = snd_soc_dai_set_channel_map(codec_dai,
				msm8960_slim_0_tx_ch, tx_ch, 0, 0);
		if (ret < 0) {
			pr_err("%s: failed to set codec channel map\n",
								__func__);
			goto end;
		}
	}
end:
	return ret;
}

static int msm8960_slimbus_2_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	int ret = 0;
	unsigned int rx_ch[SLIM_MAX_RX_PORTS], tx_ch[SLIM_MAX_TX_PORTS];
	unsigned int rx_ch_cnt = 0, tx_ch_cnt = 0;
	unsigned int num_tx_ch = 0;
	unsigned int num_rx_ch = 0;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {

		num_rx_ch =  params_channels(params);

		pr_info("%s: %s rx_dai_id = %d  num_ch = %d\n", __func__,
			codec_dai->name, codec_dai->id, num_rx_ch);

		ret = snd_soc_dai_get_channel_map(codec_dai,
				&tx_ch_cnt, tx_ch, &rx_ch_cnt , rx_ch);
		if (ret < 0) {
			pr_err("%s: failed to get codec chan map\n", __func__);
			goto end;
		}

		ret = snd_soc_dai_set_channel_map(cpu_dai, 0, 0,
				num_rx_ch, rx_ch);
		if (ret < 0) {
			pr_err("%s: failed to set cpu chan map\n", __func__);
			goto end;
		}
		ret = snd_soc_dai_set_channel_map(codec_dai, 0, 0,
				num_rx_ch, rx_ch);
		if (ret < 0) {
			pr_err("%s: failed to set codec channel map\n",
								__func__);
			goto end;
		}
	} else {
		num_tx_ch =  params_channels(params);

		pr_info("%s: %s  tx_dai_id = %d  num_ch = %d\n", __func__,
			codec_dai->name, codec_dai->id, num_tx_ch);

		ret = snd_soc_dai_get_channel_map(codec_dai,
				&tx_ch_cnt, tx_ch, &rx_ch_cnt , rx_ch);
		if (ret < 0) {
			pr_err("%s: failed to get codec chan map\n", __func__);
			goto end;
		}

		ret = snd_soc_dai_set_channel_map(cpu_dai,
				num_tx_ch, tx_ch, 0 , 0);
		if (ret < 0) {
			pr_err("%s: failed to set cpu chan map\n", __func__);
			goto end;
		}
		ret = snd_soc_dai_set_channel_map(codec_dai,
				num_tx_ch, tx_ch, 0, 0);
		if (ret < 0) {
			pr_err("%s: failed to set codec channel map\n",
								__func__);
			goto end;
		}
	}
end:
	return ret;
}

static int msm8960_audrx_init(struct snd_soc_pcm_runtime *rtd)
{
	int err;
	struct snd_soc_codec *codec = rtd->codec;
	struct snd_soc_dapm_context *dapm = &codec->dapm;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;

	pr_info("%s(), dev_name: %s\n", __func__, dev_name(cpu_dai->dev));

#if 0 
	if (system_rev == 1)  
	        err = snd_soc_add_controls(codec, tabla_xbvol_controls,
                                ARRAY_SIZE(tabla_xbvol_controls));

	if (err < 0)
		return err;
#endif

	snd_soc_dapm_new_controls(dapm, msm8960_dapm_widgets,
				ARRAY_SIZE(msm8960_dapm_widgets));

	snd_soc_dapm_new_controls(dapm, ville_dapm_widgets,
				ARRAY_SIZE(ville_dapm_widgets));

	snd_soc_dapm_add_routes(dapm, common_audio_map,
		ARRAY_SIZE(common_audio_map));

	pr_info("%s(), %s\n", __func__, codec->name);
	if (!strncmp(codec->name, "tabla1x_codec", 13))
		snd_soc_dapm_add_routes(dapm, tabla_1_x_audio_map,
			 ARRAY_SIZE(tabla_1_x_audio_map));
	else
		snd_soc_dapm_add_routes(dapm, tabla_2_x_audio_map,
			 ARRAY_SIZE(tabla_2_x_audio_map));

	snd_soc_dapm_enable_pin(dapm, "Ext Spk Bottom Pos");
	snd_soc_dapm_enable_pin(dapm, "Ext Spk Bottom Neg");
	snd_soc_dapm_enable_pin(dapm, "Ext Spk Top Pos");
	snd_soc_dapm_enable_pin(dapm, "Ext Spk Top Neg");

	snd_soc_dapm_sync(dapm);

	err = snd_soc_jack_new(codec, "Headset Jack",
			       (SND_JACK_HEADSET | SND_JACK_OC_HPHL |
				SND_JACK_OC_HPHR | SND_JACK_UNSUPPORTED),
			       &hs_jack);
	if (err) {
		pr_err("failed to create new jack\n");
		return err;
	}

	err = snd_soc_jack_new(codec, "Button Jack",
			       TABLA_JACK_BUTTON_MASK, &button_jack);
	if (err) {
		pr_err("failed to create new jack\n");
		return err;
	}

	codec_clk = clk_get(cpu_dai->dev, "osr_clk");


	return err;
}

static int msm8960_slim_0_rx_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
			struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
	SNDRV_PCM_HW_PARAM_RATE);

	struct snd_interval *channels = hw_param_interval(params,
			SNDRV_PCM_HW_PARAM_CHANNELS);

	pr_debug("%s()\n", __func__);
	rate->min = rate->max = 48000;
	channels->min = channels->max = msm8960_slim_0_rx_ch;

	return 0;
}

static int msm8960_slim_0_tx_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
			struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
	SNDRV_PCM_HW_PARAM_RATE);

	struct snd_interval *channels = hw_param_interval(params,
			SNDRV_PCM_HW_PARAM_CHANNELS);

	pr_debug("%s()\n", __func__);
	rate->min = rate->max = 48000;
	channels->min = channels->max = msm8960_slim_0_tx_ch;

	return 0;
}

static int msm8960_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
			struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
	SNDRV_PCM_HW_PARAM_RATE);

	pr_debug("%s()\n", __func__);
	rate->min = rate->max = 48000;

	return 0;
}

static int msm8960_hdmi_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
					struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);

	struct snd_interval *channels = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_CHANNELS);

	pr_info("%s channels->min %u channels->max %u ()\n", __func__,
			channels->min, channels->max);

	rate->min = rate->max = 48000;

	return 0;
}

static int msm8960_btsco_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
					struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);

	struct snd_interval *channels = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_CHANNELS);

	rate->min = rate->max = msm8960_btsco_rate;
	channels->min = channels->max = msm8960_btsco_ch;

	return 0;
}
static int msm8960_auxpcm_be_params_fixup(struct snd_soc_pcm_runtime *rtd,
					struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);

	struct snd_interval *channels = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_CHANNELS);

	rate->min = rate->max = msm8960_auxpcm_rate;
	
	channels->min = channels->max = 1;

	return 0;
}
static int msm8960_aux_pcm_get_gpios(void)
{
	int ret = 0;

	pr_info("%s\n", __func__);

	ret = gpio_request(GPIO_AUX_PCM_DOUT, "AUX PCM DOUT");
	if (ret < 0) {
		pr_err("%s: Failed to request gpio(%d): AUX PCM DOUT",
				__func__, GPIO_AUX_PCM_DOUT);
		goto fail_dout;
	}

	ret = gpio_request(GPIO_AUX_PCM_DIN, "AUX PCM DIN");
	if (ret < 0) {
		pr_err("%s: Failed to request gpio(%d): AUX PCM DIN",
				__func__, GPIO_AUX_PCM_DIN);
		goto fail_din;
	}

	ret = gpio_request(GPIO_AUX_PCM_SYNC, "AUX PCM SYNC");
	if (ret < 0) {
		pr_err("%s: Failed to request gpio(%d): AUX PCM SYNC",
				__func__, GPIO_AUX_PCM_SYNC);
		goto fail_sync;
	}
	ret = gpio_request(GPIO_AUX_PCM_CLK, "AUX PCM CLK");
	if (ret < 0) {
		pr_err("%s: Failed to request gpio(%d): AUX PCM CLK",
				__func__, GPIO_AUX_PCM_CLK);
		goto fail_clk;
	}

	return 0;

fail_clk:
	gpio_free(GPIO_AUX_PCM_SYNC);
fail_sync:
	gpio_free(GPIO_AUX_PCM_DIN);
fail_din:
	gpio_free(GPIO_AUX_PCM_DOUT);
fail_dout:

	return ret;
}

static int msm8960_aux_pcm_free_gpios(void)
{
	gpio_free(GPIO_AUX_PCM_DIN);
	gpio_free(GPIO_AUX_PCM_DOUT);
	gpio_free(GPIO_AUX_PCM_SYNC);
	gpio_free(GPIO_AUX_PCM_CLK);

	return 0;
}
static int msm8960_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;

	pr_debug("%s(): dai_link_str_name = %s cpu_dai = %s codec_dai = %s\n",
		__func__, rtd->dai_link->stream_name,
		rtd->dai_link->cpu_dai_name, rtd->dai_link->codec_dai_name);
	return 0;
}

static int msm8960_auxpcm_startup(struct snd_pcm_substream *substream)
{
	int ret = 0;

	pr_debug("%s(): substream = %s\n", __func__, substream->name);
	ret = msm8960_aux_pcm_get_gpios();
	if (ret < 0) {
		pr_err("%s: Aux PCM GPIO request failed\n", __func__);
		return -EINVAL;
	}
	return 0;
}

static void msm8960_auxpcm_shutdown(struct snd_pcm_substream *substream)
{

	pr_debug("%s(): substream = %s\n", __func__, substream->name);
	msm8960_aux_pcm_free_gpios();
}

static void msm8960_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;

	pr_debug("%s(): dai_link str_name = %s cpu_dai = %s codec_dai = %s\n",
		__func__, rtd->dai_link->stream_name,
		rtd->dai_link->cpu_dai_name, rtd->dai_link->codec_dai_name);
}

static struct snd_soc_ops msm8960_be_ops = {
	.startup = msm8960_startup,
	.hw_params = msm8960_hw_params,
	.shutdown = msm8960_shutdown,
};

static struct snd_soc_ops msm8960_auxpcm_be_ops = {
	.startup = msm8960_auxpcm_startup,
	.shutdown = msm8960_auxpcm_shutdown,
};

static struct snd_soc_ops msm8960_slimbus_2_be_ops = {
	.startup = msm8960_startup,
	.hw_params = msm8960_slimbus_2_hw_params,
	.shutdown = msm8960_shutdown,
};

static struct snd_soc_dai_link msm8960_dai_common[] = {
	
	{
		.name = "MSM8960 Media1",
		.stream_name = "MultiMedia1",
		.cpu_dai_name	= "MultiMedia1",
		.platform_name  = "msm-pcm-dsp",
		.dynamic = 1,
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.trigger = {SND_SOC_DPCM_TRIGGER_POST, SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1, 
		.be_id = MSM_FRONTEND_DAI_MULTIMEDIA1
	},
	{
		.name = "MSM8960 Media2",
		.stream_name = "MultiMedia2",
		.cpu_dai_name	= "MultiMedia2",
		.platform_name  = "msm-multi-ch-pcm-dsp",
		.dynamic = 1,
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.trigger = {SND_SOC_DPCM_TRIGGER_POST, SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1, 
		.be_id = MSM_FRONTEND_DAI_MULTIMEDIA2,
	},
	{
		.name = "Circuit-Switch Voice",
		.stream_name = "CS-Voice",
		.cpu_dai_name   = "CS-VOICE",
		.platform_name  = "msm-pcm-voice",
		.dynamic = 1,
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.trigger = {SND_SOC_DPCM_TRIGGER_POST, SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1, 
		.be_id = MSM_FRONTEND_DAI_CS_VOICE,
	},
	{
		.name = "MSM VoIP",
		.stream_name = "VoIP",
		.cpu_dai_name	= "VoIP",
		.platform_name  = "msm-voip-dsp",
		.dynamic = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST, SND_SOC_DPCM_TRIGGER_POST},
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1, 
		.be_id = MSM_FRONTEND_DAI_VOIP,
	},
	{
		.name = "MSM8960 LPA",
		.stream_name = "LPA",
		.cpu_dai_name	= "MultiMedia3",
		.platform_name  = "msm-pcm-lpa",
		.dynamic = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST, SND_SOC_DPCM_TRIGGER_POST},
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1, 
		.be_id = MSM_FRONTEND_DAI_MULTIMEDIA3,
	},
	
	{
		.name = "SLIMBUS_0 Hostless",
		.stream_name = "SLIMBUS_0 Hostless",
		.cpu_dai_name	= "SLIMBUS0_HOSTLESS",
		.platform_name  = "msm-pcm-hostless",
		.dynamic = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST, SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1, 
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
	},
	{
		.name = "INT_FM Hostless",
		.stream_name = "INT_FM Hostless",
		.cpu_dai_name	= "INT_FM_HOSTLESS",
		.platform_name  = "msm-pcm-hostless",
		.dynamic = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST, SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1, 
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
	},
	{
		.name = "MSM AFE-PCM RX",
		.stream_name = "AFE-PROXY RX",
		.cpu_dai_name = "msm-dai-q6.241",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-rx",
		.platform_name  = "msm-pcm-afe",
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1, 
	},
	{
		.name = "MSM AFE-PCM TX",
		.stream_name = "AFE-PROXY TX",
		.cpu_dai_name = "msm-dai-q6.240",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.platform_name  = "msm-pcm-afe",
		.ignore_suspend = 1,
	},
	{
		.name = "MSM8960 Compr",
		.stream_name = "COMPR",
		.cpu_dai_name	= "MultiMedia4",
		.platform_name  = "msm-compr-dsp",
		.dynamic = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST, SND_SOC_DPCM_TRIGGER_POST},
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1, 
		.be_id = MSM_FRONTEND_DAI_MULTIMEDIA4,
	},
	{
		.name = "AUXPCM Hostless",
		.stream_name = "AUXPCM Hostless",
		.cpu_dai_name	= "AUXPCM_HOSTLESS",
		.platform_name  = "msm-pcm-hostless",
		.dynamic = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST, SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1, 
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
	},
	
	{
		.name = "HDMI_RX_HOSTLESS",
		.stream_name = "HDMI_RX_HOSTLESS",
		.cpu_dai_name = "HDMI_HOSTLESS",
		.platform_name = "msm-pcm-hostless",
		.dynamic = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST, SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1, 
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
	},
	{
		.name = "VoLTE",
		.stream_name = "VoLTE",
		.cpu_dai_name   = "VoLTE",
		.platform_name  = "msm-pcm-voice",
		.dynamic = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST, SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1, 
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.be_id = MSM_FRONTEND_DAI_VOLTE,
	},
	{
		.name = "SGLTE",
		.stream_name = "SGLTE",
		.cpu_dai_name   = "SGLTE",
		.platform_name  = "msm-pcm-voice",
		.dynamic = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
					SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.be_id = MSM_FRONTEND_DAI_SGLTE,
	},
	
	{
		.name = LPASS_BE_INT_BT_SCO_RX,
		.stream_name = "Internal BT-SCO Playback",
		.cpu_dai_name = "msm-dai-q6.12288",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name	= "msm-stub-rx",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_INT_BT_SCO_RX,
		.be_hw_params_fixup = msm8960_btsco_be_hw_params_fixup,
		.ignore_pmdown_time = 1, 
	},
	{
		.name = LPASS_BE_INT_BT_SCO_TX,
		.stream_name = "Internal BT-SCO Capture",
		.cpu_dai_name = "msm-dai-q6.12289",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name	= "msm-stub-tx",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_INT_BT_SCO_TX,
		.be_hw_params_fixup = msm8960_btsco_be_hw_params_fixup,
	},
	{
		.name = LPASS_BE_INT_FM_RX,
		.stream_name = "Internal FM Playback",
		.cpu_dai_name = "msm-dai-q6.12292",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-rx",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_INT_FM_RX,
		.be_hw_params_fixup = msm8960_be_hw_params_fixup,
		.ignore_pmdown_time = 1, 
	},
	{
		.name = LPASS_BE_INT_FM_TX,
		.stream_name = "Internal FM Capture",
		.cpu_dai_name = "msm-dai-q6.12293",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_INT_FM_TX,
		.be_hw_params_fixup = msm8960_be_hw_params_fixup,
	},
	
	{
		.name = LPASS_BE_HDMI,
		.stream_name = "HDMI Playback",
		.cpu_dai_name = "msm-dai-q6-hdmi.8",
		.platform_name = "msm-pcm-routing",
		.codec_name     = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-rx",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_HDMI_RX,
		.be_hw_params_fixup = msm8960_hdmi_be_hw_params_fixup,
		.ignore_pmdown_time = 1, 
	},
	
	{
		.name = LPASS_BE_AFE_PCM_RX,
		.stream_name = "AFE Playback",
		.cpu_dai_name = "msm-dai-q6.224",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-rx",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_AFE_PCM_RX,
		.ignore_pmdown_time = 1, 
	},
	{
		.name = LPASS_BE_AFE_PCM_TX,
		.stream_name = "AFE Capture",
		.cpu_dai_name = "msm-dai-q6.225",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_AFE_PCM_TX,
	},
	
	{
		.name = LPASS_BE_AUXPCM_RX,
		.stream_name = "AUX PCM Playback",
		.cpu_dai_name = "msm-dai-q6.2",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-rx",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_AUXPCM_RX,
		.be_hw_params_fixup = msm8960_auxpcm_be_params_fixup,
		.ops = &msm8960_auxpcm_be_ops,
		.ignore_pmdown_time = 1,
	},
	{
		.name = LPASS_BE_AUXPCM_TX,
		.stream_name = "AUX PCM Capture",
		.cpu_dai_name = "msm-dai-q6.3",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_AUXPCM_TX,
		.be_hw_params_fixup = msm8960_auxpcm_be_params_fixup,
	},
	
	{
		.name = LPASS_BE_VOICE_PLAYBACK_TX,
		.stream_name = "Voice Farend Playback",
		.cpu_dai_name = "msm-dai-q6.32773",
		.platform_name = "msm-pcm-routing",
		.codec_name     = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-rx",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_VOICE_PLAYBACK_TX,
		.be_hw_params_fixup = msm8960_be_hw_params_fixup,
	},
	
	{
		.name = LPASS_BE_INCALL_RECORD_TX,
		.stream_name = "Voice Uplink Capture",
		.cpu_dai_name = "msm-dai-q6.32772",
		.platform_name = "msm-pcm-routing",
		.codec_name     = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_INCALL_RECORD_TX,
		.be_hw_params_fixup = msm8960_be_hw_params_fixup,
	},
	
	{
		.name = LPASS_BE_INCALL_RECORD_RX,
		.stream_name = "Voice Downlink Capture",
		.cpu_dai_name = "msm-dai-q6.32771",
		.platform_name = "msm-pcm-routing",
		.codec_name     = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_INCALL_RECORD_RX,
		.be_hw_params_fixup = msm8960_be_hw_params_fixup,
		.ignore_pmdown_time = 1, 
	},
	{
		.name = "MSM8960 Media5",
		.stream_name = "MultiMedia5",
		.cpu_dai_name	= "MultiMedia5",
		.platform_name	= "msm-multi-ch-pcm-dsp",
		.dynamic = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
							SND_SOC_DPCM_TRIGGER_POST},
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1, 
		.be_id = MSM_FRONTEND_DAI_MULTIMEDIA5
	},
	{
		.name = "MSM8960 Media6",
		.stream_name = "MultiMedia6",
		.cpu_dai_name	= "MultiMedia6",
		.platform_name	= "msm-multi-ch-pcm-dsp",
		.dynamic = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
					SND_SOC_DPCM_TRIGGER_POST},
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1, 
		.be_id = MSM_FRONTEND_DAI_MULTIMEDIA6
	},
	{
		.name = "MSM8960 Compr2",
		.stream_name = "COMPR2",
		.cpu_dai_name	= "MultiMedia7",
		.platform_name	= "msm-compr-dsp",
		.dynamic = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
					SND_SOC_DPCM_TRIGGER_POST},
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1, 
		.be_id = MSM_FRONTEND_DAI_MULTIMEDIA7,
	},
	{
		.name = "MSM8960 Compr3",
		.stream_name = "COMPR3",
		.cpu_dai_name	= "MultiMedia8",
		.platform_name	= "msm-compr-dsp",
		.dynamic = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
					SND_SOC_DPCM_TRIGGER_POST},
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1, 
		.be_id = MSM_FRONTEND_DAI_MULTIMEDIA8,
	},
};

static struct snd_soc_dai_link msm8960_dai_delta_tabla1x[] = {
	
	{
		.name = LPASS_BE_SLIMBUS_0_RX,
		.stream_name = "Slimbus Playback",
		.cpu_dai_name = "msm-dai-q6.16384",
		.platform_name = "msm-pcm-routing",
		.codec_name     = "tabla1x_codec",
		.codec_dai_name	= "tabla_rx1",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_SLIMBUS_0_RX,
		.init = &msm8960_audrx_init,
		.be_hw_params_fixup = msm8960_slim_0_rx_be_hw_params_fixup,
		.ops = &msm8960_be_ops,
		.ignore_pmdown_time = 1, 
	},
	{
		.name = LPASS_BE_SLIMBUS_0_TX,
		.stream_name = "Slimbus Capture",
		.cpu_dai_name = "msm-dai-q6.16385",
		.platform_name = "msm-pcm-routing",
		.codec_name     = "tabla1x_codec",
		.codec_dai_name	= "tabla_tx1",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_SLIMBUS_0_TX,
		.be_hw_params_fixup = msm8960_slim_0_tx_be_hw_params_fixup,
		.ops = &msm8960_be_ops,
	},
	
	{
		.name = "SLIMBUS_2 Hostless Capture",
		.stream_name = "SLIMBUS_2 Hostless Capture",
		.cpu_dai_name = "msm-dai-q6.16389",
		.platform_name = "msm-pcm-hostless",
		.codec_name = "tabla1x_codec",
		.codec_dai_name = "tabla_tx2",
		.ignore_suspend = 1,
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ops = &msm8960_slimbus_2_be_ops,
	},
	
	{
		.name = "SLIMBUS_2 Hostless Playback",
		.stream_name = "SLIMBUS_2 Hostless Playback",
		.cpu_dai_name = "msm-dai-q6.16388",
		.platform_name = "msm-pcm-hostless",
		.codec_name = "tabla1x_codec",
		.codec_dai_name = "tabla_rx3",
		.ignore_suspend = 1,
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ops = &msm8960_slimbus_2_be_ops,
	},
};


static struct snd_soc_dai_link msm8960_dai_delta_tabla2x[] = {
	
	{
		.name = LPASS_BE_SLIMBUS_0_RX,
		.stream_name = "Slimbus Playback",
		.cpu_dai_name = "msm-dai-q6.16384",
		.platform_name = "msm-pcm-routing",
		.codec_name     = "tabla_codec",
		.codec_dai_name	= "tabla_rx1",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_SLIMBUS_0_RX,
		.init = &msm8960_audrx_init,
		.be_hw_params_fixup = msm8960_slim_0_rx_be_hw_params_fixup,
		.ops = &msm8960_be_ops,
		.ignore_pmdown_time = 1, 
	},
	{
		.name = LPASS_BE_SLIMBUS_0_TX,
		.stream_name = "Slimbus Capture",
		.cpu_dai_name = "msm-dai-q6.16385",
		.platform_name = "msm-pcm-routing",
		.codec_name     = "tabla_codec",
		.codec_dai_name	= "tabla_tx1",
		.no_pcm = 1,
		.be_id = MSM_BACKEND_DAI_SLIMBUS_0_TX,
		.be_hw_params_fixup = msm8960_slim_0_tx_be_hw_params_fixup,
		.ops = &msm8960_be_ops,
	},
	
	{
		.name = "SLIMBUS_2 Hostless Capture",
		.stream_name = "SLIMBUS_2 Hostless Capture",
		.cpu_dai_name = "msm-dai-q6.16389",
		.platform_name = "msm-pcm-hostless",
		.codec_name = "tabla_codec",
		.codec_dai_name = "tabla_tx2",
		.ignore_suspend = 1,
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ops = &msm8960_slimbus_2_be_ops,
	},
	
	{
		.name = "SLIMBUS_2 Hostless Playback",
		.stream_name = "SLIMBUS_2 Hostless Playback",
		.cpu_dai_name = "msm-dai-q6.16388",
		.platform_name = "msm-pcm-hostless",
		.codec_name = "tabla_codec",
		.codec_dai_name = "tabla_rx3",
		.ignore_suspend = 1,
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ops = &msm8960_slimbus_2_be_ops,
	},
};

static struct snd_soc_dai_link msm8960_tabla1x_dai[
					 ARRAY_SIZE(msm8960_dai_common) +
					 ARRAY_SIZE(msm8960_dai_delta_tabla1x)];


static struct snd_soc_dai_link msm8960_dai[
					 ARRAY_SIZE(msm8960_dai_common) +
					 ARRAY_SIZE(msm8960_dai_delta_tabla2x)];

static struct snd_soc_card snd_soc_tabla1x_card_msm8960 = {
		.name		= "msm8960-tabla1x-snd-card",
		.dai_link	= msm8960_tabla1x_dai,
		.num_links	= ARRAY_SIZE(msm8960_tabla1x_dai),
		.controls = tabla_msm8960_controls,
		.num_controls = ARRAY_SIZE(tabla_msm8960_controls),
};

static struct snd_soc_card snd_soc_card_msm8960 = {
		.name		= "msm8960-snd-card",
		.dai_link	= msm8960_dai,
		.num_links	= ARRAY_SIZE(msm8960_dai),
		.controls = tabla_msm8960_controls,
		.num_controls = ARRAY_SIZE(tabla_msm8960_controls),
};

void ville_set_q6_effect_mode(int mode)
{
	pr_aud_info("%s: mode %d\n", __func__, mode);
	atomic_set(&q6_effect_mode, mode);
}

int ville_get_q6_effect_mode(void)
{
	int mode = atomic_read(&q6_effect_mode);
	pr_aud_info("%s: mode %d\n", __func__, mode);
	return mode;
}

static struct acoustic_ops acoustic = {
	.set_q6_effect = ville_set_q6_effect_mode,
};

static struct q6asm_ops qops = {
	.get_q6_effect = ville_get_q6_effect_mode,
};

static struct msm_pcm_routing_ops rops = {
	.get_q6_effect = ville_get_q6_effect_mode,
};

static struct platform_device *msm8960_snd_device;
static struct platform_device *msm8960_snd_tabla1x_device;

static int __init ville_audio_init(void)
{
	int ret;

	if (!cpu_is_msm8960()) {
		pr_info("%s: Not the right machine type\n", __func__);
		return -ENODEV ;
	}
	pr_info("%s", __func__);

	msm8960_snd_device = platform_device_alloc("soc-audio", 0);
	if (!msm8960_snd_device) {
		pr_err("Platform device allocation failed\n");
		return -ENOMEM;
	}

	memcpy(msm8960_dai, msm8960_dai_common, sizeof(msm8960_dai_common));
	memcpy(msm8960_dai + ARRAY_SIZE(msm8960_dai_common),
		msm8960_dai_delta_tabla2x, sizeof(msm8960_dai_delta_tabla2x));

	platform_set_drvdata(msm8960_snd_device, &snd_soc_card_msm8960);
	ret = platform_device_add(msm8960_snd_device);
	if (ret) {
		platform_device_put(msm8960_snd_device);
		return ret;
	}

	msm8960_snd_tabla1x_device = platform_device_alloc("soc-audio", 1);
	if (!msm8960_snd_tabla1x_device) {
		pr_err("Platform device allocation failed\n");
		return -ENOMEM;
	}

	memcpy(msm8960_tabla1x_dai, msm8960_dai_common,
		sizeof(msm8960_dai_common));
	memcpy(msm8960_tabla1x_dai + ARRAY_SIZE(msm8960_dai_common),
		msm8960_dai_delta_tabla1x, sizeof(msm8960_dai_delta_tabla1x));

	platform_set_drvdata(msm8960_snd_tabla1x_device,
		&snd_soc_tabla1x_card_msm8960);
	ret = platform_device_add(msm8960_snd_tabla1x_device);
	if (ret) {
		platform_device_put(msm8960_snd_tabla1x_device);
		return ret;
	}

	mutex_init(&cdc_mclk_mutex);
	htc_register_q6asm_ops(&qops);
	htc_register_pcm_routing_ops(&rops);
	acoustic_register_ops(&acoustic);
	return ret;

}
late_initcall(ville_audio_init);

static void __exit ville_audio_exit(void)
{

	if (!cpu_is_msm8960()) {
		pr_info("%s: Not the right machine type\n", __func__);
		return ;
	}
	pr_info("%s", __func__);

	platform_device_unregister(msm8960_snd_device);
	platform_device_unregister(msm8960_snd_tabla1x_device);
	mutex_destroy(&cdc_mclk_mutex);
}
module_exit(ville_audio_exit);

MODULE_DESCRIPTION("ALSA Platform Ville");
MODULE_LICENSE("GPL v2");
