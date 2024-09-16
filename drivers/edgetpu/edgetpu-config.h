/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Defines chipset dependent configuration.
 *
 * Copyright (C) 2019 Google, Inc.
 */

#ifndef __EDGETPU_CONFIG_H__
#define __EDGETPU_CONFIG_H__

#if IS_ENABLED(CONFIG_RIO)

#include "rio/config.h"

#else /* unknown */

#error "Unknown EdgeTPU config"

#endif /* unknown */

#define EDGETPU_DEFAULT_FIRMWARE_NAME "google/edgetpu-" DRIVER_NAME ".fw"
#define EDGETPU_TEST_FIRMWARE_NAME "google/edgetpu-" DRIVER_NAME "-test.fw"

#ifndef EDGETPU_NUM_CORES
#define EDGETPU_NUM_CORES 1
#endif

/* By default IOMMU domains can be modified while detached from a mailbox.*/
#ifndef HAS_DETACHABLE_IOMMU_DOMAINS
#define HAS_DETACHABLE_IOMMU_DOMAINS	1
#endif

#ifndef EDGETPU_HAS_GSA
#define EDGETPU_HAS_GSA 1
#endif

#ifndef EDGETPU_FEATURE_ALWAYS_ON
#define EDGETPU_FEATURE_ALWAYS_ON 0
#endif

#endif /* __EDGETPU_CONFIG_H__ */
