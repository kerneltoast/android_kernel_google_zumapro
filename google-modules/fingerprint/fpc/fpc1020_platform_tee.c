/*
 * FPC1020 Fingerprint sensor device driver
 *
 * This driver will control the platform resources that the FPC fingerprint
 * sensor needs to operate. The major things are probing the sensor to check
 * that it is actually connected and let the Kernel know this and with that also
 * enabling and disabling of regulators, controlling GPIOs such as sensor reset
 * line, sensor IRQ line.
 *
 * The driver will expose most of its available functionality in sysfs which
 * enables dynamic control of these features from eg. a user space process.
 *
 * The sensor's IRQ events will be pushed to Kernel's event handling system and
 * are exposed in the drivers event node.
 *
 * This driver will NOT send any commands to the sensor it only controls the
 * electrical parts.
 *
 *
 * Copyright (c) 2015 Fingerprint Cards AB <tech@fingerprints.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License Version 2
 * as published by the Free Software Foundation.
 */
#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>

#define FPC_TTW_HOLD_TIME 3000

#define RESET_LOW_SLEEP_MIN_US 5000
#define RESET_LOW_SLEEP_MAX_US (RESET_LOW_SLEEP_MIN_US + 100)
#define RESET_HIGH_SLEEP1_MIN_US 100
#define RESET_HIGH_SLEEP1_MAX_US (RESET_HIGH_SLEEP1_MIN_US + 100)
#define RESET_HIGH_SLEEP2_MIN_US 5000
#define RESET_HIGH_SLEEP2_MAX_US (RESET_HIGH_SLEEP2_MIN_US + 100)
#define PWR_ON_SLEEP_MIN_US 100
#define PWR_ON_SLEEP_MAX_US (PWR_ON_SLEEP_MIN_US + 900)

#define NUM_PARAMS_REG_ENABLE_SET 2

const char *rst_gpio_power_down_state = "rst,power_down";

struct vreg_config {
	char *name;
	unsigned long vmin;
	unsigned long vmax;
	int ua_load;
};

static const struct vreg_config vreg_conf[] = {
	{ "vdd_ana", 1800000UL, 1800000UL, 6000, },
	{ "vcc_spi", 1800000UL, 1800000UL, 10, },
	{ "vdd_io", 1800000UL, 1800000UL, 6000, },
};

struct fpc1020_data {
	struct device *dev;

	struct regulator *vreg[ARRAY_SIZE(vreg_conf)];

	struct wakeup_source *ttw_ws;
	int irq_gpio;
	int rst_gpio;
	struct mutex lock; /* To set/get exported values in sysfs */
	bool prepared;
	atomic_t wakeup_enabled; /* Used both in ISR and non-ISR */
	struct pinctrl *rst_pinctrl;
	struct pinctrl_state *rst_state;
	atomic_t irq_enabled;
};

static int vreg_setup(struct fpc1020_data *fpc1020, const char *name,
	bool enable)
{
	size_t i;
	int rc;
	struct regulator *vreg;
	struct device *dev = fpc1020->dev;

	for (i = 0; i < ARRAY_SIZE(fpc1020->vreg); i++) {
		const char *n = vreg_conf[i].name;

		if (!strncmp(n, name, strlen(n)))
			goto found;
	}

	dev_err(dev, "Regulator %s not found\n", name);

	return -EINVAL;

found:
	vreg = fpc1020->vreg[i];
	if (enable) {
		if (!vreg) {
			vreg = regulator_get(dev, name);
			if (IS_ERR(vreg)) {
				dev_err(dev, "Unable to get %s\n", name);
				return PTR_ERR(vreg);
			}
		}
/*
		if (regulator_count_voltages(vreg) > 0) {
			rc = regulator_set_voltage(vreg, vreg_conf[i].vmin,
					vreg_conf[i].vmax);
			if (rc)
				dev_err(dev,
					"Unable to set voltage on %s, %d\n",
					name, rc);
		}
*/
		rc = regulator_enable(vreg);
		if (rc) {
			dev_err(dev, "error enabling %s: %d\n", name, rc);
			regulator_put(vreg);
			vreg = NULL;
		}
		fpc1020->vreg[i] = vreg;
	} else {
		if (vreg) {
			if (regulator_is_enabled(vreg)) {
				regulator_disable(vreg);
				dev_dbg(dev, "disabled %s\n", name);
			}
			regulator_put(vreg);
			fpc1020->vreg[i] = NULL;
		}
		rc = 0;
	}

	return rc;
}

/**
 * clk_enable_set() - sysfs node for controlling clocks.
 *
 * This is disabled in platform variant of this driver but kept for
 * backwards compatibility. Only prints a debug print that it is
 * disabled.
 *
 * @dev: fp device structure
 * @attr: device attribute
 * @buf: buffer that being passed to this driver
 * @count: count
 */
static ssize_t clk_enable_set(struct device *dev,
	struct device_attribute *attr,
	const char *buf, size_t count)
{
	dev_dbg(dev,
		"clk_enable sysfs node not enabled in platform driver\n");

	return count;
}
static DEVICE_ATTR(clk_enable, 0200, NULL, clk_enable_set);

static ssize_t regulator_enable_set(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct fpc1020_data *fpc1020 = dev_get_drvdata(dev);
	char op;
	char name[16];
	int rc;
	bool enable;

	if (NUM_PARAMS_REG_ENABLE_SET != sscanf(buf, "%15[^,],%c", name, &op))
		return -EINVAL;
	if (op == 'e')
		enable = true;
	else if (op == 'd')
		enable = false;
	else
		return -EINVAL;

	mutex_lock(&fpc1020->lock);
	rc = vreg_setup(fpc1020, name, enable);
	mutex_unlock(&fpc1020->lock);

	return rc ? rc : count;
}
static DEVICE_ATTR(regulator_enable, 0200, NULL, regulator_enable_set);

static int hw_reset(struct fpc1020_data *fpc1020)
{
	int irq_gpio;
    int rst_gpio;
	struct device *dev = fpc1020->dev;

	// Configure reset pin as High
	gpio_set_value(fpc1020->rst_gpio, 1);
	usleep_range(RESET_HIGH_SLEEP1_MIN_US, RESET_HIGH_SLEEP1_MAX_US);
	rst_gpio = gpio_get_value(fpc1020->rst_gpio);
	dev_info(dev, "RST after reset_active %d\n",rst_gpio);

	// Configure reset pin as low
	gpio_set_value(fpc1020->rst_gpio, 0);
	usleep_range(RESET_LOW_SLEEP_MIN_US, RESET_LOW_SLEEP_MAX_US);
	rst_gpio = gpio_get_value(fpc1020->rst_gpio);
	dev_info(dev, "RST after reset_reset %d\n",rst_gpio);

	// Configure reset pin as High
	gpio_set_value(fpc1020->rst_gpio, 1);
	usleep_range(RESET_HIGH_SLEEP2_MIN_US, RESET_HIGH_SLEEP2_MAX_US);
	rst_gpio = gpio_get_value(fpc1020->rst_gpio);
	dev_info(dev, "RST after reset_active %d\n",rst_gpio);

	// Check interrupt pin after HW reset
	irq_gpio = gpio_get_value(fpc1020->irq_gpio);
	dev_info(dev, "IRQ after reset %d\n", irq_gpio);

	return 0;
}

static ssize_t hw_reset_set(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	int rc;
	struct fpc1020_data *fpc1020 = dev_get_drvdata(dev);

	if (!strncmp(buf, "reset", strlen("reset"))) {
		mutex_lock(&fpc1020->lock);
		rc = hw_reset(fpc1020);
		mutex_unlock(&fpc1020->lock);
	} else {
		return -EINVAL;
	}

	return rc ? rc : count;
}
static DEVICE_ATTR(hw_reset, 0200, NULL, hw_reset_set);

/**
 * device_prepare() - Will setup GPIOs, and regulators to correctly initialize
 * the touch sensor to be ready for work.
 *
 * In the correct order according to the sensor spec this function will
 * enable/disable regulators, and reset line, all to set the sensor in a
 * correct power on or off state "electrical" wise.
 *
 * @fpc1020: Data structure of fpc1020 driver
 * @enable: Enable/disable regulators
 * @see  device_prepare_set
 * @note This function will not send any commands to the sensor it will only
 *       control it "electrically".
 */
static int device_prepare(struct fpc1020_data *fpc1020, bool enable)
{
	int rc;

	mutex_lock(&fpc1020->lock);
	if (enable && !fpc1020->prepared) {
		fpc1020->prepared = true;
		gpio_direction_output(fpc1020->rst_gpio, 0);

		rc = vreg_setup(fpc1020, "vcc_spi", true);
		if (rc)
			goto exit;

		rc = vreg_setup(fpc1020, "vdd_io", true);
		if (rc)
			goto exit_1;

		rc = vreg_setup(fpc1020, "vdd_ana", true);
		if (rc)
			goto exit_2;

		usleep_range(PWR_ON_SLEEP_MIN_US, PWR_ON_SLEEP_MAX_US);

		/* As we can't control chip select here the other part of the
		 * sensor driver eg. the TEE driver needs to do a _SOFT_ reset
		 * on the sensor after power up to be sure that the sensor is
		 * in a good state after power up. Okeyed by ASIC. */

		gpio_direction_output(fpc1020->rst_gpio, 1);
	} else if (!enable && fpc1020->prepared) {
		rc = 0;
		gpio_direction_output(fpc1020->rst_gpio, 0);

		usleep_range(PWR_ON_SLEEP_MIN_US, PWR_ON_SLEEP_MAX_US);

		(void)vreg_setup(fpc1020, "vdd_ana", false);
exit_2:
		(void)vreg_setup(fpc1020, "vdd_io", false);
exit_1:
		(void)vreg_setup(fpc1020, "vcc_spi", false);
exit:
		fpc1020->prepared = false;
	} else {
		rc = 0;
	}
	mutex_unlock(&fpc1020->lock);

	return rc;
}

/**
 * device_prepare_set() - sysfs node to enable/disable (power up/power down)
 * the touch sensor
 *
 * @see device_prepare
 * @dev: fp device structure
 * @attr: device attribute
 * @buf: buffer that being passed to this driver
 * @count: count
 */
static ssize_t device_prepare_set(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	int rc;
	struct fpc1020_data *fpc1020 = dev_get_drvdata(dev);

	if (!strncmp(buf, "enable", strlen("enable")))
		rc = device_prepare(fpc1020, true);
	else if (!strncmp(buf, "disable", strlen("disable")))
		rc = device_prepare(fpc1020, false);
	else
		return -EINVAL;

	return rc ? rc : count;
}
static DEVICE_ATTR(device_prepare, 0200, NULL, device_prepare_set);

/**
 * wakeup_enable_set() - sysfs node for controlling whether the driver is
 * allowed to wake up the platform on interrupt.
 *
 * @dev: fp device structure
 * @attr: device attribute
 * @buf: buffer that being passed to this driver
 * @count: count
 */

static ssize_t wakeup_enable_set(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct fpc1020_data *fpc1020 = dev_get_drvdata(dev);
	ssize_t ret = count;

	mutex_lock(&fpc1020->lock);
	if (!strncmp(buf, "enable", strlen("enable")))
		atomic_set(&fpc1020->wakeup_enabled, 1);
	else if (!strncmp(buf, "disable", strlen("disable")))
		atomic_set(&fpc1020->wakeup_enabled, 0);
	else
		ret = -EINVAL;
	mutex_unlock(&fpc1020->lock);

	return ret;
}
static DEVICE_ATTR(wakeup_enable, 0200, NULL, wakeup_enable_set);

/**
 * irq_get() - sysf node to check the interrupt status of the sensor, the
 * interrupt handler should perform sysf_notify to allow userland to poll
 * the node.
 *
 * @dev: fp device structure
 * @attr: device attribute
 * @buf: buffer that being passed to this driver
 */

static ssize_t irq_get(struct device *dev,
	struct device_attribute *attr,
	char *buf)
{
	struct fpc1020_data *fpc1020 = dev_get_drvdata(dev);
	int irq = gpio_get_value(fpc1020->irq_gpio);

	return scnprintf(buf, PAGE_SIZE, "%i\n", irq);
}

/**
 * irq_ack() - writing to the irq node will just drop a printk message
 * and return success, used for latency measurement.
 *
 * @dev: fp device structure
 * @attr: device attribute
 * @buf: buffer that being passed to this driver
 * @count: count
 */

static ssize_t irq_ack(struct device *dev,
	struct device_attribute *attr,
	const char *buf, size_t count)
{
	struct fpc1020_data *fpc1020 = dev_get_drvdata(dev);

	dev_dbg(fpc1020->dev, "%s\n", __func__);

	return count;
}
static DEVICE_ATTR(irq, 0600, irq_get, irq_ack);

/**
 * irq_enable_set() - sysfs node for controlling whether the driver is allowed
 * to notify HAL of finger down event
 *
 * @dev: fp device structure
 * @attr: device attribute
 * @buf: buffer that being passed to this driver
 * @count: count
 */

static ssize_t irq_enable_set(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct fpc1020_data *fpc1020 = dev_get_drvdata(dev);
	ssize_t ret = count;

	mutex_lock(&fpc1020->lock);
	if (!strncmp(buf, "enable", strlen("enable")))
		atomic_set(&fpc1020->irq_enabled, 1);
	else if (!strncmp(buf, "disable", strlen("disable")))
		atomic_set(&fpc1020->irq_enabled, 0);
	else
		ret = -EINVAL;
	mutex_unlock(&fpc1020->lock);

	return ret;
}
static DEVICE_ATTR(irq_enable, 0200, NULL, irq_enable_set);

static struct attribute *attributes[] = {
	&dev_attr_device_prepare.attr,
	&dev_attr_regulator_enable.attr,
	&dev_attr_hw_reset.attr,
	&dev_attr_wakeup_enable.attr,
	&dev_attr_clk_enable.attr,
	&dev_attr_irq.attr,
	&dev_attr_irq_enable.attr,
	NULL
};

static const struct attribute_group attribute_group = {
	.attrs = attributes,
};

static irqreturn_t fpc1020_irq_handler(int irq, void *handle)
{
	struct fpc1020_data *fpc1020 = handle;

	dev_dbg(fpc1020->dev, "%s\n", __func__);

	if (atomic_read(&fpc1020->wakeup_enabled)) {
		__pm_wakeup_event(fpc1020->ttw_ws, FPC_TTW_HOLD_TIME);
	}

	if (atomic_read(&fpc1020->irq_enabled)) {
	    sysfs_notify(&fpc1020->dev->kobj, NULL, dev_attr_irq.attr.name);
    }

	return IRQ_HANDLED;
}

static int fpc1020_request_named_gpio(struct fpc1020_data *fpc1020,
	const char *label, int *gpio)
{
	struct device *dev = fpc1020->dev;
	struct device_node *np = dev->of_node;
	int rc = of_get_named_gpio(np, label, 0);

	if (rc < 0) {
		dev_err(dev, "failed to get '%s'\n", label);
		return rc;
	}
	*gpio = rc;

	rc = devm_gpio_request(dev, *gpio, label);
	if (rc) {
		dev_err(dev, "failed to request gpio %d\n", *gpio);
		return rc;
	}
	dev_dbg(dev, "%s %d\n", label, *gpio);

	return 0;
}

static int fpc1020_config_gpio(struct fpc1020_data *fpc1020)
{
	struct device *dev = fpc1020->dev;
	int ret = 0;
	// Configurate IRQ pin
	ret = gpio_direction_input(fpc1020->irq_gpio);
	if(ret) {
	    dev_err(dev, "Failed to set irq GPIO\n");
	    return ret;
	}
	// Configurate Reset pin
	ret = gpio_direction_output(fpc1020->rst_gpio, 1);
	if(ret) {
	    dev_err(dev, "Failed to set reset pin high\n");
	    return ret;
	}

	return 0;

}

static int config_rst_power_down(struct fpc1020_data *fpc_data) {

	struct device *dev = fpc_data->dev;
	int result = 0;

	fpc_data->rst_pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(fpc_data->rst_pinctrl)) {
         dev_err(dev, "%s: Can't get pinctrl\n",__func__);
		 return -1;
	}

	fpc_data->rst_state = pinctrl_lookup_state(fpc_data->rst_pinctrl, rst_gpio_power_down_state);
	if (IS_ERR_OR_NULL(fpc_data->rst_state)) {
		dev_err(dev, "%s: Failed to lookup reset lower power state\n",__func__);
		return -1;
	}

	result = pinctrl_select_state(fpc_data->rst_pinctrl, fpc_data->rst_state);
	if (result) {
		dev_err(dev, "%s: Can not set %s state\n",__func__, rst_gpio_power_down_state);
		return -1;
	}

	dev_info(dev, "%s: finish\n",__func__);
	return 0;
}


static int fpc1020_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int rc = 0;
	int irqf;
	struct device_node *np = dev->of_node;
	struct fpc1020_data *fpc1020 = devm_kzalloc(dev, sizeof(*fpc1020),
			GFP_KERNEL);

	if (!fpc1020) {
		dev_err(dev,
			"failed to allocate memory for struct fpc1020_data\n");
		rc = -ENOMEM;
		goto exit;
	}

	fpc1020->dev = dev;
	platform_set_drvdata(pdev, fpc1020);

	if (!np) {
		dev_err(dev, "no of node found\n");
		rc = -EINVAL;
		goto exit;
	}

	// Config reset lower power state
	rc = config_rst_power_down(fpc1020);
	if (rc)
		goto exit;

	// Request IRQ and Reset pin GPIO
	rc = fpc1020_request_named_gpio(fpc1020, "fpc,gpio_irq",
			&fpc1020->irq_gpio);
	if (rc)
		goto exit;
	rc = fpc1020_request_named_gpio(fpc1020, "fpc,gpio_rst",
			&fpc1020->rst_gpio);
	if (rc)
		goto exit;

	// Configure GPIO pin direction
	rc = fpc1020_config_gpio(fpc1020);
	if (rc)
		goto exit;

	atomic_set(&fpc1020->wakeup_enabled, 0);

	irqf = IRQF_TRIGGER_RISING | IRQF_ONESHOT;
	if (of_property_read_bool(dev->of_node, "fpc,enable-wakeup")) {
		irqf |= IRQF_NO_SUSPEND;
		device_init_wakeup(dev, 1);
	}

	mutex_init(&fpc1020->lock);
	rc = devm_request_threaded_irq(dev, gpio_to_irq(fpc1020->irq_gpio),
			NULL, fpc1020_irq_handler, irqf,
			dev_name(dev), fpc1020);
	if (rc) {
		dev_err(dev, "could not request irq %d\n",
				gpio_to_irq(fpc1020->irq_gpio));
		goto exit;
	}

	atomic_set(&fpc1020->irq_enabled, 1);

	dev_dbg(dev, "requested irq %d\n", gpio_to_irq(fpc1020->irq_gpio));

	/* Request that the interrupt should be wakeable */
	enable_irq_wake(gpio_to_irq(fpc1020->irq_gpio));

	fpc1020->ttw_ws = wakeup_source_register(NULL, "fpc_ttw_ws");
	if (!fpc1020->ttw_ws) {
		dev_err(dev, "failed to register wakeup source\n");
		rc = -ENODEV;
		goto exit;
	}

	rc = sysfs_create_group(&dev->kobj, &attribute_group);
	if (rc) {
		dev_err(dev, "could not create sysfs\n");
		goto exit;
	}

	if (of_property_read_bool(dev->of_node, "fpc,enable-on-boot")) {
		dev_info(dev, "Enabling hardware\n");
		(void)device_prepare(fpc1020, true);
	}

	rc = hw_reset(fpc1020);

	dev_info(dev, "%s: ok\n", __func__);

exit:
	return rc;
}

static int fpc1020_remove(struct platform_device *pdev)
{
	struct fpc1020_data *fpc1020 = platform_get_drvdata(pdev);

	sysfs_remove_group(&pdev->dev.kobj, &attribute_group);
	mutex_destroy(&fpc1020->lock);
	wakeup_source_unregister(fpc1020->ttw_ws);
	(void)vreg_setup(fpc1020, "vdd_ana", false);
	(void)vreg_setup(fpc1020, "vdd_io", false);
	(void)vreg_setup(fpc1020, "vcc_spi", false);
	dev_info(&pdev->dev, "%s\n", __func__);

	return 0;
}

static struct of_device_id fpc1020_of_match[] = {
	{ .compatible = "fpc,fpc1020", },
	{}
};
MODULE_DEVICE_TABLE(of, fpc1020_of_match);

static struct platform_driver fpc1020_driver = {
	.driver = {
		.name	= "fpc1020",
		.owner	= THIS_MODULE,
		.of_match_table = fpc1020_of_match,
	},
	.probe	= fpc1020_probe,
	.remove	= fpc1020_remove,
};

static int __init fpc1020_init(void)
{
	int rc = platform_driver_register(&fpc1020_driver);

	if (!rc)
		pr_info("%s OK\n", __func__);
	else
		pr_err("%s %d\n", __func__, rc);

	return rc;
}

static void __exit fpc1020_exit(void)
{
	pr_info("%s\n", __func__);
	platform_driver_unregister(&fpc1020_driver);
}
module_init(fpc1020_init);
module_exit(fpc1020_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Aleksej Makarov");
MODULE_AUTHOR("Henrik Tillman <henrik.tillman@fingerprints.com>");
MODULE_DESCRIPTION("FPC1020 Fingerprint sensor device driver.");
