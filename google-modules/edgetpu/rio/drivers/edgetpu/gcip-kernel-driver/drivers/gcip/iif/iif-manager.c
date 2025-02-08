// SPDX-License-Identifier: GPL-2.0-only
/*
 * GCIP-integrated IIF driver manager.
 *
 * Copyright (C) 2023 Google LLC
 */

#include <linux/container_of.h>
#include <linux/idr.h>
#include <linux/kref.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/version.h>

#include <gcip/iif/iif-fence-table.h>
#include <gcip/iif/iif-manager.h>

static void iif_manager_destroy(struct kref *kref)
{
	struct iif_manager *mgr = container_of(kref, struct iif_manager, kref);

	ida_destroy(&mgr->idp);
	kfree(mgr);
}

struct iif_manager *iif_manager_init(const struct device_node *np)
{
	struct iif_manager *mgr;
	int ret;

	mgr = kzalloc(sizeof(*mgr), GFP_KERNEL);
	if (!mgr)
		return ERR_PTR(-ENOMEM);

	ret = iif_fence_table_init(np, &mgr->fence_table);
	if (ret) {
		kfree(mgr);
		return ERR_PTR(ret);
	}

	kref_init(&mgr->kref);
	ida_init(&mgr->idp);

	return mgr;
}

struct iif_manager *iif_manager_get(struct iif_manager *mgr)
{
	kref_get(&mgr->kref);
	return mgr;
}

void iif_manager_put(struct iif_manager *mgr)
{
	kref_put(&mgr->kref, iif_manager_destroy);
}
