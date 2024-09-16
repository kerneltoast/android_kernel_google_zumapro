// SPDX-License-Identifier: GPL-2.0
/*
 * CP PMIC (Power Management IC) driver.
 *
 * Copyright (c) 2023, Google LLC. All rights reserved.
 */

#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/spmi.h>
#include "cp_pmic.h"

struct reg_entry {
	u32 reg;
	u32 val;
	u32 delay_ms;
};

struct pmic_reg_sequence {
	size_t num_entries;
	struct reg_entry *reg_entries;
};

struct pmic_info {
	struct spmi_device *sdev;
	struct regmap *regmap;
	struct pmic_reg_sequence warm_reset_seq;
};

static const struct regmap_range pmic_wr_range[] = {
	regmap_reg_range(0x0675, 0x067d),
};

static const struct regmap_access_table pmic_wr_table = {
	.yes_ranges = pmic_wr_range,
	.n_yes_ranges = ARRAY_SIZE(pmic_wr_range),
};

static const struct regmap_range pmic_rd_range[] = {
	regmap_reg_range(0x0675, 0x067d),
};

static const struct regmap_access_table pmic_rd_table = {
	.yes_ranges = pmic_rd_range,
	.n_yes_ranges = ARRAY_SIZE(pmic_rd_range),
};

static struct regmap_config pmic_regmap_config = {
	.name = "modem_pmic",
	.reg_bits = 16,
	.val_bits = 8,
	.val_format_endian = REGMAP_ENDIAN_NATIVE,
	.max_register = 0x67d,
	.wr_table = &pmic_wr_table,
	.rd_table = &pmic_rd_table,
};

static ssize_t pmic_read_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct pmic_info *info = dev_get_drvdata(dev);
	u32 reg, val;
	int ret;

	if (!info) {
		dev_err(dev, "pmic_info not available.\n");
		return -EINVAL;
	}

	if (sscanf(buf, "%x", &reg) != 1) {
		dev_err(dev, "Invalid format. Use '<reg_addr (hex)>.'\n");
		return -EINVAL;
	}

	ret = regmap_read(info->regmap, reg, &val);
	if (ret) {
		dev_err(dev, "Failed to read register 0x%08x: %d\n", reg, ret);
		return ret;
	}

	dev_info(dev, "Read PMIC register 0x%08x with value 0x%08x", reg, val);

	return count;
}
static DEVICE_ATTR_WO(pmic_read);

static ssize_t pmic_write_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct pmic_info *info = dev_get_drvdata(dev);
	u32 reg, val;
	int ret;

	if (!info) {
		dev_err(dev, "pmic_info not available.\n");
		return -EINVAL;
	}

	if (sscanf(buf, "%x %x", &reg, &val) != 2) {
		dev_err(dev, "Invalid format. Use '<reg_addr (hex)> <value (hex)>.'\n");
		return -EINVAL;
	}

	ret = regmap_write(info->regmap, reg, val);
	if (ret) {
		dev_err(dev, "Failed to write register 0x%08x: %d\n", reg, ret);
		return ret;
	}

	return count;
}
static DEVICE_ATTR_WO(pmic_write);

static const struct attribute *pmic_attrs[] = {
	&dev_attr_pmic_read.attr,
	&dev_attr_pmic_write.attr,
	NULL,
};

static const struct attribute_group pmic_attr_group = {
	.attrs = (struct attribute **)pmic_attrs,
	.name = "pmic",
};

void pmic_warm_reset_sequence(struct device *dev)
{
	struct pmic_info *info = dev_get_drvdata(dev);
	size_t i;
	struct pmic_reg_sequence *seq;
	struct reg_entry *entry;
	int ret;

	if (!info) {
		dev_info(dev, "pmic_info not available.\n");
		return;
	}

	seq = &info->warm_reset_seq;
	for (i = 0; i < seq->num_entries; i++) {
		entry = &seq->reg_entries[i];

		ret = regmap_write(info->regmap, entry->reg, entry->val);
		if (ret) {
			dev_info(dev, "Failed to write register 0x%x\n", entry->reg);
			return;
		}
		if (entry->delay_ms)
			msleep(entry->delay_ms);
	}

	dev_info(dev, "Warm reset sequence completed.\n");
}
EXPORT_SYMBOL_GPL(pmic_warm_reset_sequence);

static int pmic_dt_init(struct device *dev, struct pmic_info *info)
{
	struct property *prop;
	size_t num_entries, i;
	u32 *seq_data;

	if (!dev->of_node) {
		dev_err(dev, "of_node not found.\n");
		return -ENOENT;
	}

	prop = of_find_property(dev->of_node, "warm_reset_seq", NULL);
	if (!prop) {
		dev_err(dev, "Missing or invalid warm_reset_seq property in DT.\n");
		return -ENODATA;
	}

	// Check the number of entries in the "warm_reset_seq" property
	num_entries = prop->length / sizeof(struct reg_entry);
	info->warm_reset_seq.num_entries = num_entries;

	info->warm_reset_seq.reg_entries = devm_kmalloc(dev,
			num_entries * sizeof(struct reg_sequence), GFP_KERNEL);
	if (!info->warm_reset_seq.reg_entries)
		return -ENOMEM;

	// Access the data from the property and populate the reg_sequence structure
	seq_data = (u32 *)prop->value;
	for (i = 0; i < num_entries; i++) {
		info->warm_reset_seq.reg_entries[i].reg = be32_to_cpup(seq_data++);
		info->warm_reset_seq.reg_entries[i].val = be32_to_cpup(seq_data++);
		info->warm_reset_seq.reg_entries[i].delay_ms = be32_to_cpup(seq_data++);
	}

	return 0;
}

static int pmic_probe(struct spmi_device *sdev)
{
	struct device *dev = &sdev->dev;
	struct pmic_info *info;

	// Create pmic_info
	info = devm_kzalloc(dev, sizeof(struct pmic_info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	// Initialize pmic_info
	spmi_device_set_drvdata(sdev, info);
	info->sdev = sdev;

	// Initialize regmap for PMIC access
	info->regmap = devm_regmap_init_spmi_ext(sdev, &pmic_regmap_config);
	if (IS_ERR(&info->regmap)) {
		dev_err(&sdev->dev, "Failed to initialize PMIC regmap\n");
		return PTR_ERR(&info->regmap);
	}

	if (pmic_dt_init(dev, info))
		dev_err(dev, "Failed to initialize PMIC DT data\n");

	if (sysfs_create_group(&dev->kobj, &pmic_attr_group))
		dev_err(dev, "Failed to create PMIC sysfs group\n");

	return 0;
}

static const struct of_device_id pmic_of_match[] = {
	{ .compatible = "google,cp-pmic-spmi", },
	{},
};
MODULE_DEVICE_TABLE(of, pmic_of_match);

static struct spmi_driver pmic_driver = {
	.probe = pmic_probe,
	.driver = {
		.name = "cp_pmic_driver",
		.of_match_table = pmic_of_match,
	},
};

static int of_dev_node_match(struct device *dev, const void *node)
{
	return dev->of_node == node;
}

struct device *pmic_get_device(struct device_node *node)
{
	struct device *dev = NULL;
	struct bus_type *sbt = pmic_driver.driver.bus;

	if (sbt)
		dev = bus_find_device(sbt, NULL, node, of_dev_node_match);

	return dev;
}
EXPORT_SYMBOL_GPL(pmic_get_device);

struct spmi_device *pmic_get_spmi_device(struct device_node *node)
{
	struct device *dev;
	struct spmi_device *sdev = NULL;

	dev = pmic_get_device(node);
	if (dev)
		sdev = to_spmi_device(dev);
	return sdev;
}
EXPORT_SYMBOL_GPL(pmic_get_spmi_device);

static int __init pmic_init(void)
{
	spmi_driver_register(&pmic_driver);
	return 0;
}
module_init(pmic_init);

static void __exit pmic_exit(void)
{
	spmi_driver_unregister(&pmic_driver);
}
module_exit(pmic_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Google CP PMIC Driver");
MODULE_AUTHOR("Salmax Chang <salmaxchang@google.com>");

