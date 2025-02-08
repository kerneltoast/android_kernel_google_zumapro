// SPDX-License-Identifier: GPL-2.0-only
/*
 * Google Hall Effect Sensor Driver
 *
 * Copyright (c) 2021 Google LLC
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/input.h>
#include <linux/gpio.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/sysctl.h>

struct hall_sensor_drv_data {
	struct device *dev;
	int irq_gpio;
	struct input_dev *input;
	bool irq_enabled;
	int prev_state;
};

static void hall_sensor_report_state(struct hall_sensor_drv_data *hall_data)
{
	struct input_dev *input = hall_data->input;
	struct device *dev = hall_data->dev;
	int state = !gpio_get_value(hall_data->irq_gpio);

	dev_dbg(dev, "state:%d\n", state);
	if (hall_data->prev_state != state) {
		input_event(input, EV_SW, SW_LID, state);
		input_sync(input);
		hall_data->prev_state = state;
	}
}

static irqreturn_t hall_sensor_irq_handler(int irq, void *ptr) {
	struct hall_sensor_drv_data *hall_data = ptr;

	hall_sensor_report_state(hall_data);

	return IRQ_HANDLED;
}

static int hall_sensor_open(struct input_dev *input)
{
	struct hall_sensor_drv_data *hall_data = input_get_drvdata(input);
	struct device *dev = hall_data->dev;
	int ret;

	/* Enable hall sensor irq.*/
	if (!hall_data->irq_enabled) {
		ret = devm_request_threaded_irq(dev, gpio_to_irq(
				hall_data->irq_gpio), NULL,
				hall_sensor_irq_handler, IRQF_TRIGGER_RISING |
				IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
				"hall-sensor", hall_data);
		if (ret) {
			dev_err(dev, "Could not request irq %d with error %d\n",
				gpio_to_irq(hall_data->irq_gpio), ret);
			return ret;
		}
		hall_data->irq_enabled = true;
		dev_dbg(dev, "Requested irq %d\n",
			gpio_to_irq(hall_data->irq_gpio));
	}

	/* Report current state of hall sensor.*/
	hall_sensor_report_state(hall_data);

	return 0;
}

static void hall_sensor_close(struct input_dev *input)
{
	struct hall_sensor_drv_data *hall_data = input_get_drvdata(input);
	struct device *dev = hall_data->dev;

	if (!hall_data->irq_enabled)
		return;

	/* Free hall sensor irq.*/
	devm_free_irq(dev, gpio_to_irq(hall_data->irq_gpio), hall_data);
	hall_data->irq_enabled = false;
	dev_dbg(dev, "Free irq %d\n",
		gpio_to_irq(hall_data->irq_gpio));
}

static int create_input_device(struct hall_sensor_drv_data *hall_data) {
	struct device *dev = hall_data->dev;
	struct input_dev *input;
	int ret;

	input = devm_input_allocate_device(dev);
	if (!input) {
		dev_err(dev, "Failed to allocate input device\n");
		return -ENOMEM;
	}

	hall_data->input = input;
	input_set_drvdata(input, hall_data);
	input->name = "hall effect sensor";
	input->phys = "hall_sensor/input0";
	input->dev.parent = dev;
	input->open = hall_sensor_open;
	input->close = hall_sensor_close;

	input->id.bustype = BUS_VIRTUAL;
	input->id.vendor = 0x0001;
	input->id.product = 0x0001;
	input->id.version = 0x0100;
	set_bit(EV_SW, input->evbit);
	set_bit(EV_SYN, input->evbit);
	input_set_capability(input, EV_SW, SW_LID);

	ret = input_register_device(input);
	if (ret) {
		dev_err(dev, "Failed to register input device\n");
		hall_data->input = NULL;
		input_free_device(input);
	}

	return ret;
}

static void destroy_input_device(struct hall_sensor_drv_data *hall_data) {
	struct input_dev *input = hall_data->input;

	if (!input)
		return;

	input_unregister_device(input);
	input_free_device(input);
	hall_data->input = NULL;
}

static void hall_sensor_enable(struct hall_sensor_drv_data *hall_data,
			       bool enable) {
	struct device *dev = hall_data->dev;

	if (enable) {
		if (!hall_data->input) {
			create_input_device(hall_data);
			dev_dbg(dev, "Input registered\n");
		}
		return;
	}

	destroy_input_device(hall_data);
	dev_dbg(dev, "Input unregistered\n");
}

static ssize_t state_show(struct device *dev,
			   struct device_attribute *attr, char *buf) {
	struct hall_sensor_drv_data *hall_data = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%d\n",
			gpio_get_value(hall_data->irq_gpio));
}
static DEVICE_ATTR_RO(state);

static ssize_t enable_store(struct device *dev,
	struct device_attribute *attr,
	const char *buf, size_t count) {
	struct hall_sensor_drv_data *hall_data = dev_get_drvdata(dev);
	int ret;
	int val;

	ret = kstrtoint(buf, 10, &val);
	if (ret != 0) {
		dev_err(dev, "Invalid val:%s, err %d\n", buf, ret);
		return -EINVAL;
	}

	hall_sensor_enable(hall_data, !!val);

	return count;
}
static DEVICE_ATTR_WO(enable);

static struct attribute *attributes[] = {
	&dev_attr_enable.attr,
	&dev_attr_state.attr,
	NULL
};

static const struct attribute_group attribute_group = {
	.attrs = attributes,
};

static int hall_sensor_parse_dt(struct device *dev,
				struct hall_sensor_drv_data *hall_data) {
	struct device_node *np = dev->of_node;
	int ret = 0;

	hall_data->irq_gpio = of_get_named_gpio(np, "hall,gpio", 0);

	if (!gpio_is_valid(hall_data->irq_gpio)) {
		dev_err(dev, "irq gpio %d is invalid.", hall_data->irq_gpio);
		ret = -EINVAL;
	}

	return ret;
}

static int hall_sensor_config_gpio(struct hall_sensor_drv_data *hall_data,
				   int gpio) {
	struct device *dev = hall_data->dev;
	int ret = 0;
	ret = devm_gpio_request(dev, gpio, "hall-gpio");
	if (ret) {
		dev_err(dev, "Failed to request gpio with error %d.\n", ret);
		return ret;
	}

	ret = gpio_direction_input(gpio);
	if (ret) {
		dev_err(dev, "Failed to set input direction with error %d.\n",
			ret);
		return ret;
	}

	return ret;
}

static int hall_sensor_probe(struct platform_device *pdev) {
	struct device *dev = &pdev->dev;
	int ret = 0;
	struct hall_sensor_drv_data *hall_data = devm_kzalloc(dev,
			 sizeof(*hall_data), GFP_KERNEL);

	if (!hall_data) {
		return -ENOMEM;
	}

	hall_data->dev = dev;
	platform_set_drvdata(pdev, hall_data);

	ret = hall_sensor_parse_dt(dev, hall_data);
	if (ret) {
		dev_err(dev, "Failed to parse dt with error %d.\n", ret);
		return ret;
	}

	ret = hall_sensor_config_gpio(hall_data, hall_data->irq_gpio);
	if (ret) {
		dev_err(dev, "Failed to config gpio with error %d.\n", ret);
		return ret;
	}

	hall_data->irq_enabled = false;
	hall_data->prev_state = -1;

	ret = create_input_device(hall_data);
	if (ret) {
		dev_err(dev, "Failed to create input device with error %d.\n", ret);
		return ret;
	}

	ret = sysfs_create_group(&dev->kobj, &attribute_group);
	if (ret) {
		dev_err(dev, "Failed to create sysfs with error %d.\n", ret);
		return ret;
	}

	dev_dbg(dev, "%s done\n", __func__);

	return ret;
}

static int hall_sensor_remove(struct platform_device *pdev) {
	struct hall_sensor_drv_data *hall_data = platform_get_drvdata(pdev);

	sysfs_remove_group(&pdev->dev.kobj, &attribute_group);
	hall_sensor_enable(hall_data, false);
	return 0;
}

static struct of_device_id hall_sensor_of_match[] = {
	{ .compatible = "hall,drv5032", },
	{}
};
MODULE_DEVICE_TABLE(of, hall_sensor_of_match);

static struct platform_driver hall_sensor_driver = {
	.driver = {
		.name	= "hall_sensor",
		.owner	= THIS_MODULE,
		.of_match_table = hall_sensor_of_match,
	},
	.probe	= hall_sensor_probe,
	.remove	= hall_sensor_remove,
};

static int __init hall_sensor_init(void)
{
	return platform_driver_register(&hall_sensor_driver);
}

static void __exit hall_sensor_exit(void)
{
	platform_driver_unregister(&hall_sensor_driver);
}

module_init(hall_sensor_init);
module_exit(hall_sensor_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Rick Chen <rickctchen@google.com>");
MODULE_DESCRIPTION("Hall effect sensor driver");
