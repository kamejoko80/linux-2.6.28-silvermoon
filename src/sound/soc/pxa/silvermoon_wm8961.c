/*
 * silvermoon_wm8961.c  --  SoC audio for Chumby silvermoon platform.
							(derived from aspenite.c interface to the WM8753 from Marvell)
 *
 *
 *  TTC FPGA audio amplifier code taken from arch/arm/mach-pxa/mainstone.c
 *  Copyright:	MontaVista Software Inc.
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 *
 *  Revision history
 *    18th Jun 2009   Initial version.
 *
 */


//---------------------------
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/device.h>
#include <linux/i2c.h>
//#include <sound/driver.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>

#include <asm/mach-types.h>
#include <asm/io.h>

#include <asm/uaccess.h>
#include <plat/regs-ssp.h>

#include <linux/serio.h>
#include <plat/ssp.h>
#include <mach/addr-map.h>


#include "../codecs/wm8961.h"
#include "pxa3xx-pcm.h"
#include "pxa3xx-ssp.h"

#undef SILVERMOON_WM8961_DEBUG
//#define ZYLONITEII_SSP_SYSFS

#ifdef CONFIG_SND_PXA3XX_SOC_SILVERMOON_DEBUG
#define SILVERMOON_WM8961_DEBUG 1
#endif

#ifdef SILVERMOON_WM8961_DEBUG
#define dbg(format, arg...) \
	printk(KERN_INFO format "\n" , ## arg)
#else
	#define dbg(format, arg...) do {} while (0)
#endif


static struct snd_soc_machine silvermoon;

struct _ssp_conf {
	unsigned int main_clk;
	unsigned int sys_num;
	unsigned int sys_den;
	unsigned int bit_clk;
	unsigned int ssp_num;
	unsigned int ssp_den;
	unsigned int freq_out;
};


static const struct _ssp_conf ssp_conf[] = {
	/*main_clk, sys_num, sys_den, bit_clk, ssp_num, ssp_den, freq_out*/
	{12288000,  0x659,    0x40,   3072000, 0x100,   0x40,	48000},
	{11289600, 0x1fa1,   0x125,   2822000, 0x100,   0x40,   44100},
	{12288000,  0x659,    0x40,   2048000, 0x180,   0x40,	32000},
	{12288000,  0x659,    0x40,   1536000, 0x200,   0x40,	24000},
	{11289600,  0x6E9,    0x40,   1411000, 0x200,   0x40,	22050},
	{12288000,  0x659,    0x40,   1024000, 0x300,   0x40,	16000},
	{11289600,  0x6E9,    0x40,    706500, 0x400,   0x40,	11025},
	{12288000,  0x659,    0x40,    512000, 0x600,   0x40,	8000},
	{12288000,  0x659,    0x40,   6144000, 0x100,   0x80,	96000},
};

static int ssp_conf_lookup(unsigned int rate)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(ssp_conf); i++) {
		if (ssp_conf[i].freq_out == rate)
		{
			dbg("ssp_conf_lookup: %d\n", i);
			return i;
		}
	}
	return -EINVAL;
}


#define ASYSDR   (* ((volatile unsigned long *) (APB_VIRT_BASE + 0x51050)))
#define ASSPDR   (* ((volatile unsigned long *) (APB_VIRT_BASE + 0x51054)))
static int ssp_set_clock(unsigned int ceoff)
{

	//ATB- though Marvell chose not mention this in the code,
	// these registers control the clock rates for the sysclock and bitclock
	int asysdr,asspdr;
	asysdr = ssp_conf[ceoff].sys_num << 16 | ssp_conf[ceoff].sys_den;
	ASYSDR = asysdr;
	asspdr = ssp_conf[ceoff].ssp_num << 16 | ssp_conf[ceoff].ssp_den;
	ASSPDR = asspdr;
}


static int silvermoon_wm8961_init(struct snd_soc_codec *codec)
{
	dbg(KERN_INFO "silvermoon_wm8961_init");

	//ATB provide MCLK to the codec at startup, because the codec requires it
	//  for us to write to registers
	ssp_set_clock(0);

	return 0;
}

static int silvermoon_wm8961_hifi_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai_link *machine = rtd->dai;
	struct snd_soc_dai *cpu_dai = machine->cpu_dai;
	struct ssp_device *ssp = cpu_dai->private_data;
	struct snd_soc_dai *codec_dai = machine->codec_dai;
	unsigned int format;
	dbg("silvermoon_wm8961_startup");

	cpu_dai->playback.channels_min = 2;
	cpu_dai->playback.channels_max = 2;

	//ATB - some SSP initializations that I have expected would be in the pxa3xx-ssp module
	__raw_writel(0xE1C0003F, ssp->mmio_base + SSCR0);
	__raw_writel(0x00701DC0, ssp->mmio_base + SSCR1);
	__raw_writel(0x3, ssp->mmio_base + SSTSA);
	__raw_writel(0x3, ssp->mmio_base + SSRSA);
	__raw_writel(0x40200004, ssp->mmio_base + SSPSP);

	format = SND_SOC_DAIFMT_LEFT_J | SND_SOC_DAIFMT_CBS_CFS | SND_SOC_DAIFMT_NB_NF;
	codec_dai->dai_ops.set_fmt(codec_dai, format);


	snd_soc_dai_set_sysclk(codec_dai, 0, ssp_conf[1].main_clk, 0);



	return 0;
}

static int silvermoon_wm8961_hifi_prepare(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai_link *machine = rtd->dai;
	struct snd_soc_dai *cpu_dai = machine->cpu_dai;
	struct snd_soc_dai *codec_dai = machine->codec_dai;
	struct ssp_device *ssp = cpu_dai->private_data;
	struct snd_pcm_runtime *runtime = substream->runtime;
	unsigned long rate = runtime->rate;
	int ceoff;
	dbg("silvermoon_wm8961_hifi_prepare");

	__raw_writel(0xE1C0003F, ssp->mmio_base + SSCR0);
	__raw_writel(0x00701DC0, ssp->mmio_base + SSCR1);
	__raw_writel(0x3, ssp->mmio_base + SSTSA);
	__raw_writel(0x3, ssp->mmio_base + SSRSA);
	__raw_writel(0x40200004, ssp->mmio_base + SSPSP);

	ceoff = ssp_conf_lookup(rate);

	snd_soc_dai_set_sysclk(codec_dai, 0, ssp_conf[ceoff].main_clk, 0);

	if (ceoff >= 0 )
	{
		ssp_set_clock(ceoff);
	} else {
		printk(KERN_ERR "Wrong audio sample rate\n");
	}


	return 0;
}

static int silvermoon_wm8961_hifi_shutdown(struct snd_pcm_substream *substream)
{
	dbg("silvermoon_wm8961_hifi_shutdown");
	return 0;
}

static int silvermoon_wm8961_hifi_hw_params(struct snd_pcm_substream *substream)
{

	return 0;
}


/* machine stream operations */
static struct snd_soc_ops silvermoon_wm8961_machine_ops = {
	.startup = silvermoon_wm8961_hifi_startup,
	.prepare = silvermoon_wm8961_hifi_prepare,
	.shutdown = silvermoon_wm8961_hifi_shutdown,
	.hw_params = silvermoon_wm8961_hifi_hw_params,
};

static struct snd_soc_dai_link silvermoon_dai[] = {
{
	.name = "WM8961",
	.stream_name = "WM8961 HiFi",
	.cpu_dai = &pxa3xx_ssp_dai[0],
	.codec_dai = &wm8961_dai,
	.ops = &silvermoon_wm8961_machine_ops,
	.init = silvermoon_wm8961_init,
},

};

static struct snd_soc_machine silvermoon = {
	.name = "SILVERMOON WM8961",
	.dai_link = silvermoon_dai,
	.num_links = ARRAY_SIZE(silvermoon_dai),
};

static struct wm8961_setup_data wm8961_setup = {
	.i2c_address = 0x1b,
	.i2c_bus = 1,
};

static struct snd_soc_device silvermoon_snd_devdata = {
	.machine = &silvermoon,
	.platform = &pxa3xx_soc_platform,
	.codec_dev = &soc_codec_dev_wm8961,
	.codec_data = &wm8961_setup,
};

static struct platform_device *silvermoon_snd_device;


static int __init silvermoon_init(void)
{
	int ret;
	int err;

	dbg(KERN_INFO "silvermoon_init");
	if (!machine_is_silvermoon())
		return -ENODEV;

	silvermoon_snd_device = platform_device_alloc("soc-audio", -1);
	if (!silvermoon_snd_device)
		return -ENOMEM;

#ifdef SILVERMOON_WM8961_DEBUG
	printk( KERN_INFO "%s:%d %s()\n", __FILE__, __LINE__, __FUNCTION__ );
#endif

	platform_set_drvdata(silvermoon_snd_device, &silvermoon_snd_devdata);
	silvermoon_snd_devdata.dev = &silvermoon_snd_device->dev;
	ret = platform_device_add(silvermoon_snd_device);
	if (ret)
		platform_device_put(silvermoon_snd_device);

	return ret;
}

static void __exit silvermoon_exit(void)
{
	platform_device_unregister(silvermoon_snd_device);
}

module_init(silvermoon_init);
module_exit(silvermoon_exit);

/* Module information */
MODULE_AUTHOR("chumby, www.chumby.com");
MODULE_LICENSE("GPL");
