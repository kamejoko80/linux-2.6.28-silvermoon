/*
 *  linux/drivers/mmc/host/pxa_sdh.c - PXAxxx SD Host driver
 *
 *  Copyright (C) 2008-2009 Marvell International Ltd.
 *                Philip Rakity <prakity@marvell.com>
 *
 *  Based on linux/drivers/mmc/host/sdhci-pci.c - PXA MMC shim driver
 * 
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/mmc/host.h>
#include <linux/mmc/card.h>
#include <linux/dma-mapping.h>

#include <mach/mmc.h>

#include "sdhci.h"

#define SD_RESP_6	0x1c		/* Command Response 6 */
#define SD_RESP_7	0x1e		/* Command Response 7 */

#define SD_FIFO_PARAM	0xE0
#define DIS_PAD_SD_CLK_GATE_BIT	(1 << 10) /* Turn on/off Dynamic SD Clock Gating */

#define SD_CLOCK_AND_BURST_SIZE_SETUP	0xE6
#define SDCLK_DELAY_SHIFT	10
#define SDCLK_SEL_SHIFT		8
#define SDCLK_SEL_MASK		0x3

#define DRIVER_NAME	"pxa-sdh"
#define MAX_SLOTS	8

#define DEFAULT_NO_DYNAMIC_CLOCKING	0

struct sdhci_mmc_slot {
	struct sdhci_mmc_chip	*chip;
	struct sdhci_host	*host;
	struct clk	*clk;
	u32	clkrate;
	u8	width;
	u8	eightBitEnabled;
	u8	clockEnabled;
	u8	no_dynamic_SD_clocking;
};

struct sdhci_mmc_chip {
	struct platform_device	*pdev;
	struct resource	*res;
	struct sdhci_mmc_fixes 	*fixes;
	unsigned int	quirks;
	int	num_slots;	/* Slots on controller */
	struct sdhci_mmc_slot	*slots[MAX_SLOTS];
	struct pxasdh_platform_data *pdata;
};

struct sdhci_mmc_fixes {
	unsigned int	quirks;
	int	(*probe)(struct sdhci_mmc_chip*);
	int	(*probe_slot)(struct sdhci_mmc_slot*);
	void	(*remove_slot)(struct sdhci_mmc_slot*, int);
	int	(*suspend)(struct sdhci_mmc_chip*, pm_message_t);
	int	(*resume)(struct sdhci_mmc_chip*);
};

#define DBG(f, x...) \
	pr_debug(DRIVER_NAME " [%s()]: " f, __func__,## x)

static void inline programFIFO (struct sdhci_host *host)
{
	struct sdhci_mmc_slot *slot = sdhci_priv(host);
	unsigned short tmp;

	tmp = readw(host->ioaddr + SD_FIFO_PARAM);
	DBG("ENTER %s SD_FIFO_PARAM = %04X\n", mmc_hostname(host->mmc), tmp);
	
	if (slot->no_dynamic_SD_clocking)
		tmp |= DIS_PAD_SD_CLK_GATE_BIT;
	else
		tmp &= ~DIS_PAD_SD_CLK_GATE_BIT;

	writew(tmp, host->ioaddr + SD_FIFO_PARAM);
	DBG("EXIT: %s SD_FIFO_PARAM = %04X\n", mmc_hostname(host->mmc), tmp);
}

static int platform_supports_8_bit(struct sdhci_host *host)
{
	struct sdhci_mmc_slot *slot = sdhci_priv(host);
	
	return slot->width >= 8;
}

#define SD_CE_ATA_2		0xEA
#define MMC_CARD 	(1<<12)
#define	MMC_WIDTH	(1<<8)

static void platform_clear_8_bit(struct sdhci_host *host)
{
	unsigned short tmp;
	struct sdhci_mmc_slot *slot = sdhci_priv(host);
	
	tmp = readw(host->ioaddr + SD_CE_ATA_2);
	tmp &= ~(MMC_CARD | MMC_WIDTH);
	writew(tmp, host->ioaddr + SD_CE_ATA_2);
	slot->eightBitEnabled = 0;
	DBG("EXIT: %s SD_CE_ATA_2 = %04X\n", mmc_hostname(host->mmc), tmp);
}

static void platform_set_8_bit(struct sdhci_host *host)
{
	unsigned short tmp;
	struct sdhci_mmc_slot *slot = sdhci_priv(host);
	
	tmp = readw(host->ioaddr + SD_CE_ATA_2);
	tmp |= MMC_CARD | MMC_WIDTH;
	writew(tmp, host->ioaddr + SD_CE_ATA_2);
	slot->eightBitEnabled = 1;
	DBG("EXIT: %s SD_CE_ATA_2 = %04X\n", mmc_hostname(host->mmc), tmp);
}

static int platform_init_after_reset (struct sdhci_host *host)
{	
	struct sdhci_mmc_slot *slot = sdhci_priv(host);
	int tmp;

	programFIFO(host);
	
	if (platform_supports_8_bit(host)) {
		if (slot->eightBitEnabled)
			platform_set_8_bit(host);
		else
			platform_clear_8_bit(host);
	}
	
	
#if defined(CONFIG_MACH_CHUMBY_SILVERMOON)
	tmp = readw(host->ioaddr + SD_CLOCK_AND_BURST_SIZE_SETUP) &
		~(SDCLK_SEL_MASK << SDCLK_SEL_SHIFT);
	writew(tmp, host->ioaddr + SD_CLOCK_AND_BURST_SIZE_SETUP);
#define SD_HOST_CTRL		0x0028		/* Host Control */
	// manipulate the SD_HOST_CTRL reg for MMC3 (see sec A.28.21 in software manual)
	// The silvermoon unit should *always* have an SD card in for the rootfs.  
	// OR in the bits for a fake card detect signal always on
	if (host->mmc->index == 2)
	{
		tmp = readw(host->ioaddr + SD_HOST_CTRL);
		tmp  |= (1<<6) | (1<<7);
		writew(tmp, host->ioaddr + SD_HOST_CTRL);
	}
#endif

	DBG ("SD_CLOCK_AND_BURST_SIZE_SETUP to %04X\n", readw(host->ioaddr + SD_CLOCK_AND_BURST_SIZE_SETUP));
	return 0;
}


/* 
 * we cannot talk to controller for 8 bus cycles according to sdio spec
 * at lowest speed this is 100,000 HZ per cycle or 800,000 cycles
 * which is quite a LONG TIME on a fast cpu -- so delay if needed
 */
static void platform_specific_delay (struct sdhci_host *host)
{
	unsigned long delay;
	
	if (host->clock < 3200000) {
		if (platform_supports_8_bit(host) == 0) {
			delay = 5*(3000000/host->clock) + 9;
			mdelay(delay);
		}
	}
}

static void platform_specific_sdio (struct sdhci_host *host, int enable)
{
	struct sdhci_mmc_slot *slot = sdhci_priv(host);
	
	DBG("ENTER: %s enable = %d, no_dynamic_SD_clocking = %d\n", 
		mmc_hostname(host->mmc), enable, slot->no_dynamic_SD_clocking);

	if (enable) {
		slot->no_dynamic_SD_clocking = 1;
		programFIFO(host);
	}
	else
	{
		slot->no_dynamic_SD_clocking = 0;
		programFIFO(host);
	}
	DBG("EXIT: %s enable = %d, no_dynamic_SD_clocking = %d\n", 
		mmc_hostname(host->mmc), enable, slot->no_dynamic_SD_clocking);

}

static void platform_specific_reset (struct sdhci_host *host, u8 mask)
{
	DBG("ENTER: %s mask == SDHCI_RESET_ALL = %d\n", mmc_hostname (host->mmc), mask == SDHCI_RESET_ALL);

	platform_init_after_reset (host);
}

static void enable_clock (struct sdhci_host *host)
{
	struct sdhci_mmc_slot *slot = sdhci_priv(host);
	
	DBG("ENTER: %s slot->clockEnabled = %d\n", mmc_hostname(host->mmc), slot->clockEnabled);

	if (slot->clockEnabled == 0) {
		clk_enable(slot->clk);
		slot->clockEnabled = 1;
	}
}


static void disable_clock (struct sdhci_host *host)
{
	struct sdhci_mmc_slot *slot = sdhci_priv(host);
	
	DBG("ENTER:\n");

	if (slot->clockEnabled) {
		clk_disable(slot->clk);
		slot->clockEnabled = 0;
	}
}

static void set_clock (struct sdhci_host *host, unsigned int clock)
{
	DBG("ENTER: clock = %d\n", clock);

	if (clock == 0)
		disable_clock(host);
	else
		enable_clock(host);
}


static int handle_shared_mmc_bus (struct sdhci_host *host, u32 intmask)
{
	struct sdhci_mmc_slot *slot = sdhci_priv(host);
	
	DBG("ENTER: hostname = %s, intmask = %08X\n", mmc_hostname(host->mmc), intmask);
	DBG("ENTER: slot = %p, chip = %p\n", slot, slot->chip);
	DBG("ENTER: hostname = %s, no_dynamic_SD_clocking = %d\n", 
		mmc_hostname(host->mmc), slot->no_dynamic_SD_clocking);

	if (intmask & SDHCI_INT_CARD_INSERT) {
		if(slot->chip->pdata && slot->chip->pdata->mfp_config)	
			slot->chip->pdata->mfp_config();
	
		/* force clock on */
		clk_enable(slot->clk);
		slot->clockEnabled = 1;
	}
	else if (intmask & SDHCI_INT_CARD_REMOVE) {
		slot->no_dynamic_SD_clocking = DEFAULT_NO_DYNAMIC_CLOCKING;
		programFIFO (host);
	}
	DBG("EXIT: hostname = %s, no_dynamic_SD_clocking = %d\n", 
		mmc_hostname(host->mmc), slot->no_dynamic_SD_clocking);
	
	return 0;
}



static unsigned int get_max_clock (struct sdhci_host *host)
{
	struct sdhci_mmc_slot *slot = sdhci_priv(host);
	unsigned int clkrate = slot->clkrate;

	DBG("EXIT: slot->clkrate = %u \n", clkrate);

	return clkrate;
}

static unsigned int get_f_max_clock (struct sdhci_host *host)
{
	struct sdhci_mmc_slot *slot = sdhci_priv(host);
	unsigned int f_max;

	f_max = slot->clkrate;
	
	DBG("EXIT: %s f_max = %u\n", mmc_hostname(host->mmc), f_max);

	return f_max;
}

static inline u16 pxa168_readw(struct sdhci_host *host, int reg)
{
	u32 temp;
	if (reg == SDHCI_HOST_VERSION) {
		temp = readl (host->ioaddr + SDHCI_HOST_VERSION - 2) >> 16;
		return temp & 0xffff;
	}
	
	return readw(host->ioaddr + reg);
}


static struct sdhci_ops sdhci_mmc_ops = {
	.sd_readw = &pxa168_readw,
	.init = &handle_shared_mmc_bus,
	.platform_specific_reset = &platform_specific_reset,
	.platform_specific_delay = &platform_specific_delay,
	.platform_specific_sdio = &platform_specific_sdio,
	.get_max_clock = &get_max_clock,
	.get_f_max_clock = &get_f_max_clock,
	.set_clock = &set_clock,
	.platform_supports_8_bit = &platform_supports_8_bit,
	.platform_set_8_bit = &platform_set_8_bit,
	.platform_clear_8_bit = &platform_clear_8_bit,
};


static void sdhci_mmc_remove_slot(struct sdhci_mmc_slot *slot)
{
	int dead;
	u32 scratch;

	DBG("ENTER %s\n", mmc_hostname(slot->host->mmc));
	dead = 0;
	scratch = readl(slot->host->ioaddr + SDHCI_INT_STATUS);
	if (scratch == (u32)-1)
		dead = 1;

	sdhci_remove_host(slot->host, dead);

	if (slot->chip->fixes && slot->chip->fixes->remove_slot)
		slot->chip->fixes->remove_slot(slot, dead);

	free_irq(slot->host->irq, slot->host);
	if (slot->host->ioaddr)
		iounmap(slot->host->ioaddr);
	release_mem_region(slot->chip->res->start, SZ_256);
	sdhci_free_host(slot->host);
}

static int pxa_sdh_probe(struct platform_device *pdev)
{
	struct sdhci_host *host = NULL;
	struct resource *r;
	int ret;
	int irq;
	struct sdhci_mmc_chip *chip;
	struct sdhci_mmc_slot *slot = NULL;

	DBG("ENTER\n");
	r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	irq = platform_get_irq(pdev, 0);
	DBG("platform_get_resource = %p\n", r);
	if (!r || irq < 0) {
		ret = -ENXIO;
		goto err;
	}
		
	chip = kzalloc(sizeof(struct sdhci_mmc_chip), GFP_KERNEL);
	if (!chip) {
		ret = -ENOMEM;
		goto err;
	}

	chip->quirks = SDHCI_QUIRK_32BIT_DMA_ADDR | SDHCI_QUIRK_32BIT_DMA_SIZE | 
		SDHCI_QUIRK_NO_BUSY_IRQ | SDHCI_QUIRK_PLATORM_RESET | SDHCI_QUIRK_USE_SUPPLIED_CLOCKS;

#if 1
#warning FORCE HIGHSPEED BIT LOW IN CONTROL REGISTER
	chip->quirks |= SDHCI_QUIRK_BROKEN_HOST_HIGHSPEED;
#endif

#if 1
#warning SET BROKEN_TIMEOUT_VAL
	chip->quirks |= SDHCI_QUIRK_BROKEN_TIMEOUT_VAL;
#endif
	r = request_mem_region(r->start, SZ_256, DRIVER_NAME);
	DBG("request_mem_region = %p\n", r);
	if (!r) {
		ret = -EBUSY;
		goto out;
	}

	host = sdhci_alloc_host(&pdev->dev, sizeof(struct sdhci_mmc_slot));
	if (IS_ERR(host)) {
		ret = PTR_ERR(host);
		goto out;
	}

	host->ioaddr = ioremap(r->start, SZ_256);
	if (!host->ioaddr) {
		ret = -ENOMEM;
		goto out;
	}

	host->irq = irq;
	host->quirks = chip->quirks;
	host->ops = &sdhci_mmc_ops;
	host->hw_name = "MMC";

	chip->pdev = pdev;
	chip->res = r;
	chip->pdata = pdev->dev.platform_data;
	platform_set_drvdata(pdev, chip);
	
	slot = sdhci_priv(host);
	chip->slots[0] = slot;
	chip->num_slots = 1;
	slot->chip = chip;
	slot->host = host;
	slot->no_dynamic_SD_clocking = DEFAULT_NO_DYNAMIC_CLOCKING;
	
	slot->eightBitEnabled = 0;
	slot->clockEnabled = 0;
	slot->clk = clk_get(&pdev->dev, "PXA-SDHCLK");
	if (slot->clk == NULL) {
		ret = -ENXIO;
		goto out;
	}
	slot->clkrate = clk_get_rate(slot->clk);

	if (chip->pdata)
		slot->width = chip->pdata->bus_width;
	else
		slot->width = 4;

	DBG("slot->width = %d\n", slot->width);

	enable_clock(host);
	
	platform_init_after_reset(host);

	ret = sdhci_add_host(host);
	if (ret)
		goto out;
	
	if(chip->pdata->mfp_config)	
		chip->pdata->mfp_config();

	DBG ("Exit %s\n", mmc_hostname(host->mmc));
	return 0;

 out:
 	DBG ("%s ERROR EXIT\n", __FUNCTION__);
	if (host) {
		if (host->ioaddr)
			iounmap(host->ioaddr);
			sdhci_free_host(host);
	}
	if (r)
		release_mem_region(r->start, SZ_256);
	
	platform_set_drvdata(pdev, NULL);
	if (slot && slot->clk)
	{
		if (slot->clockEnabled)
			clk_disable(slot->clk);
		slot->clockEnabled = 0;
	}
	kfree(chip);
	
err:
	return ret;
}

static int __devexit pxa_sdh_remove(struct platform_device *pdev)
{
	struct sdhci_mmc_chip *chip;
	int i;

	DBG ("ENTER");
	
	chip = platform_get_drvdata(pdev);
	if (chip) {
		for (i = 0;i < chip->num_slots; i++)
			sdhci_mmc_remove_slot(chip->slots[i]);
		platform_set_drvdata(pdev, NULL);
		clk_disable(chip->slots[i]->clk);
		kfree(chip);
	}
	return 0;
}

#ifdef CONFIG_PM
static int pxa_sdh_suspend(struct platform_device *dev, pm_message_t state)
{
	struct sdhci_mmc_chip *chip;
	struct sdhci_mmc_slot *slot;
	int i, ret;

	chip = platform_get_drvdata(dev);
	if (!chip)
		return 0;

	for (i = 0;i < chip->num_slots;i++) {
		slot = chip->slots[i];
		if (!slot)
			continue;

		ret = sdhci_suspend_host(slot->host, state);

		if (ret) {
			for (i--;i >= 0;i--)
				sdhci_resume_host(chip->slots[i]->host);
			return ret;
		}
	}

	if (chip->fixes && chip->fixes->suspend) {
		ret = chip->fixes->suspend(chip, state);
		if (ret) {
			for (i = chip->num_slots - 1;i >= 0;i--)
				sdhci_resume_host(chip->slots[i]->host);
			return ret;
		}
	}
	return 0;
}

static int pxa_sdh_resume(struct platform_device *dev)
{
	struct sdhci_mmc_chip *chip;
	struct sdhci_mmc_slot *slot;
	int i, ret;

	chip = platform_get_drvdata(dev);
	if (!chip)
		return 0;

	if (chip->fixes && chip->fixes->resume) {
		ret = chip->fixes->resume(chip);
		if (ret)
			return ret;
	}

	for (i = 0;i < chip->num_slots;i++) {
		slot = chip->slots[i];
		if (!slot)
			continue;

		ret = sdhci_resume_host(slot->host);
		if (ret)
			return ret;
	}

	return 0;
}
#else
#define pxa_sdh_suspend	NULL
#define pxa_sdh_resume	NULL
#endif

static struct platform_driver pxa_sdh_driver = {
	.probe		= pxa_sdh_probe,
	.remove		= pxa_sdh_remove,
	.suspend	= pxa_sdh_suspend,
	.resume		= pxa_sdh_resume,
	.driver		= {
		.name	= DRIVER_NAME,
	},
};

static int __init pxa_sdh_init(void)
{
	return platform_driver_register(&pxa_sdh_driver);
}

static void __exit pxa_sdh_exit(void)
{
	platform_driver_unregister(&pxa_sdh_driver);
}

module_init(pxa_sdh_init);
module_exit(pxa_sdh_exit);

MODULE_AUTHOR("Philip Rakity <prakity@marvell.com>");
MODULE_DESCRIPTION("PXA SD Host Controller(MMC) Interface");
MODULE_LICENSE("GPL");
