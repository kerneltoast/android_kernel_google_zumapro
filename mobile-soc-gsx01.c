// SPDX-License-Identifier: GPL-2.0-only
/*
 * SoC specific function definitions for GSx01.
 *
 * Copyright (C) 2023 Google LLC
 */

#include <linux/acpm_dvfs.h>
#include <linux/slab.h>
#include <soc/google/exynos_pm_qos.h>

#include <gcip/gcip-kci.h>
#include <gcip/gcip-slc.h>

#include "gxp-config.h"
#include "gxp-core-telemetry.h"
#include "gxp-firmware.h"
#include "gxp-gsx01-ssmt.h"
#include "gxp-kci.h"
#include "gxp-lpm.h"
#include "gxp-pm.h"
#include "mobile-soc-gsx01.h"
#include "mobile-soc.h"

/*
 * Encode INT/MIF values as a 16 bit pair in the 32-bit return value
 * (in units of MHz, to provide enough range)
 */
#define PM_QOS_INT_SHIFT (16)
#define PM_QOS_MIF_MASK (0xFFFF)
#define PM_QOS_FACTOR (1000)

static const s32 aur_memory_state2int_table[] = { 0,
						  AUR_MEM_INT_MIN,
						  AUR_MEM_INT_VERY_LOW,
						  AUR_MEM_INT_LOW,
						  AUR_MEM_INT_HIGH,
						  AUR_MEM_INT_VERY_HIGH,
						  AUR_MEM_INT_MAX };

static const s32 aur_memory_state2mif_table[] = { 0,
						  AUR_MEM_MIF_MIN,
						  AUR_MEM_MIF_VERY_LOW,
						  AUR_MEM_MIF_LOW,
						  AUR_MEM_MIF_HIGH,
						  AUR_MEM_MIF_VERY_HIGH,
						  AUR_MEM_MIF_MAX };

static u64 pm_arg_encode(s32 int_val, s32 mif_val)
{
	return ((int_val / PM_QOS_FACTOR) << PM_QOS_INT_SHIFT) | (mif_val / PM_QOS_FACTOR);
}

static void pm_arg_decode(u64 value, s32 *int_val, s32 *mif_val)
{
	*int_val = (value >> PM_QOS_INT_SHIFT) * PM_QOS_FACTOR;
	*mif_val = (value & PM_QOS_MIF_MASK) * PM_QOS_FACTOR;
}

void gxp_soc_set_pm_arg_from_state(struct gxp_req_pm_qos_work *work,
				   enum aur_memory_power_state state)
{
	s32 int_val = aur_memory_state2int_table[state];
	s32 mif_val = aur_memory_state2mif_table[state];

	work->pm_value = pm_arg_encode(int_val, mif_val);
}

int gxp_soc_pm_set_rate(unsigned int id, unsigned long rate)
{
	return exynos_acpm_set_rate(id, rate);
}

unsigned long gxp_soc_pm_get_rate(unsigned int id, unsigned long dbg_val)
{
	return exynos_acpm_get_rate(id, dbg_val);
}

void gxp_soc_pm_init(struct gxp_dev *gxp)
{
	exynos_pm_qos_add_request(&gxp->soc_data->int_min, PM_QOS_DEVICE_THROUGHPUT, 0);
	exynos_pm_qos_add_request(&gxp->soc_data->mif_min, PM_QOS_BUS_THROUGHPUT, 0);
}

void gxp_soc_pm_exit(struct gxp_dev *gxp)
{
	exynos_pm_qos_remove_request(&gxp->soc_data->mif_min);
	exynos_pm_qos_remove_request(&gxp->soc_data->int_min);
}

void gxp_soc_pm_set_request(struct gxp_dev *gxp, u64 value)
{
	s32 int_val;
	s32 mif_val;

	pm_arg_decode(value, &int_val, &mif_val);
	dev_dbg(gxp->dev, "%s: pm_qos request - int = %d mif = %d\n", __func__, int_val, mif_val);

	exynos_pm_qos_update_request(&gxp->soc_data->int_min, int_val);
	exynos_pm_qos_update_request(&gxp->soc_data->mif_min, mif_val);
}

u64 gxp_soc_pm_get_request(struct gxp_dev *gxp)
{
	s32 int_val =
		exynos_pm_qos_read_req_value(PM_QOS_DEVICE_THROUGHPUT, &gxp->soc_data->int_min);
	s32 mif_val = exynos_pm_qos_read_req_value(PM_QOS_BUS_THROUGHPUT, &gxp->soc_data->mif_min);

	return pm_arg_encode(int_val, mif_val);
}

void gxp_soc_pm_reset(struct gxp_dev *gxp)
{
	exynos_pm_qos_update_request(&gxp->soc_data->int_min, 0);
	exynos_pm_qos_update_request(&gxp->soc_data->mif_min, 0);
}

int gxp_soc_init(struct gxp_dev *gxp)
{
	int ret;

	gxp->soc_data = devm_kzalloc(gxp->dev, sizeof(struct gxp_soc_data), GFP_KERNEL);
	if (!gxp->soc_data)
		return -ENOMEM;

	ret = gxp_gsx01_ssmt_init(gxp, &gxp->soc_data->ssmt);
	if (ret) {
		dev_err(gxp->dev, "Failed to find SSMT\n");
		return ret;
	}

	gcip_slc_debugfs_init(&gxp->soc_data->slc, gxp->dev, gxp->d_entry);

	return 0;
}

void gxp_soc_exit(struct gxp_dev *gxp)
{
	gcip_slc_debugfs_exit(&gxp->soc_data->slc);
}

void gxp_soc_activate_context(struct gxp_dev *gxp, struct gcip_iommu_domain *gdomain,
			      uint core_list)
{
	struct gxp_ssmt *ssmt = &gxp->soc_data->ssmt;
	struct gcip_slc *slc = &gxp->soc_data->slc;
	uint core;

	/* Program VID only when cores are managed by us. */
	if (gxp_is_direct_mode(gxp)) {
		for (core = 0; core < GXP_NUM_CORES; core++)
			if (BIT(core) & core_list) {
				dev_dbg(gxp->dev, "Assign core%u to PASID %d\n", core,
					gdomain->pasid);
				gxp_gsx01_ssmt_set_core_vid(ssmt, core, gdomain->pasid);
			}
	} else {
		gxp_gsx01_ssmt_activate_scid(ssmt, gdomain->pasid);
	}

	if (gcip_slc_is_valid(slc))
		gxp_gsx01_ssmt_set_slc_attr(ssmt, slc);
}

void gxp_soc_deactivate_context(struct gxp_dev *gxp, struct gcip_iommu_domain *gdomain,
				uint core_list)
{
	struct gxp_ssmt *ssmt = &gxp->soc_data->ssmt;
	uint core;

	/* Program VID only when cores are managed by us. */
	if (gxp_is_direct_mode(gxp)) {
		for (core = 0; core < GXP_NUM_CORES; core++) {
			if (BIT(core) & core_list)
				gxp_gsx01_ssmt_set_core_vid(ssmt, core, 0);
		}
	} else {
		gxp_gsx01_ssmt_deactivate_scid(ssmt, gdomain->pasid);
	}
}

void gxp_soc_set_iremap_context(struct gxp_dev *gxp)
{
}

void gxp_soc_lpm_init(struct gxp_dev *gxp)
{
	/* Startup TOP's PSM */
	gxp_lpm_init(gxp);
}

void gxp_soc_lpm_destroy(struct gxp_dev *gxp)
{
	/* Shutdown TOP's PSM */
	gxp_lpm_destroy(gxp);
}
