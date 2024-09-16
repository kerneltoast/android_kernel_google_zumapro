// SPDX-License-Identifier: GPL-2.0-only
/*
 * GCIP-integrated IIF driver fence table.
 *
 * Copyright (C) 2023 Google LLC
 */

#define pr_fmt(fmt) "iif: " fmt

#include <linux/io.h>
#include <linux/of_address.h>
#include <linux/of.h>
#include <linux/slab.h>

#include <gcip/iif/iif-fence-table.h>

#define IIF_FENCE_WAIT_TABLE_PROP_NAME "iif-fence-wait-table-region"
#define IIF_FENCE_SIGNAL_TABLE_PROP_NAME "iif-fence-signal-table-region"

static int iif_fence_table_get_resource(const struct device_node *np, const char *name,
					struct resource *r)
{
	int ret;
	struct device_node *iif_np;

	iif_np = of_parse_phandle(np, name, 0);
	if (IS_ERR_OR_NULL(iif_np))
		return -ENODEV;

	ret = of_address_to_resource(iif_np, 0, r);
	of_node_put(iif_np);

	return ret;
}

static int iif_fence_wait_table_init(const struct device_node *np,
				     struct iif_fence_table *fence_table)
{
	struct resource r;
	size_t table_size;
	void *vaddr;
	int ret;

	ret = iif_fence_table_get_resource(np, IIF_FENCE_WAIT_TABLE_PROP_NAME, &r);
	if (ret) {
		pr_err("Failed to get the fence wait-table region");
		return ret;
	}

	table_size = IIF_IP_RESERVED * IIF_NUM_FENCES_PER_IP * sizeof(*fence_table->wait_table);

	if (resource_size(&r) < table_size) {
		pr_err("Unsufficient fence wait-table space in device tree");
		return -EINVAL;
	}

	vaddr = memremap(r.start, resource_size(&r), MEMREMAP_WC);
	if (IS_ERR_OR_NULL(vaddr)) {
		pr_err("Failed to map the fence wait-table region");
		return -ENODEV;
	}

	fence_table->wait_table = vaddr;

	return 0;
}

static int iif_fence_signal_table_init(const struct device_node *np,
				       struct iif_fence_table *fence_table)
{
	struct resource r;
	size_t table_size;
	void *vaddr;
	int ret;

	ret = iif_fence_table_get_resource(np, IIF_FENCE_SIGNAL_TABLE_PROP_NAME, &r);
	if (ret) {
		pr_err("Failed to get the fence signal-table region");
		return ret;
	}

	table_size = IIF_IP_RESERVED * IIF_NUM_FENCES_PER_IP * sizeof(*fence_table->signal_table);

	if (resource_size(&r) < table_size) {
		pr_err("Unsufficient fence signal-table space in device tree");
		return -EINVAL;
	}

	vaddr = memremap(r.start, resource_size(&r), MEMREMAP_WC);
	if (IS_ERR_OR_NULL(vaddr)) {
		pr_err("Failed to map the fence signal-table region");
		return -ENODEV;
	}

	fence_table->signal_table = vaddr;

	return 0;
}

int iif_fence_table_init(const struct device_node *np, struct iif_fence_table *fence_table)
{
	int ret;

	ret = iif_fence_wait_table_init(np, fence_table);
	if (ret)
		return ret;

	ret = iif_fence_signal_table_init(np, fence_table);

	return ret;
}
