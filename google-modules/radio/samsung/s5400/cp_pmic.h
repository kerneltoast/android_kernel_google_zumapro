// SPDX-License-Identifier: GPL-2.0
/*
 * CP PMIC (Power Management IC) driver.
 *
 * Copyright (c) 2023, Google LLC. All rights reserved.
 */

#ifndef __CP_PMIC_H__
#define __CP_PMIC_H__

void pmic_warm_reset_sequence(struct device *dev);
struct device *pmic_get_device(struct device_node *node);
struct spmi_device *pmic_get_spmi_device(struct device_node *node);

#endif

