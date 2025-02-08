// SPDX-License-Identifier: GPL-2.0-only
/*
 * The manager of inter-IP fences.
 *
 * It manages the pool of fence IDs. The IIF driver device will initialize a manager and each IP
 * driver will fetch the manager from the IIF device.
 *
 * Copyright (C) 2023-2024 Google LLC
 */

#include <linux/container_of.h>
#include <linux/export.h>
#include <linux/idr.h>
#include <linux/kref.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/version.h>

#include <iif/iif-fence-table.h>
#include <iif/iif-fence.h>
#include <iif/iif-manager.h>

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

int iif_manager_register_ops(struct iif_manager *mgr, enum iif_ip_type ip,
			     const struct iif_manager_ops *ops, void *data)
{
	if (!ops || !ops->fence_unblocked_by_ap)
		return -EINVAL;

	mgr->ops[ip] = ops;
	mgr->data[ip] = data;

	return 0;
}

int iif_manager_acquire_block_wakelock(struct iif_manager *mgr, enum iif_ip_type ip)
{
	if (mgr->ops[ip] && mgr->ops[ip]->acquire_block_wakelock)
		return mgr->ops[ip]->acquire_block_wakelock(mgr->data[ip]);
	return 0;
}

void iif_manager_release_block_wakelock(struct iif_manager *mgr, enum iif_ip_type ip)
{
	if (mgr->ops[ip] && mgr->ops[ip]->release_block_wakelock)
		mgr->ops[ip]->release_block_wakelock(mgr->data[ip]);
}

void iif_manager_broadcast_fence_unblocked_by_ap(struct iif_manager *mgr, struct iif_fence *fence)
{
	enum iif_ip_type ip;
	unsigned int tmp;

	for_each_waiting_ip(&mgr->fence_table, fence->id, ip, tmp)
		mgr->ops[ip]->fence_unblocked_by_ap(fence, mgr->data[ip]);
}
