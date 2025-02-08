/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __MM_PIXEL_VMSCAN_H__
#define __MM_PIXEL_VMSCAN_H__

#include <trace/events/vmscan.h>

void vh_direct_reclaim_begin(void *data, int order, gfp_t gfp_mask);
void vh_direct_reclaim_end(void *data, unsigned long nr_reclaimed);
void rvh_madvise_pageout_begin(void *data, void **private);
void rvh_madvise_pageout_end(void *data, void *private, struct list_head *folio_list);
void rvh_reclaim_folio_list(void *data, struct list_head *folio_list, void *private);
int create_vmscan_sysfs(struct kobject *mm_kobj);
void remove_vmscan_sysfs(void);
#endif
