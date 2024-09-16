/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * GCIP-integrated IIF driver fence table.
 *
 * Copyright (C) 2023 Google LLC
 */

#ifndef __IIF_IIF_FENCE_TABLE_H__
#define __IIF_IIF_FENCE_TABLE_H__

#include <linux/of.h>
#include <linux/types.h>

#include <gcip/iif/iif.h>

/* Entry of the wait table. */
struct iif_wait_table_entry {
	uint8_t waiting_ips;
	uint8_t reserved[7];
} __packed;

/* Entry of the signal table. */
struct iif_signal_table_entry {
	uint16_t remaining_signals;
	uint8_t reserved[6];
} __packed;

/* The fence table which will be shared with the firmware side. */
struct iif_fence_table {
	struct iif_wait_table_entry *wait_table;
	struct iif_signal_table_entry *signal_table;
};

/*
 * Parses the fence table region from the device tree and map it to @fence_table.
 *
 * Returns 0 if succeeded. If it fails in mapping the table, returns -ENODEV.
 */
int iif_fence_table_init(const struct device_node *np, struct iif_fence_table *fence_table);

/*
 * Initializes the entry of @fence_id in the fence table.
 *
 * Since this function will be called only when the fence is initialized, we don't need any locks
 * to protect the entry.
 */
static inline void iif_fence_table_init_fence_entry(struct iif_fence_table *fence_table,
						    unsigned int fence_id,
						    unsigned int total_signalers)
{
	fence_table->wait_table[fence_id].waiting_ips = 0;
	fence_table->signal_table[fence_id].remaining_signals = total_signalers;
}

/*
 * Sets waiting IP bit of the wait table entry of @fence_id.
 *
 * Since this function will be called by the `iif_fence_submit_waiter` function which protects the
 * entry by itself with holding its lock, we don't have to hold any locks here.
 */
static inline void iif_fence_table_set_waiting_ip(struct iif_fence_table *fence_table,
						  unsigned int fence_id, enum iif_ip_type ip)
{
	fence_table->wait_table[fence_id].waiting_ips |= BIT(ip);
}

#endif /* __IIF_IIF_FENCE_TABLE_H__ */
