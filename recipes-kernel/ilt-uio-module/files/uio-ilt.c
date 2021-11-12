/*
 * demo driver for aries interrupt latency timer IP core (uio_ilt)
 *
 * This driver is derived from the Microchip MSS DMA driver for
 * PolarFire SoCs and used as an example in a webinar session.
 *
 * Copyright (C) 2018-19 Microchip Incorporated - http://www.microchip.com/
 *
 * Modifications for ILT
 * Copyright (C) 2021    Aries Embedded GmbH - http://www.microchip.com/
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation version 2.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/device.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <linux/uio_driver.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/genalloc.h>

#define DRV_NAME "uio-ilt"
#define DRV_VERSION "0.1"

// offset and bit definitions of interrupt ack and status register
// (which is the only register and bits we need to access)
#define ILT_INT_ASR_OFFSET  0x20
#define ILT_INT_ACK0 (1<<0)
#define ILT_ACK0_WAIT (1<<24)

/* driver info struct */
struct uio_ilt_dev {
	struct uio_info *info;
    void __iomem *ilt_base; // holds mmaped pointer to ILT register base
};


/* simple interrupt handler
 *
 * acknowledges interrupt in ILTs interrupt acknowledge/status
 * register and returns IRQ_HANDLED do userspace application
 * is signalled about the interrupt (read returns)
 * the rest is up to the userspace
 * */
static irqreturn_t uio_ilt_handler(int irq, struct uio_info *info)
{
	struct uio_ilt_dev *dev_info = info->priv;

    // create a pointer to our status register
    void __iomem *base = dev_info->ilt_base;
	void __iomem *int_ack_sr = base + (ILT_INT_ASR_OFFSET);

    // see if ILT is waiting for an ACK (double check interrupt)
    if((ioread32(int_ack_sr) & ILT_ACK0_WAIT ) == ILT_ACK0_WAIT)
	{
        // acknowledge that the kernel driver has received the interrupt
        iowrite32(ILT_INT_ACK0, int_ack_sr);
		return IRQ_HANDLED;
	}

	return IRQ_NONE;
}


/* driver unload cleanup */
static void uio_ilt_cleanup(struct device *dev,
		struct uio_ilt_dev *dev_info)
{
	int cnt;
	struct uio_info *p = dev_info->info;

    uio_unregister_device(p);
    kfree(p->name);

    iounmap(dev_info->ilt_base);

    kfree(dev_info->info);
	kfree(dev_info);
}


/* driver probe function */
static int uio_ilt_probe(struct platform_device *pdev)
{
	struct uio_info *p;
	struct uio_ilt_dev *dev_info;
	struct resource *r;
	struct device *dev = &pdev->dev;
	struct uio_mem *uiomem;
	int ret = -ENODEV, cnt = 0, len, i;
	/* struct uio_ilt_pdata *pdata = dev_get_platdata(dev); TODO */

	dev_info(dev, "Running Probe\n");

	dev_info = kzalloc(sizeof(struct uio_ilt_dev), GFP_KERNEL);
	if (!dev_info)
		return -ENOMEM;

	dev_info->info = kzalloc(sizeof(*p), GFP_KERNEL);
	if (!dev_info->info) {
		kfree(dev_info);
		return -ENOMEM;
	}

	r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!r) {
		dev_err(dev, "No ILT memory resource specified\n");
		goto out_free;
	}

	if (!r->start) {
		dev_err(dev, "Invalid memory resource\n");
		goto out_free;
	}

	p = dev_info->info;


	uiomem = &p->mem[0];

	for (i = 0; i < pdev->num_resources; ++i) {
		struct resource *r = &pdev->resource[i];

		if (r->flags != IORESOURCE_MEM)
			continue;

		if (uiomem >= &p->mem[MAX_UIO_MAPS]) {
			dev_warn(&pdev->dev, "device has more than "
					__stringify(MAX_UIO_MAPS)
					" I/O memory resources.\n");
			break;
		}

		uiomem->memtype = UIO_MEM_PHYS;
		uiomem->addr = r->start & PAGE_MASK;
		uiomem->offs = r->start & ~PAGE_MASK;
		uiomem->size = (uiomem->offs + resource_size(r)
				+ PAGE_SIZE - 1) & PAGE_MASK;
		uiomem->name = r->name;
		++uiomem;
	}

	while (uiomem < &p->mem[MAX_UIO_MAPS]) {
		uiomem->size = 0;
		++uiomem;
	}

    dev_info->ilt_base = ioremap( p->mem[0].addr, p->mem[0].size);

    p->name = kasprintf(GFP_KERNEL, "aries_ilt%d", cnt);
	p->version = DRV_VERSION;

	/* Register ILT IRQ line */
	p->irq = platform_get_irq(pdev, 0);
	p->irq_flags = IRQF_SHARED;
	p->handler = uio_ilt_handler;
	p->priv = dev_info;

	ret = uio_register_device(dev, p);
	if (ret < 0)
		goto out_free;

	platform_set_drvdata(pdev, dev_info);
	return 0;

out_free:
	uio_ilt_cleanup(dev, dev_info);
	return ret;
}

static int uio_ilt_remove(struct platform_device *dev)
{
	struct uio_ilt_dev *dev_info = platform_get_drvdata(dev);

	uio_ilt_cleanup(&dev->dev, dev_info);
	return 0;
}

static int uio_ilt_runtime_nop(struct device *dev)
{
	/* Runtime PM callback shared between ->runtime_suspend()
	 * and ->runtime_resume(). Simply returns success.
	 *
	 * In this driver pm_runtime_get_sync() and pm_runtime_put_sync()
	 * are used at open() and release() time. This allows the
	 * Runtime PM code to turn off power to the device while the
	 * device is unused, ie before open() and after release().
	 *
	 * This Runtime PM callback does not need to save or restore
	 * any registers since user space is responsbile for hardware
	 * register reinitialization after open().
	 */
	return 0;
}

static const struct dev_pm_ops uio_ilt_dev_pm_ops = {
	.runtime_suspend = uio_ilt_runtime_nop,
	.runtime_resume = uio_ilt_runtime_nop,
};


#if defined(CONFIG_OF)
static const struct of_device_id uio_ilt_dt_ids[] = {
	{ .compatible = "aries,aries-ilt" },
	{ /*sentinel */ }
};
#endif

static struct platform_driver uio_ilt_driver = {
	.probe = uio_ilt_probe,
	.remove = uio_ilt_remove,
	.driver = {
		.name = DRV_NAME,
		.pm = &uio_ilt_dev_pm_ops,
		.of_match_table = of_match_ptr(uio_ilt_dt_ids),
		.owner = THIS_MODULE,
    },
};

module_platform_driver(uio_ilt_driver);

MODULE_LICENSE("GPL v2");
MODULE_VERSION(DRV_VERSION);
MODULE_DESCRIPTION("Userspace I/O platform driver for demo ILT IP core");
MODULE_AUTHOR("Moritz von Dawans <mvd@aries-embedded.de");
MODULE_ALIAS("platform:" DRV_NAME);

