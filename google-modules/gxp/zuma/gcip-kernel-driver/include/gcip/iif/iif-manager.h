/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * GCIP-integrated IIF driver manager.
 *
 * Copyright (C) 2023 Google LLC
 */

#ifndef __IIF_IIF_MANAGER_H__
#define __IIF_IIF_MANAGER_H__

#include <linux/idr.h>
#include <linux/kref.h>
#include <linux/of.h>

#include <gcip/iif/iif-fence-table.h>

/*
 * The structure overall data required by IIF driver such as fence table.
 *
 * Until we have stand-alone IIF driver, one of the IP drivers will initializes a manager by
 * the `iif_init` function and every IP driver will share it.
 */
struct iif_manager {
	/* Reference count of this instance. */
	struct kref kref;
	/* Fence ID pool. */
	struct ida idp;
	/* Fence table shared with the firmware. */
	struct iif_fence_table fence_table;
};

/*
 * Initializes IIF driver and returns its manager. Its initial reference count is 1. It will map
 * the fence table by parsing the device tree via @np.
 *
 * The returned manager will be destroyed when its reference count becomes 0 by `iif_manager_put`
 * function.
 */
struct iif_manager *iif_manager_init(const struct device_node *np);

/* Increases the reference count of @mgr. */
struct iif_manager *iif_manager_get(struct iif_manager *mgr);

/* Decreases the reference count of @mgr and if it becomes 0, releases @mgr. */
void iif_manager_put(struct iif_manager *mgr);

#endif /* __IIF_IIF_MANAGER_H__ */
