/*
 * Copyright (C) 2015 Hisilicon Limited, All Rights Reserved.
 * Author: Jun Ma <majun258@huawei.com>
 * Author: Yun Wu <wuyun.wu@huawei.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/acpi.h>
#include <linux/interrupt.h>
#include <linux/iort.h>
#include <linux/irqchip.h>
#include <linux/module.h>
#include <linux/msi.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

/* The maximum IRQ pin number of mbigen chip(start from 0) */
#define MAXIMUM_IRQ_PIN_NUM		640

/**
 * In mbigen vector register
 * bit[31:16]:	device id
 * bit[15:0]:	event id value
 */
#define IRQ_DEVICE_ID_SHIFT		16
#define IRQ_EVENT_ID_MASK		0xffff

/* offset of vector register in mbigen node */
#define REG_MBIGEN_VEC_OFFSET		0x300
#define REG_MBIGEN_EXT_VEC_OFFSET		0x320

/**
 * offset of clear register in mbigen node
 * This register is used to clear the status
 * of interrupt
 */
#define REG_MBIGEN_CLEAR_OFFSET		0x100

/**
 * offset of interrupt type register
 * This register is used to configure interrupt
 * trigger type
 */
#define REG_MBIGEN_TYPE_OFFSET		0x0

/**
 * struct mbigen_device - holds the information of mbigen device.
 *
 * @pdev:		pointer to the platform device structure of mbigen chip.
 * @base:		mapped address of this mbigen chip.
 * @dev_id:		device id of this mbigen device.
 */
struct mbigen_device {
	struct platform_device	*pdev;
	void __iomem		*base;
	unsigned int		dev_id;
};

/**
 * define the event ID start value of each mbigen node
 * in a mbigen chip
 */
static int mbigen_event_base[6] = {0, 64, 128, 192, 256, 384};

static int get_mbigen_nid(unsigned int offset)
{
	int nid = 0;

	if (offset < 256)
		nid = offset / 64;
	else if (offset < 384)
		nid = 4;
	else if (offset < 640)
		nid = 5;

	return nid;
}

static inline unsigned int get_mbigen_vec_reg(irq_hw_number_t hwirq)
{
	unsigned int nid;

	nid = get_mbigen_nid(hwirq);

	if (nid < 4)
		return (nid * 4) + REG_MBIGEN_VEC_OFFSET;
	else
		return (nid - 4) * 4 + REG_MBIGEN_EXT_VEC_OFFSET;
}

static inline void get_mbigen_type_reg(irq_hw_number_t hwirq,
					u32 *mask, u32 *addr)
{
	int ofst;

	ofst = hwirq / 32 * 4;
	*mask = 1 << (hwirq % 32);

	*addr = ofst + REG_MBIGEN_TYPE_OFFSET;
}

static inline void get_mbigen_clear_reg(irq_hw_number_t hwirq,
					u32 *mask, u32 *addr)
{
	unsigned int ofst;

	ofst = hwirq / 32 * 4;

	*mask = 1 << (hwirq % 32);
	*addr = ofst + REG_MBIGEN_CLEAR_OFFSET;
}

static void mbigen_eoi_irq(struct irq_data *data)
{
	void __iomem *base = data->chip_data;
	u32 mask, addr;

	get_mbigen_clear_reg(data->hwirq, &mask, &addr);

	writel_relaxed(mask, base + addr);

	irq_chip_eoi_parent(data);
}

static int mbigen_set_type(struct irq_data *data, unsigned int type)
{
	void __iomem *base = data->chip_data;
	u32 mask, addr, val;

	if (type != IRQ_TYPE_LEVEL_HIGH && type != IRQ_TYPE_EDGE_RISING)
		return -EINVAL;

	get_mbigen_type_reg(data->hwirq, &mask, &addr);

	val = readl_relaxed(base + addr);

	if (type == IRQ_TYPE_LEVEL_HIGH)
		val |= mask;
	else
		val &= ~mask;

	writel_relaxed(val, base + addr);

	return 0;
}

static struct irq_chip mbigen_irq_chip = {
	.name =			"mbigen-v1",
	.irq_mask =		irq_chip_mask_parent,
	.irq_unmask =		irq_chip_unmask_parent,
	.irq_eoi =		mbigen_eoi_irq,
	.irq_set_type =		mbigen_set_type,
	.irq_set_affinity =	irq_chip_set_affinity_parent,
};

static void mbigen_write_msg(struct msi_desc *desc, struct msi_msg *msg)
{
	struct irq_data *d = irq_get_irq_data(desc->irq);
	void __iomem *base = d->chip_data;
	struct mbigen_device *mgn_chip;
	u32 newval, oldval, nid;

	mgn_chip = platform_msi_get_host_data(d->domain);

	nid = get_mbigen_nid(d->hwirq);

	base += get_mbigen_vec_reg(d->hwirq);

	newval = (mgn_chip->dev_id << IRQ_DEVICE_ID_SHIFT) | mbigen_event_base[nid];
	oldval = readl_relaxed(base);

	if (newval != oldval)
		writel_relaxed(newval, base);
}

static int mbigen_domain_translate(struct irq_domain *d,
				    struct irq_fwspec *fwspec,
				    unsigned long *hwirq,
				    unsigned int *type)
{
	if (is_of_node(fwspec->fwnode) || is_acpi_device_node(fwspec->fwnode)) {
		if (fwspec->param_count != 2)
			return -EINVAL;

		if (fwspec->param[0] > MAXIMUM_IRQ_PIN_NUM)
			return -EINVAL;

		*hwirq = fwspec->param[0];

		/* If there is no valid irq type, just use the default type */
		if ((fwspec->param[1] == IRQ_TYPE_EDGE_RISING) ||
			(fwspec->param[1] == IRQ_TYPE_LEVEL_HIGH))
			*type = fwspec->param[1];
		else
			return -EINVAL;

		return 0;
	}
	return -EINVAL;
}

static int mbigen_irq_domain_alloc(struct irq_domain *domain,
					unsigned int virq,
					unsigned int nr_irqs,
					void *args)
{
	struct irq_fwspec *fwspec = args;
	irq_hw_number_t hwirq;
	unsigned int type;
	struct mbigen_device *mgn_chip;
	int i, err;

	err = mbigen_domain_translate(domain, fwspec, &hwirq, &type);
	if (err)
		return err;


	mgn_chip = platform_msi_get_host_data(domain);

	/* In order to carry hwirq information into parent
	* domain alloc function, we set hwirq and chip info
	* before platform_msi_domain_alloc is called
	*/
	for (i = 0; i < nr_irqs; i++)
		irq_domain_set_hwirq_and_chip(domain, virq + i, hwirq + i,
				      &mbigen_irq_chip, mgn_chip->base);

	err = platform_msi_domain_alloc(domain, virq, nr_irqs);
	if (err)
		return err;

	return 0;
}

static struct irq_domain_ops mbigen_domain_ops = {
	.translate	= mbigen_domain_translate,
	.alloc		= mbigen_irq_domain_alloc,
	.free		= irq_domain_free_irqs_common,
};

static int mbigen_device_probe(struct platform_device *pdev)
{
	struct mbigen_device *mgn_chip;
	struct resource *res;
	struct device *dev = &pdev->dev;
	struct irq_domain *domain;
	u32 num_pins, dev_id;
	int err = 0;

	mgn_chip = devm_kzalloc(dev, sizeof(*mgn_chip), GFP_KERNEL);
	if (!mgn_chip)
		return -ENOMEM;

	mgn_chip->pdev = pdev;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	mgn_chip->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(mgn_chip->base))
		return PTR_ERR(mgn_chip->base);

	if (device_property_read_u32(dev, "num-pins", &num_pins) < 0) {
		dev_err(dev, "No num-pins property\n");
		return -EINVAL;
	}

	if (dev->of_node)
		err = of_property_read_u32_index(dev->of_node, "msi-parent",
					 1, &dev_id);
	else if (has_acpi_companion(dev))
		/*
		 * Get dev_id from IORT table which represent the
		 * mapping of MBI-GEN to ITS
		 */
		err = iort_find_platform_dev_id(dev, &dev_id);
	else
		err = -EINVAL;

	if (err)
		return err;

	mgn_chip->dev_id = dev_id;
	domain = platform_msi_create_device_domain(dev, num_pins,
						   mbigen_write_msg,
						   &mbigen_domain_ops,
						   mgn_chip);
	if (!domain)
		return -ENOMEM;

	platform_set_drvdata(pdev, mgn_chip);
	dev_info(dev, "Allocated %d MSIs\n", num_pins);
	return 0;
}

static const struct of_device_id mbigen_of_match[] = {
	{ .compatible = "hisilicon,mbigen-v1" },
	{ /* END */ }
};
MODULE_DEVICE_TABLE(of, mbigen_of_match);

static const struct acpi_device_id mbigen_acpi_match[] = {
        { "HISI0151", 0 },
	{}
};
MODULE_DEVICE_TABLE(acpi, mbigen_acpi_match);

static struct platform_driver mbigen_platform_driver = {
	.driver = {
		.name		= "Hisilicon MBIGEN-V1",
		.owner		= THIS_MODULE,
		.of_match_table	= mbigen_of_match,
		.acpi_match_table = ACPI_PTR(mbigen_acpi_match),
	},
	.probe			= mbigen_device_probe,
};

static int __init mbigen_device_init(void)
{
	return platform_driver_register(&mbigen_platform_driver);
}
arch_initcall(mbigen_device_init)

MODULE_AUTHOR("Jun Ma <majun258@huawei.com>");
MODULE_AUTHOR("Yun Wu <wuyun.wu@huawei.com>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Hisilicon MBI Generator driver");
