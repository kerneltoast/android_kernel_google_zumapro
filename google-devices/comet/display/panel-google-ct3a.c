// SPDX-License-Identifier: GPL-2.0-only
/*
 * MIPI-DSI based ct3a AMOLED LCD panel driver.
 *
 * Copyright (c) 2023 Google LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <drm/drm_vblank.h>
#include <drm/display/drm_dsc_helper.h>
#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/thermal.h>
#include <video/mipi_display.h>

#include "trace/dpu_trace.h"
#include "trace/panel_trace.h"
#include "panel/panel-samsung-drv.h"
#include "gs_drm/gs_display_mode.h"

/**
 * enum ct3a_panel_feature - features supported by this panel
 * @FEAT_EARLY_EXIT: early exit from a long frame
 * @FEAT_OP_NS: normal speed (not high speed)
 * @FEAT_FRAME_AUTO: automatic (not manual) frame control
 * @FEAT_MAX: placeholder, counter for number of features
 *
 * The following features are correlated, if one or more of them change, the others need
 * to be updated unconditionally.
 */
enum ct3a_panel_feature {
	FEAT_EARLY_EXIT,
	FEAT_OP_NS,
	FEAT_FRAME_AUTO,
	FEAT_MAX,
};

#define VLIN_CMD_SIZE 3

static const u8 vlin_7v7[] = { 0x46, 0x23, 0x06 };
static const u8 vlin_7v9[] = { 0x46, 0x23, 0x02 };
static const u8 vgh_7v1[]  = { 0xF4, 0x15, 0x15, 0x15, 0x15 };
static const u8 vgh_7v4[]  = { 0xF4, 0x18, 0x18, 0x18, 0x18 };
static const u8 vreg_6v9[] = { 0xF4, 0x18 };

/**
 * struct ct3a_effective_hw_config - panel effective hardware configurations
 *
 * This struct maintains c3a panel configurations that have been committed to
 * hardware and are currently active.
 */
struct ct3a_effective_hw_config {
	/** @feat: correlated feature effective in panel */
	DECLARE_BITMAP(feat, FEAT_MAX);
	/** @vrefresh: vrefresh rate effective in panel */
	u32 vrefresh;
	/** @te_freq: panel TE frequency */
	u32 te_freq;
	/** @idle_vrefresh: idle vrefresh rate effective in panel */
	u32 idle_vrefresh;
	/** @dbv: brightness */
	u16 dbv;
	/** @za_enabled: whether zonal attenuation is enabled */
	bool za_enabled;
	/** @acl_setting: automatic current limiting setting */
	u8 acl_setting;
};

/**
 * struct ct3a_panel - panel specific runtime info
 *
 * This struct maintains ct3a panel specific runtime info, any fixed details about panel
 * should most likely go into struct exynos_panel_desc
 */
struct ct3a_panel {
	/** @base: base panel struct */
	struct exynos_panel base;
	/** @feat: software or working correlated features, not guaranteed to be effective in panel */
	DECLARE_BITMAP(feat, FEAT_MAX);
	/**
	 * @auto_mode_vrefresh: indicates current minimum refresh rate while in auto mode,
	 *			if 0 it means that auto mode is not enabled
	 */
	u32 auto_mode_vrefresh;
	/** @force_changeable_te: force changeable TE (instead of fixed) during early exit */
	bool force_changeable_te;
	/** @force_changeable_te2: force changeable TE (instead of fixed) for monitoring refresh rate */
	bool force_changeable_te2;
	/**
	 * @is_pixel_off: pixel-off command is sent to panel. Only sending normal-on or resetting
	 *		  panel can recover to normal mode after entering pixel-off state.
	 */
	bool is_pixel_off;
	/** @force_fi: force to keep frame insertion enabled */
	bool force_fi;
	/** @tzd: thermal zone struct */
	struct thermal_zone_device *tzd;
	/** @panel_voltage: panel default voltage  */
	struct panel_voltage {
		u8 vlin_default[VLIN_CMD_SIZE];
	} panel_voltage;
	struct ct3a_effective_hw_config hw;
};

#define to_spanel(ctx) container_of(ctx, struct ct3a_panel, base)

static const struct drm_dsc_config pps_config = {
		.line_buf_depth = 9,
		.bits_per_component = 8,
		.convert_rgb = true,
		.slice_width = 1076,
		.slice_height = 173,
		.simple_422 = false,
		.pic_width = 2152,
		.pic_height = 2076,
		.rc_tgt_offset_high = 3,
		.rc_tgt_offset_low = 3,
		.bits_per_pixel = 128,
		.rc_edge_factor = 6,
		.rc_quant_incr_limit1 = 11,
		.rc_quant_incr_limit0 = 11,
		.initial_xmit_delay = 512,
		.initial_dec_delay = 930,
		.block_pred_enable = true,
		.first_line_bpg_offset = 15,
		.initial_offset = 6144,
		.rc_buf_thresh = {
				14, 28, 42, 56,
				70, 84, 98, 105,
				112, 119, 121, 123,
				125, 126
		},
		.rc_range_params = {
				{.range_min_qp = 0, .range_max_qp = 4, .range_bpg_offset = 2},
				{.range_min_qp = 0, .range_max_qp = 4, .range_bpg_offset = 0},
				{.range_min_qp = 1, .range_max_qp = 5, .range_bpg_offset = 0},
				{.range_min_qp = 1, .range_max_qp = 6, .range_bpg_offset = 62},
				{.range_min_qp = 3, .range_max_qp = 7, .range_bpg_offset = 60},
				{.range_min_qp = 3, .range_max_qp = 7, .range_bpg_offset = 58},
				{.range_min_qp = 3, .range_max_qp = 7, .range_bpg_offset = 56},
				{.range_min_qp = 3, .range_max_qp = 8, .range_bpg_offset = 56},
				{.range_min_qp = 3, .range_max_qp = 9, .range_bpg_offset = 56},
				{.range_min_qp = 3, .range_max_qp = 10, .range_bpg_offset = 54},
				{.range_min_qp = 5, .range_max_qp = 10, .range_bpg_offset = 54},
				{.range_min_qp = 5, .range_max_qp = 11, .range_bpg_offset = 52},
				{.range_min_qp = 5, .range_max_qp = 11, .range_bpg_offset = 52},
				{.range_min_qp = 9, .range_max_qp = 12, .range_bpg_offset = 52},
				{.range_min_qp = 12, .range_max_qp = 13, .range_bpg_offset = 52}
		},
		.rc_model_size = 8192,
		.flatness_min_qp = 3,
		.flatness_max_qp = 12,
		.initial_scale_value = 32,
		.scale_decrement_interval = 14,
		.scale_increment_interval = 4976,
		.nfl_bpg_offset = 179,
		.slice_bpg_offset = 75,
		.final_offset = 4320,
		.vbr_enable = false,
		.slice_chunk_size = 1076,
		.dsc_version_minor = 2,
		.dsc_version_major = 1,
		.native_422 = false,
		.native_420 = false,
		.second_line_bpg_offset = 0,
		.nsl_bpg_offset = 0,
		.second_line_offset_adj = 0,
};

#define CT3A_WRCTRLD_DIMMING_BIT    0x08
#define CT3A_WRCTRLD_BCTRL_BIT      0x20

#define CT3A_TE_USEC_120HZ_HS 320
#define CT3A_TE_USEC_VRR_HS 320
#define CT3A_TE_USEC_VRR_NS 640

static const u8 unlock_cmd_f0[] = { 0xF0, 0x5A, 0x5A };
static const u8 lock_cmd_f0[] = { 0xF0, 0xA5, 0xA5 };
static const u8 ltps_update[] = { 0xF7, 0x0F };
static const u8 pixel_off[] = { 0x22 };

static const struct exynos_dsi_cmd ct3a_lp_low_cmds[] = {
	/* AOD Low Mode, 10nit */
	EXYNOS_DSI_CMD_SEQ(0x51, 0x03, 0x8A),
};

static const struct exynos_dsi_cmd ct3a_lp_high_cmds[] = {
	/* AOD Low Mode, 10nit */
	EXYNOS_DSI_CMD_SEQ(0x51, 0x07, 0xFF),
};

static const struct exynos_binned_lp ct3a_binned_lp[] = {
	/* low threshold 40 nits */
	BINNED_LP_MODE("low", 689, ct3a_lp_low_cmds),
	BINNED_LP_MODE("high", 2988, ct3a_lp_high_cmds)
};

static const struct exynos_dsi_cmd ct3a_off_cmds[] = {
	EXYNOS_DSI_CMD_SEQ(MIPI_DCS_SET_DISPLAY_OFF),
	EXYNOS_DSI_CMD_SEQ_DELAY(120, MIPI_DCS_ENTER_SLEEP_MODE),
};
static DEFINE_EXYNOS_CMD_SET(ct3a_off);

static const struct exynos_dsi_cmd ct3a_init_cmds[] = {
	EXYNOS_DSI_CMD_SEQ(MIPI_DCS_SET_TEAR_ON),

	/* CASET: 2151 */
	EXYNOS_DSI_CMD_SEQ(MIPI_DCS_SET_COLUMN_ADDRESS, 0x00, 0x00, 0x08, 0x67),
	/* PASET: 2075 */
	EXYNOS_DSI_CMD_SEQ(MIPI_DCS_SET_PAGE_ADDRESS, 0x00, 0x00, 0x08, 0x1B),

	EXYNOS_DSI_CMD0(unlock_cmd_f0),
	/* manual TE, fixed TE2 setting */
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_PROTO1, 0xB9, 0x00, 0x51, 0x00, 0x00),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_PROTO1_1|PANEL_REV_PROTO1_2, 0xB9, 0x04, 0x51, 0x00, 0x00),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0xB9, 0x04),
	EXYNOS_DSI_CMD_SEQ(0xB0, 0x00, 0x08, 0xB9),
	EXYNOS_DSI_CMD_SEQ(0xB9, 0x08, 0x1C, 0x00, 0x00, 0x08, 0x1C, 0x00, 0x00),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0xB0, 0x00, 0x01, 0xB9),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0xB9, 0x51),
	EXYNOS_DSI_CMD_SEQ(0xB0, 0x00, 0x22, 0xB9),
	EXYNOS_DSI_CMD_SEQ(0xB9, 0x00, 0x2F, 0x00, 0x82, 0x00, 0x2F, 0x00, 0x82),

	/* early exit off */
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_PROTO1_2), 0xB0, 0x00, 0x01, 0xBD),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_LT(PANEL_REV_PROTO1_2), 0xBD, 0x81),
	EXYNOS_DSI_CMD0(ltps_update),

	/* gamma improvement setting */
	EXYNOS_DSI_CMD_SEQ(0xB0, 0x00, 0x24, 0xF8),
	EXYNOS_DSI_CMD_SEQ(0xF8, 0x05),

	/* HLPM transition preset*/
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0xB0, 0x00, 0x03, 0xBB),
	EXYNOS_DSI_CMD_SEQ_REV(PANEL_REV_GE(PANEL_REV_EVT1), 0xBB, 0x45, 0x0E),

	EXYNOS_DSI_CMD0(lock_cmd_f0),
};
static DEFINE_EXYNOS_CMD_SET(ct3a_init);

static u8 ct3a_get_te2_option(struct exynos_panel *ctx)
{
	struct ct3a_panel *spanel = to_spanel(ctx);

	if (!ctx || !ctx->current_mode || spanel->force_changeable_te2)
		return TE2_OPT_CHANGEABLE;

	if (ctx->current_mode->exynos_mode.is_lp_mode ||
	    (test_bit(FEAT_EARLY_EXIT, spanel->feat) &&
		spanel->auto_mode_vrefresh < 30))
		return TE2_OPT_FIXED;

	return TE2_OPT_CHANGEABLE;
}

static void ct3a_update_te2(struct exynos_panel *ctx)
{
	struct ct3a_panel *spanel = to_spanel(ctx);
	ctx->te2.option = ct3a_get_te2_option(ctx);

	dev_dbg(ctx->dev,
		"TE2 updated: %s mode, option %s, idle %s\n",
		test_bit(FEAT_OP_NS, spanel->feat) ? "NS" : "HS",
		(ctx->te2.option == TE2_OPT_CHANGEABLE) ? "changeable" : "fixed",
		ctx->panel_idle_vrefresh ? "active" : "inactive");
}

static inline bool is_auto_mode_allowed(struct exynos_panel *ctx)
{
	/* don't want to enable auto mode/early exit during dimming on */
	if (ctx->dimming_on)
		return false;

	if (ctx->idle_delay_ms) {
		const unsigned int delta_ms = panel_get_idle_time_delta(ctx);

		if (delta_ms < ctx->idle_delay_ms)
			return false;
	}

	return ctx->panel_idle_enabled;
}

static u32 ct3a_get_min_idle_vrefresh(struct exynos_panel *ctx,
				     const struct exynos_panel_mode *pmode)
{
	const int vrefresh = drm_mode_vrefresh(&pmode->mode);
	int min_idle_vrefresh = ctx->min_vrefresh;

	if ((min_idle_vrefresh < 0) || !is_auto_mode_allowed(ctx))
		return 0;

	if (min_idle_vrefresh <= 1)
		min_idle_vrefresh = 1;
	else if (min_idle_vrefresh <= 10)
		min_idle_vrefresh = 10;
	else if (min_idle_vrefresh <= 30)
		min_idle_vrefresh = 30;
	else
		return 0;

	if (min_idle_vrefresh >= vrefresh) {
		dev_dbg(ctx->dev, "min idle vrefresh (%d) higher than target (%d)\n",
				min_idle_vrefresh, vrefresh);
		return 0;
	}

	dev_dbg(ctx->dev, "%s: min_idle_vrefresh %d\n", __func__, min_idle_vrefresh);

	return min_idle_vrefresh;
}

/**
 * ct3a_set_panel_feat - configure panel features
 * @ctx: exynos_panel struct
 * @pmode: exynos_panel_mode struct, target panel mode
 * @idle_vrefresh: target vrefresh rate in auto mode, 0 if disabling auto mode
 * @enforce: force to write all of registers even if no feature state changes
 *
 * Configure panel features based on the context.
 */
static void ct3a_set_panel_feat(struct exynos_panel *ctx,
	const struct exynos_panel_mode *pmode, u32 idle_vrefresh, bool enforce)
{
	struct ct3a_panel *spanel = to_spanel(ctx);
	unsigned long *feat = spanel->feat;
	u32 vrefresh = drm_mode_vrefresh(&pmode->mode);
	u32 te_freq = exynos_drm_mode_te_freq(&pmode->mode);
	bool is_vrr = is_vrr_mode(pmode);
	u8 val;
	DECLARE_BITMAP(changed_feat, FEAT_MAX);

#ifndef PANEL_FACTORY_BUILD
	vrefresh = 1;
	idle_vrefresh = 0;
	set_bit(FEAT_EARLY_EXIT, feat);
	if (spanel->force_fi)
		set_bit(FEAT_FRAME_AUTO, feat);
	else
		clear_bit(FEAT_FRAME_AUTO, feat);
	if (is_vrr) {
		if (pmode->mode.flags & DRM_MODE_FLAG_NS)
			set_bit(FEAT_OP_NS, feat);
		else
			clear_bit(FEAT_OP_NS, feat);
	}
#endif

	if (enforce) {
		bitmap_fill(changed_feat, FEAT_MAX);
	} else {
		bitmap_xor(changed_feat, feat, spanel->hw.feat, FEAT_MAX);
		if (bitmap_empty(changed_feat, FEAT_MAX) &&
			vrefresh == spanel->hw.vrefresh &&
			idle_vrefresh == spanel->hw.idle_vrefresh &&
			te_freq == spanel->hw.te_freq) {
			dev_dbg(ctx->dev, "%s: no changes, skip update\n", __func__);
			return;
		}
	}

	dev_dbg(ctx->dev,
		"op=%s ee=%s fi=%s fps=%u idle_fps=%u te=%u vrr=%s\n",
		test_bit(FEAT_OP_NS, feat) ? "ns" : "hs",
		test_bit(FEAT_EARLY_EXIT, feat) ? "on" : "off",
		test_bit(FEAT_FRAME_AUTO, feat) ? "auto" : "manual",
		vrefresh, idle_vrefresh, te_freq,
		is_vrr ? "y" : "n");

	EXYNOS_DCS_BUF_ADD_SET(ctx, unlock_cmd_f0);
	/* TE setting */
	if (test_bit(FEAT_EARLY_EXIT, changed_feat) ||
		test_bit(FEAT_OP_NS, changed_feat) || spanel->hw.te_freq != te_freq) {
		if (test_bit(FEAT_EARLY_EXIT, feat) && !spanel->force_changeable_te) {
			if (is_vrr && te_freq == 240) {
				/* 240Hz multi TE */
				EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x00, 0x08, 0xB9);
				if (test_bit(FEAT_OP_NS, feat))
					EXYNOS_DCS_BUF_ADD(ctx, 0xB9, 0x08, 0x1C, 0x00, 0x00, 0x01,
							0xC6, 0x00, 0x01);
				else
					EXYNOS_DCS_BUF_ADD(ctx, 0xB9, 0x08, 0x1C, 0x00, 0x00, 0x03,
							0xE0, 0x00, 0x01);
				EXYNOS_DCS_BUF_ADD(ctx, 0xB9, 0x61);
			} else {
				if (ctx->panel_rev < PANEL_REV_EVT1)
					EXYNOS_DCS_BUF_ADD(ctx, 0xB9, 0x51, 0x51, 0x00, 0x00);
				else
					EXYNOS_DCS_BUF_ADD(ctx, 0xB9, 0x51);

				EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x00, 0x02, 0xB9);
				/* Fixed TE */
				if (test_bit(FEAT_OP_NS, feat) || te_freq != 60) {
					EXYNOS_DCS_BUF_ADD(ctx, 0xB9, 0x00);
				} else {
					EXYNOS_DCS_BUF_ADD(ctx, 0xB9, 0x01);
				}
			}
		} else {
			/* Changeable TE */
			if (ctx->panel_rev == PANEL_REV_PROTO1)
				EXYNOS_DCS_BUF_ADD(ctx, 0xB9, 0x00, 0x51, 0x00, 0x00);
			else if (ctx->panel_rev == PANEL_REV_PROTO1_1 || ctx->panel_rev == PANEL_REV_PROTO1_2)
				EXYNOS_DCS_BUF_ADD(ctx, 0xB9, 0x04, 0x51, 0x00, 0x00);
			else
				EXYNOS_DCS_BUF_ADD(ctx, 0xB9, 0x04);
		}
	}

	/*
	 * Early-exit: enable or disable
	 */
	if (is_vrr) {
		EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x00, 0x01, 0xBD);
		EXYNOS_DCS_BUF_ADD(ctx, 0xBD, 0x41);
	} else {
		EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x00, 0x01, 0xBD);
		val = test_bit(FEAT_EARLY_EXIT, feat) ? 0x01 : 0x81;
		EXYNOS_DCS_BUF_ADD(ctx, 0xBD, val);
	}

	/*
	 * Frequency setting: FI, frequency, idle frequency
	 *
	 * Description: this sequence possibly overrides some configs early-exit
	 * and operation set, depending on FI mode.
	 */
	if (test_bit(FEAT_FRAME_AUTO, feat)) {
		if (test_bit(FEAT_OP_NS, feat))
			EXYNOS_DCS_BUF_ADD(ctx, 0x60, 0x18);
		else
			EXYNOS_DCS_BUF_ADD(ctx, 0x60, 0x00);
		/* frame insertion on */
		EXYNOS_DCS_BUF_ADD(ctx, 0xBD, 0xE3);
		/* target frequency */
		EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x00, 0x13, 0xBD);
		if (test_bit(FEAT_OP_NS, feat)) {
			if (idle_vrefresh == 30) {
				val = 0x04;
			} else if (idle_vrefresh == 10) {
				val = 0x14;
			} else {
				if (idle_vrefresh != 1)
					dev_warn(ctx->dev, "%s: unsupported target freq %d (ns)\n",
						 __func__, idle_vrefresh);
				/* 1Hz */
				val = 0xEC;
			}
			EXYNOS_DCS_BUF_ADD(ctx, 0xBD, 0x00, val);
		} else {
			if (idle_vrefresh == 60) {
				val = 0x02;
			} else if (idle_vrefresh == 30) {
				val = 0x06;
			} else if (idle_vrefresh == 10) {
				val = 0x16;
			} else {
				if (idle_vrefresh != 1)
					dev_warn(ctx->dev, "%s: unsupported target freq %d (hs)\n",
						 __func__, idle_vrefresh);
				/* 1Hz */
				val = 0xEC;
			}
			EXYNOS_DCS_BUF_ADD(ctx, 0xBD, 0x00, val);
		}
		/* step setting */
		EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x00, 0x9E, 0xBD);
		if (test_bit(FEAT_OP_NS, feat))
			EXYNOS_DCS_BUF_ADD(ctx, 0xBD, 0x00, 0x00, 0x00, 0x04, 0x00, 0x14, 0x00, 0x00);
		else
			EXYNOS_DCS_BUF_ADD(ctx, 0xBD, 0x00, 0x00, 0x00, 0x02, 0x00, 0x06, 0x00, 0x16);

		EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x00, 0xAE, 0xBD);
		if (test_bit(FEAT_OP_NS, feat))
			EXYNOS_DCS_BUF_ADD(ctx, 0xBD, 0x00, 0x02, 0x00, 0x00);
		else
			EXYNOS_DCS_BUF_ADD(ctx, 0xBD, 0x00, 0x00, 0x02, 0x00);

		if (ctx->panel_rev >= PANEL_REV_PROTO1_2) {
			if (test_bit(FEAT_OP_NS, feat)) {
				if (ctx->panel_rev == PANEL_REV_PROTO1_2)
					EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x00, 0x85);
				else
					EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x00, 0x85, 0xBD);
			} else {
				if (ctx->panel_rev == PANEL_REV_PROTO1_2)
					EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x00, 0x83);
				else
					EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x00, 0x83, 0xBD);
			}
			EXYNOS_DCS_BUF_ADD(ctx, 0xBD, 0x00);
		}
	} else { /* manual */
		if (!is_vrr) {
			EXYNOS_DCS_BUF_ADD(ctx, 0xBD, 0xE1);
		}
		if (test_bit(FEAT_OP_NS, feat)) {
			if(is_vrr) {
				EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x00, 0x83, 0xBD);
				EXYNOS_DCS_BUF_ADD(ctx, 0xBD, 0x00);
				EXYNOS_DCS_BUF_ADD(ctx, 0xB9, 0x61);
			}

			EXYNOS_DCS_BUF_ADD(ctx, 0xF2, 0x01);

			if (vrefresh == 1) {
				if (ctx->panel_rev >= PANEL_REV_PROTO1_2)
					val = 0x1E;
				else
					val = 0x1D;
			} else if (vrefresh == 10) {
				val = 0x1C;
			} else if (vrefresh == 30) {
				val = 0x19;
			} else {
				if (vrefresh != 60)
					dev_warn(ctx->dev,
						 "%s: unsupported manual freq %d (ns)\n",
						 __func__, vrefresh);
				/* 60Hz */
				val = 0x18;
			}
		} else {
			EXYNOS_DCS_BUF_ADD(ctx, 0xF2, 0x01);

			if (vrefresh == 1) {
				val = 0x06;
			} else if (vrefresh == 10) {
				val = 0x05;
			} else if (vrefresh == 30) {
				if (ctx->panel_rev < PANEL_REV_EVT1)
					val = 0x02;
				else
					val = 0x03;
			} else if (vrefresh == 60) {
				if (ctx->panel_rev < PANEL_REV_EVT1)
					val = 0x01;
				else
					val = 0x02;
			} else {
				if (vrefresh != 120)
					dev_warn(ctx->dev,
						 "%s: unsupported manual freq %d (hs)\n",
						 __func__, vrefresh);
				/* 120Hz */
				val = 0x00;
			}
		}
		/* TODO: b/321871919 - send in the same batch */
		if(is_vrr)
			EXYNOS_DCS_BUF_ADD_AND_FLUSH(ctx, 0x60, val);
		else
			EXYNOS_DCS_BUF_ADD(ctx, 0x60, val);
	}
	/* TODO: b/321871919 - send in the same batch */
	if(is_vrr)
		EXYNOS_DCS_BUF_ADD_SET_AND_FLUSH(ctx, ltps_update);
	else
		EXYNOS_DCS_BUF_ADD_SET(ctx, ltps_update);
	EXYNOS_DCS_BUF_ADD_SET_AND_FLUSH(ctx, lock_cmd_f0);

	spanel->hw.vrefresh = vrefresh;
	spanel->hw.idle_vrefresh = idle_vrefresh;
	spanel->hw.te_freq = te_freq;
	bitmap_copy(spanel->hw.feat, feat, FEAT_MAX);
}

/**
 * ct3a_update_panel_feat - configure panel features with current refresh rate
 * @ctx: exynos_panel struct
 * @enforce: force to write all of registers even if no feature state changes
 *
 * Configure panel features based on the context without changing current refresh rate
 * and idle setting.
 */
static void ct3a_update_panel_feat(struct exynos_panel *ctx, bool enforce)
{
	const struct exynos_panel_mode *pmode = ctx->current_mode;
	struct ct3a_panel *spanel = to_spanel(ctx);
	u32 idle_vrefresh = spanel->auto_mode_vrefresh;

	ct3a_set_panel_feat(ctx, pmode, idle_vrefresh, enforce);
}

static void ct3a_update_refresh_mode(struct exynos_panel *ctx,
				const struct exynos_panel_mode *pmode,
					const u32 idle_vrefresh)
{
	struct ct3a_panel *spanel = to_spanel(ctx);

	dev_info(ctx->dev, "%s: mode: %s set idle_vrefresh: %u\n", __func__,
		pmode->mode.name, idle_vrefresh);

	spanel->auto_mode_vrefresh = idle_vrefresh;
	/*
	 * Note: when mode is explicitly set, panel performs early exit to get out
	 * of idle at next vsync, and will not back to idle until not seeing new
	 * frame traffic for a while. If idle_vrefresh != 0, try best to guess what
	 * panel_idle_vrefresh will be soon, and ct3a_update_idle_state() in
	 * new frame commit will correct it if the guess is wrong.
	 */
	ctx->panel_idle_vrefresh = idle_vrefresh;
	ct3a_set_panel_feat(ctx, pmode, idle_vrefresh, false);
	notify_panel_mode_changed(ctx, false);

	dev_dbg(ctx->dev, "%s: display state is notified\n", __func__);
}

static void ct3a_change_frequency(struct exynos_panel *ctx,
					const struct exynos_panel_mode *pmode)
{
	u32 vrefresh = drm_mode_vrefresh(&pmode->mode);
	u32 idle_vrefresh = 0;

	if (!ctx)
		return;

	if (vrefresh > ctx->op_hz) {
		dev_err(ctx->dev, "invalid freq setting: op_hz=%u, vrefresh=%u\n",
				ctx->op_hz, vrefresh);
		return;
	}

	if (pmode->idle_mode == IDLE_MODE_ON_INACTIVITY)
		idle_vrefresh = ct3a_get_min_idle_vrefresh(ctx, pmode);

	ct3a_update_refresh_mode(ctx, pmode, idle_vrefresh);

	dev_dbg(ctx->dev, "%s: change to %uHz\n", __func__, vrefresh);
}

static void ct3a_panel_idle_notification(struct exynos_panel *ctx,
		u32 display_id, u32 vrefresh, u32 idle_te_vrefresh)
{
	char event_string[64];
	char *envp[] = { event_string, NULL };
	struct drm_device *dev = ctx->bridge.dev;

	if (!dev) {
		dev_warn(ctx->dev, "%s: drm_device is null\n", __func__);
	} else {
		snprintf(event_string, sizeof(event_string),
			"PANEL_IDLE_ENTER=%u,%u,%u", display_id, vrefresh, idle_te_vrefresh);
		kobject_uevent_env(&dev->primary->kdev->kobj, KOBJ_CHANGE, envp);
	}
}

static void ct3a_wait_one_vblank(struct exynos_panel *ctx)
{
	struct drm_crtc *crtc = NULL;

	if (ctx->exynos_connector.base.state)
		crtc = ctx->exynos_connector.base.state->crtc;

	DPU_ATRACE_BEGIN(__func__);
	if (crtc) {
		int ret = drm_crtc_vblank_get(crtc);

		if (!ret) {
			drm_crtc_wait_one_vblank(crtc);
			drm_crtc_vblank_put(crtc);
		} else {
			usleep_range(8350, 8500);
		}
	} else {
		usleep_range(8350, 8500);
	}
	DPU_ATRACE_END(__func__);
}

static bool ct3a_set_self_refresh(struct exynos_panel *ctx, bool enable)
{
	const struct exynos_panel_mode *pmode = ctx->current_mode;
	struct ct3a_panel *spanel = to_spanel(ctx);
	u32 idle_vrefresh;

	dev_dbg(ctx->dev, "%s: %d\n", __func__, enable);

	if (unlikely(!pmode))
		return false;

	/* self refresh is not supported in lp mode since that always makes use of early exit */
	if (pmode->exynos_mode.is_lp_mode) {
		/* set 1Hz while self refresh is active, otherwise clear it */
		ctx->panel_idle_vrefresh = enable ? 1 : 0;
		notify_panel_mode_changed(ctx, true);
		return false;
	}

	idle_vrefresh = ct3a_get_min_idle_vrefresh(ctx, pmode);

	if (pmode->idle_mode != IDLE_MODE_ON_SELF_REFRESH) {
		/*
		 * if idle mode is on inactivity, may need to update the target fps for auto mode,
		 * or switch to manual mode if idle should be disabled (idle_vrefresh=0)
		 */
		if ((pmode->idle_mode == IDLE_MODE_ON_INACTIVITY) &&
			(spanel->auto_mode_vrefresh != idle_vrefresh)) {
			ct3a_update_refresh_mode(ctx, pmode, idle_vrefresh);
			return true;
		}
		return false;
	}

	if (!enable)
		idle_vrefresh = 0;

	/* if there's no change in idle state then skip cmds */
	if (ctx->panel_idle_vrefresh == idle_vrefresh)
		return false;

	DPU_ATRACE_BEGIN(__func__);
	ct3a_update_refresh_mode(ctx, pmode, idle_vrefresh);

	if (idle_vrefresh) {
		const int vrefresh = drm_mode_vrefresh(&pmode->mode);

		ct3a_panel_idle_notification(ctx, 0, vrefresh, 120);
	} else if (ctx->panel_need_handle_idle_exit) {
		/*
		 * after exit idle mode with fixed TE at non-120hz, TE may still keep at 120hz.
		 * If any layer that already be assigned to DPU that can't be handled at 120hz,
		 * panel_need_handle_idle_exit will be set then we need to wait one vblank to
		 * avoid underrun issue.
		 */
		dev_dbg(ctx->dev, "wait one vblank after exit idle\n");
		ct3a_wait_one_vblank(ctx);
	}

	DPU_ATRACE_END(__func__);

	return true;
}

static void ct3a_refresh_ctrl(struct exynos_panel *ctx, u32 ctrl)
{
	DPU_ATRACE_BEGIN(__func__);

	if (ctrl & PANEL_REFRESH_CTRL_FI) {
		dev_dbg(ctx->dev, "%s: performing a frame insertion\n", __func__);
		EXYNOS_DCS_BUF_ADD(ctx, 0x2C, 0x00);
	}

	DPU_ATRACE_END(__func__);
}

static int ct3a_atomic_check(struct exynos_panel *ctx, struct drm_atomic_state *state)
{
	struct drm_connector *conn = &ctx->exynos_connector.base;
	struct drm_connector_state *new_conn_state = drm_atomic_get_new_connector_state(state, conn);
	struct drm_crtc_state *old_crtc_state, *new_crtc_state;
	struct ct3a_panel *spanel = to_spanel(ctx);

	if (!ctx->current_mode || drm_mode_vrefresh(&ctx->current_mode->mode) == 120 ||
	    !new_conn_state || !new_conn_state->crtc)
		return 0;

	new_crtc_state = drm_atomic_get_new_crtc_state(state, new_conn_state->crtc);
	old_crtc_state = drm_atomic_get_old_crtc_state(state, new_conn_state->crtc);
	if (!old_crtc_state || !new_crtc_state || !new_crtc_state->active)
		return 0;

	if ((spanel->auto_mode_vrefresh && old_crtc_state->self_refresh_active) ||
	    !drm_atomic_crtc_effectively_active(old_crtc_state)) {
		struct drm_display_mode *mode = &new_crtc_state->adjusted_mode;

		/* set clock to max refresh rate on self refresh exit or resume due to early exit */
		mode->clock = mode->htotal * mode->vtotal * 120 / 1000;

		if (mode->clock != new_crtc_state->mode.clock) {
			new_crtc_state->mode_changed = true;
			dev_dbg(ctx->dev, "raise mode (%s) clock to 120hz on %s\n",
				mode->name,
				old_crtc_state->self_refresh_active ? "self refresh exit" : "resume");
		}
	} else if (old_crtc_state->active_changed &&
		   (old_crtc_state->adjusted_mode.clock != old_crtc_state->mode.clock)) {
		/* clock hacked in last commit due to self refresh exit or resume, undo that */
		new_crtc_state->mode_changed = true;
		new_crtc_state->adjusted_mode.clock = new_crtc_state->mode.clock;
		dev_dbg(ctx->dev, "restore mode (%s) clock after self refresh exit or resume\n",
			new_crtc_state->mode.name);
	}

	return 0;
}

static void ct3a_update_wrctrld(struct exynos_panel *ctx)
{
	u8 val = CT3A_WRCTRLD_BCTRL_BIT;

	if (ctx->dimming_on)
		val |= CT3A_WRCTRLD_DIMMING_BIT;

	dev_dbg(ctx->dev,
		"%s(wrctrld:0x%x, hbm: %s, dimming: %s local_hbm: %s)\n",
		__func__, val, IS_HBM_ON(ctx->hbm_mode) ? "on" : "off",
		ctx->dimming_on ? "on" : "off",
		ctx->hbm.local_hbm.enabled ? "on" : "off");

	EXYNOS_DCS_BUF_ADD_AND_FLUSH(ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY, val);
}

static void ct3a_set_lp_mode(struct exynos_panel *ctx, const struct exynos_panel_mode *pmode)
 {
	struct ct3a_panel *spanel = to_spanel(ctx);

	dev_dbg(ctx->dev, "%s\n", __func__);

	DPU_ATRACE_BEGIN(__func__);

	EXYNOS_DCS_BUF_ADD_SET(ctx, unlock_cmd_f0);

	if (ctx->panel_rev == PANEL_REV_PROTO1_1) {
		EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x02, 0xAE, 0xCB);
		EXYNOS_DCS_BUF_ADD(ctx, 0xCB, 0x11, 0x70);
		EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x00, 0x05, 0xBD);
		EXYNOS_DCS_BUF_ADD(ctx, 0xBD, 0x03);
		EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x00, 0x52, 0x64);
		EXYNOS_DCS_BUF_ADD(ctx, 0x64, 0x01, 0x03, 0x0A, 0x03);
		EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x00, 0x5E, 0xBD);
		EXYNOS_DCS_BUF_ADD(ctx, 0xBD, 0x00, 0x00, 0x00, 0x08, 0x00, 0x74);
	}

	EXYNOS_DCS_BUF_ADD(ctx, 0x53, 0x24);
	EXYNOS_DCS_BUF_ADD(ctx, 0x60, 0x00, 0x00);

	/* Fixed TE */
	if (ctx->panel_rev < PANEL_REV_EVT1)
		EXYNOS_DCS_BUF_ADD(ctx, 0xB9, 0x51, 0x51, 0x00, 0x00);
	else
		EXYNOS_DCS_BUF_ADD(ctx, 0xB9, 0x51);

	/* Enable early exit */
	EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x00, 0x01, 0xBD);
	EXYNOS_DCS_BUF_ADD(ctx, 0xBD, 0x01);

	/* Auto frame insertion: 1Hz */
	EXYNOS_DCS_BUF_ADD(ctx, 0xBD, 0xE5);
	EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x00, 0x19, 0xBD);
	EXYNOS_DCS_BUF_ADD(ctx, 0xBD, 0x00, 0x74);
	EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x00, 0xB8, 0xBD);
	EXYNOS_DCS_BUF_ADD(ctx, 0xBD, 0x00, 0x00, 0x00, 0x08, 0x00, 0x74);
	EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x00, 0xC8, 0xBD);
	EXYNOS_DCS_BUF_ADD(ctx, 0xBD, 0x02, 0x01);
	EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x00, 0x85, 0xBD);
	EXYNOS_DCS_BUF_ADD(ctx, 0xBD, 0x01);
	EXYNOS_DCS_BUF_ADD_SET(ctx, ltps_update);
	EXYNOS_DCS_BUF_ADD_SET_AND_FLUSH(ctx, lock_cmd_f0);

	spanel->hw.vrefresh = 30;
	spanel->hw.te_freq = 30;

	DPU_ATRACE_END(__func__);

	dev_info(ctx->dev, "enter %dhz LP mode\n", drm_mode_vrefresh(&pmode->mode));
}

static void ct3a_set_nolp_mode(struct exynos_panel *ctx,
			      const struct exynos_panel_mode *pmode)
{
	struct ct3a_panel *spanel = to_spanel(ctx);
	u32 idle_vrefresh = spanel->auto_mode_vrefresh;

	if (!is_panel_active(ctx))
		return;

	DPU_ATRACE_BEGIN(__func__);

	EXYNOS_DCS_BUF_ADD_SET(ctx, unlock_cmd_f0);
	/* manual mode */
	EXYNOS_DCS_BUF_ADD(ctx, 0xBD, 0xE1);

	/* Disable early exit */
	EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x00, 0x01, 0xBD);
	EXYNOS_DCS_BUF_ADD(ctx, 0xBD, 0x81);

	/* changeable TE*/
	if (ctx->panel_rev == PANEL_REV_PROTO1)
		EXYNOS_DCS_BUF_ADD(ctx, 0xB9, 0x00, 0x51, 0x00, 0x00);
	else if (ctx->panel_rev == PANEL_REV_PROTO1_1 || ctx->panel_rev == PANEL_REV_PROTO1_2)
		EXYNOS_DCS_BUF_ADD(ctx, 0xB9, 0x04, 0x51, 0x00, 0x00);
	else
		EXYNOS_DCS_BUF_ADD(ctx, 0xB9, 0x04);

	/* AoD off */
	if (ctx->panel_rev == PANEL_REV_PROTO1_1) {
		EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x00, 0x52, 0x64);
		EXYNOS_DCS_BUF_ADD(ctx, 0x64, 0x00);
	}
	ct3a_update_wrctrld(ctx);
	EXYNOS_DCS_BUF_ADD_SET(ctx, ltps_update);
	EXYNOS_DCS_BUF_ADD_SET_AND_FLUSH(ctx, lock_cmd_f0);
	usleep_range(34000, 34010);
	ct3a_set_panel_feat(ctx, pmode, idle_vrefresh, true);
	ct3a_change_frequency(ctx, pmode);

	DPU_ATRACE_END(__func__);

	dev_info(ctx->dev, "exit LP mode\n");
}

static int ct3a_set_op_hz(struct exynos_panel *ctx, unsigned int hz)
{
	const unsigned int vrefresh = drm_mode_vrefresh(&ctx->current_mode->mode);
	struct ct3a_panel *spanel = to_spanel(ctx);

	if (is_vrr_mode(ctx->current_mode)) {
		dev_warn(ctx->dev, "%s: should be set via mode switch\n", __func__);
		return -EINVAL;
	}

	if ((vrefresh > hz) || ((hz != 60) && (hz != 120))) {
		dev_err(ctx->dev, "invalid op_hz=%u for vrefresh=%u\n",
			hz, vrefresh);
		return -EINVAL;
	}

	DPU_ATRACE_BEGIN(__func__);

	ctx->op_hz = hz;
	if (hz == 60)
		set_bit(FEAT_OP_NS, spanel->feat);
	else
		clear_bit(FEAT_OP_NS, spanel->feat);

	if (is_panel_active(ctx))
		ct3a_update_panel_feat(ctx, false);

	dev_info(ctx->dev, "%s op_hz at %d\n",
		is_panel_active(ctx) ? "set" : "cache", hz);

	if (hz == 120) {
		/*
		 * We may transfer the frame for the first TE after switching from
		 * NS to HS mode. The DDIC read speed will change from 60Hz to 120Hz,
		 * but the DPU write speed will remain the same. In this case,
		 * underruns would happen. Waiting for an extra vblank here so that
		 * the frame can be postponed to the next TE to avoid the noises.
		 */
		dev_dbg(ctx->dev, "wait one vblank after NS to HS\n");
		ct3a_wait_one_vblank(ctx);
	}

	DPU_ATRACE_END(__func__);

	return 0;
}

static int ct3a_set_brightness(struct exynos_panel *ctx, u16 br)
{
	u16 brightness;
	struct ct3a_panel *spanel = to_spanel(ctx);

	if (ctx->current_mode->exynos_mode.is_lp_mode) {
		const struct exynos_panel_funcs *funcs;

		/* don't stay at pixel-off state in AOD, or black screen is possibly seen */
		if (spanel->is_pixel_off) {
			EXYNOS_DCS_WRITE_SEQ(ctx, MIPI_DCS_ENTER_NORMAL_MODE);
			spanel->is_pixel_off = false;
		}
		funcs = ctx->desc->exynos_panel_func;
		if (funcs && funcs->set_binned_lp)
			funcs->set_binned_lp(ctx, br);
		return 0;
	}

	/* Use pixel off command instead of setting DBV 0 */
	if (!br) {
		if (!spanel->is_pixel_off) {
			EXYNOS_DCS_WRITE_TABLE(ctx, pixel_off);
			spanel->is_pixel_off = true;
			dev_dbg(ctx->dev, "%s: pixel off instead of dbv 0\n", __func__);
		}
		return 0;
	} else if (br && spanel->is_pixel_off) {
		EXYNOS_DCS_WRITE_SEQ(ctx, MIPI_DCS_ENTER_NORMAL_MODE);
		spanel->is_pixel_off = false;
	}

	brightness = (br & 0xFF) << 8 | br >> 8;

	return exynos_dcs_set_brightness(ctx, brightness);
}

static void ct3a_set_default_voltage(struct exynos_panel *ctx, bool enable)
{
	struct ct3a_panel *spanel = to_spanel(ctx);
	const u8 *vlin_default = spanel->panel_voltage.vlin_default;

	if(ctx->panel_rev < PANEL_REV_EVT1)
		return;
	dev_dbg(ctx->dev, "%s enable = %d\n", __func__, enable);

	EXYNOS_DCS_BUF_ADD_SET(ctx, unlock_cmd_f0);

	if (enable) {
		/* 3.4 VLIN / VGH / VREG Return Setting */
		EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x00, 0x03, 0x48);
		EXYNOS_DCS_BUF_ADD(ctx, 0x48, 0x08);
		EXYNOS_DCS_BUF_ADD(ctx, 0x48, 0xF1);
		EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x00, 0x01, 0x46);
		if(ctx->panel_rev < PANEL_REV_EVT1_1)
			EXYNOS_DCS_BUF_ADD_SET(ctx, vlin_7v7);
		else
			EXYNOS_DCS_BUF_ADD(ctx, 0x46, 0x23, vlin_default[2]);
		EXYNOS_DCS_BUF_ADD(ctx, 0x46, 0x00);
		EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x00, 0x0F, 0xF4);
		EXYNOS_DCS_BUF_ADD_SET(ctx, vgh_7v1);
		EXYNOS_DCS_BUF_ADD(ctx, 0x48, 0x80);
		EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x00, 0x32, 0xF4);
		EXYNOS_DCS_BUF_ADD_SET(ctx, vreg_6v9);
	} else {
		/* 3.3 VGH / VLIN / VREG Setting */
		EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x00, 0x03, 0x48);
		EXYNOS_DCS_BUF_ADD(ctx, 0x48, 0x08);
		EXYNOS_DCS_BUF_ADD(ctx, 0x48, 0xF1);
		EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x00, 0x01, 0x46);
		EXYNOS_DCS_BUF_ADD_SET(ctx, vlin_7v9);
		EXYNOS_DCS_BUF_ADD(ctx, 0x46, 0x00);
		EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x00, 0x0F, 0xF4);
		EXYNOS_DCS_BUF_ADD_SET(ctx, vgh_7v4);
		EXYNOS_DCS_BUF_ADD(ctx, 0x48, 0x80);
		EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x00, 0x32, 0xF4);
		EXYNOS_DCS_BUF_ADD_SET(ctx, vreg_6v9);
	}

	EXYNOS_DCS_BUF_ADD_SET_AND_FLUSH(ctx, lock_cmd_f0);
}

static int ct3a_disable(struct drm_panel *panel)
{
	struct exynos_panel *ctx = container_of(panel, struct exynos_panel, panel);
	struct ct3a_panel *spanel = to_spanel(ctx);
	int ret;

	dev_info(ctx->dev, "%s\n", __func__);

	/* skip disable sequence if going through RR */
	if (ctx->mode_in_progress == MODE_RR_IN_PROGRESS) {
		dev_dbg(ctx->dev, "%s: RRS in progress, skip\n", __func__);
		return 0;
	}

	ct3a_set_default_voltage(ctx, false);
	ret = exynos_panel_disable(panel);
	if (ret)
		return ret;

	/* panel register state gets reset after disabling hardware */
	bitmap_clear(spanel->hw.feat, 0, FEAT_MAX);
	spanel->hw.vrefresh = 60;
	spanel->hw.te_freq = 60;
	spanel->hw.idle_vrefresh = 0;

	return 0;
}

/*
 * 120hz auto mode takes at least 2 frames to start lowering refresh rate in addition to
 * time to next vblank. Use just over 2 frames time to consider worst case scenario
 */
#define EARLY_EXIT_THRESHOLD_US 17000

/**
 * ct3a_update_idle_state - update panel auto frame insertion state
 * @ctx: panel struct
 *
 * - update timestamp of switching to manual mode in case its been a while since the
 *   last frame update and auto mode may have started to lower refresh rate.
 * - trigger early exit by command if it's changeable TE and no switching delay, which
 *   could result in fast 120 Hz boost and seeing 120 Hz TE earlier, otherwise disable
 *   auto refresh mode to avoid lowering frequency too fast.
 */
static void ct3a_update_idle_state(struct exynos_panel *ctx)
{
	s64 delta_us;
	struct ct3a_panel *spanel = to_spanel(ctx);

	ctx->panel_idle_vrefresh = 0;
	if (!test_bit(FEAT_FRAME_AUTO, spanel->feat))
		return;

	delta_us = ktime_us_delta(ktime_get(), ctx->last_commit_ts);
	if (delta_us < EARLY_EXIT_THRESHOLD_US) {
		dev_dbg(ctx->dev, "skip early exit. %lldus since last commit\n",
			delta_us);
		return;
	}

	/* triggering early exit causes a switch to 120hz */
	ctx->last_mode_set_ts = ktime_get();

	DPU_ATRACE_BEGIN(__func__);

	if (!ctx->idle_delay_ms && spanel->force_changeable_te) {
		dev_dbg(ctx->dev, "sending early exit out cmd\n");
		EXYNOS_DCS_BUF_ADD_SET(ctx, unlock_cmd_f0);
		EXYNOS_DCS_BUF_ADD_SET(ctx, ltps_update);
		EXYNOS_DCS_BUF_ADD_SET_AND_FLUSH(ctx, lock_cmd_f0);
	} else {
		/* turn off auto mode to prevent panel from lowering frequency too fast */
		ct3a_update_refresh_mode(ctx, ctx->current_mode, 0);
	}

	DPU_ATRACE_END(__func__);
}

static void ct3a_commit_done(struct exynos_panel *ctx)
{
	if (ctx->current_mode->exynos_mode.is_lp_mode)
		return;

	ct3a_update_idle_state(ctx);
}

static void ct3a_set_hbm_mode(struct exynos_panel *ctx,
				enum exynos_hbm_mode mode)
{
	const bool irc_update = (IS_HBM_ON_IRC_OFF(ctx->hbm_mode) != IS_HBM_ON_IRC_OFF(mode));

	if (mode == ctx->hbm_mode)
		return;

	ctx->hbm_mode = mode;

	if ((ctx->panel_rev >= PANEL_REV_PROTO1_2) && irc_update) {
		EXYNOS_DCS_BUF_ADD_SET(ctx, unlock_cmd_f0);
		EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x00, 0xAF, 0x93);
		EXYNOS_DCS_BUF_ADD(ctx, 0x93, IS_HBM_ON_IRC_OFF(mode) ? 0x0B : 0x2B);
		EXYNOS_DCS_BUF_ADD_SET_AND_FLUSH(ctx, unlock_cmd_f0);
	}

	dev_info(ctx->dev, "hbm_on=%d hbm_ircoff=%d\n", IS_HBM_ON(ctx->hbm_mode),
		 IS_HBM_ON_IRC_OFF(ctx->hbm_mode));
}

static void ct3a_set_dimming_on(struct exynos_panel *exynos_panel,
				bool dimming_on)
{
	const struct exynos_panel_mode *pmode = exynos_panel->current_mode;
	exynos_panel->dimming_on = dimming_on;

	if (pmode->exynos_mode.is_lp_mode) {
		dev_warn(exynos_panel->dev, "in lp mode, skip to update\n");
		return;
	}

	ct3a_update_wrctrld(exynos_panel);
}

static void ct3a_mode_set(struct exynos_panel *ctx,
				const struct exynos_panel_mode *pmode)
{
	ct3a_change_frequency(ctx, pmode);
}

static bool ct3a_is_mode_seamless(const struct exynos_panel *ctx,
					const struct exynos_panel_mode *pmode)
{
	const struct drm_display_mode *c = &ctx->current_mode->mode;
	const struct drm_display_mode *n = &pmode->mode;

	/* seamless mode set can happen if active region resolution is same */
	return (c->vdisplay == n->vdisplay) && (c->hdisplay == n->hdisplay);
}

static void ct3a_debugfs_init(struct drm_panel *panel, struct dentry *root)
{
#ifdef CONFIG_DEBUG_FS
	struct exynos_panel *ctx = container_of(panel, struct exynos_panel, panel);
	struct dentry *panel_root, *csroot;
	struct ct3a_panel *spanel = to_spanel(ctx);

	if (!ctx)
		return;

	panel_root = debugfs_lookup("panel", root);
	if (!panel_root)
		return;

	csroot = debugfs_lookup("cmdsets", panel_root);
	if (!csroot) {
		goto panel_out;
	}

	exynos_panel_debugfs_create_cmdset(ctx, csroot, &ct3a_init_cmd_set, "init");
	debugfs_create_bool("force_fi", 0644, panel_root,
				&spanel->force_fi);

	dput(csroot);
panel_out:
	dput(panel_root);
#endif
}

static void ct3a_get_panel_rev(struct exynos_panel *ctx, u32 id)
{
	/* extract command 0xDB */
	u8 build_code = (id & 0xFF00) >> 8;
	u8 rev = ((build_code & 0xE0) >> 3) | ((build_code & 0x0C) >> 2);

	switch (rev) {
	case 0x00:
		ctx->panel_rev = PANEL_REV_PROTO1;
		break;
	case 0x01:
		ctx->panel_rev = PANEL_REV_PROTO1_1;
		break;
	case 0x02:
		ctx->panel_rev = PANEL_REV_PROTO1_2;
		break;
	case 0x0C:
		ctx->panel_rev = PANEL_REV_EVT1;
		break;
	case 0x0E:
		ctx->panel_rev = PANEL_REV_EVT1_1;
		break;
	case 0x0F:
		ctx->panel_rev = PANEL_REV_EVT1_2;
		break;
	case 0x11:
		ctx->panel_rev = PANEL_REV_DVT1;
		break;
	case 0x12:
		ctx->panel_rev = PANEL_REV_DVT1_1;
		break;
	case 0x14:
		ctx->panel_rev = PANEL_REV_PVT;
		break;
	default:
		dev_warn(ctx->dev,
			 "unknown rev from panel (0x%x), default to latest\n",
			 rev);
		ctx->panel_rev = PANEL_REV_LATEST;
		return;
	}

	dev_info(ctx->dev, "panel_rev: 0x%x\n", ctx->panel_rev);
}

static void ct3a_panel_reset(struct exynos_panel *ctx)
{
	dev_dbg(ctx->dev, "%s +\n", __func__);

	gpiod_set_value(ctx->reset_gpio, 1);
	usleep_range(10000, 10010);

	dev_dbg(ctx->dev, "%s -\n", __func__);

	exynos_panel_init(ctx);
}

static int ct3a_read_default_voltage(struct exynos_panel *ctx)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	struct ct3a_panel *spanel = to_spanel(ctx);
	u8 *vlin = spanel->panel_voltage.vlin_default;
	int ret;

	EXYNOS_DCS_BUF_ADD_SET(ctx, unlock_cmd_f0);
	EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x00, 0x46, 0x48);
	vlin[0] = 0x46;
	vlin[1] = 0x23;
	ret = mipi_dsi_dcs_read(dsi, 0x48, vlin + 2, VLIN_CMD_SIZE - 2);
	if (ret == (VLIN_CMD_SIZE-2)) {
		dev_info(ctx->dev, "%s: vlin: 0x%x\n", __func__, vlin[2]);
	} else {
		vlin[2] = 0x06; /* use vlin 7.7v as default */
		dev_err(ctx->dev, "unable to read vlin\n");
	}
	EXYNOS_DCS_BUF_ADD_SET_AND_FLUSH(ctx, lock_cmd_f0);

	return 0;
}

static int ct3a_read_id(struct exynos_panel *ctx)
{
	int ret = exynos_panel_read_ddic_id(ctx);
	if(ret)
		return ret;

	dev_info(ctx->dev, "%s: 0x%x\n", __func__, ctx->panel_rev);
	if(ctx->panel_rev < PANEL_REV_EVT1_1)
		return 0;

	ct3a_read_default_voltage(ctx);
	return 0;
}

static int ct3a_enable(struct drm_panel *panel)
{
	struct exynos_panel *ctx = container_of(panel, struct exynos_panel, panel);
	const struct exynos_panel_mode *pmode = ctx->current_mode;
	struct drm_dsc_picture_parameter_set pps_payload;

	if (!pmode) {
		dev_err(ctx->dev, "no current mode set\n");
		return -EINVAL;
	}

	dev_info(ctx->dev, "%s +\n", __func__);

	DPU_ATRACE_BEGIN(__func__);

	ct3a_panel_reset(ctx);

	/* TODO: b/277158216, Use 0x9E for PPS setting */
	/* DSC related configuration */
	drm_dsc_pps_payload_pack(&pps_payload, &pps_config);
	EXYNOS_PPS_WRITE_BUF(ctx, &pps_payload);
	EXYNOS_DCS_WRITE_SEQ(ctx, 0x9D, 0x01); /* DSC Enable */

	if(ctx->panel_rev < PANEL_REV_EVT1) {
		EXYNOS_DCS_WRITE_SEQ_DELAY(ctx, 120, MIPI_DCS_EXIT_SLEEP_MODE);
	}
	else {
		EXYNOS_DCS_WRITE_SEQ_DELAY(ctx,  10, MIPI_DCS_EXIT_SLEEP_MODE);
		ct3a_set_default_voltage(ctx, false);
		usleep_range(110000, 110010);
	}

	exynos_panel_send_cmd_set(ctx, &ct3a_init_cmd_set);

	ct3a_update_panel_feat(ctx, true);

	/* dimming and HBM */
	ct3a_update_wrctrld(ctx);

	/* frequency */
	ct3a_change_frequency(ctx, pmode);

	if (pmode->exynos_mode.is_lp_mode)
		ct3a_set_lp_mode(ctx, pmode);

	EXYNOS_DCS_WRITE_SEQ(ctx, MIPI_DCS_SET_DISPLAY_ON);

	ct3a_set_default_voltage(ctx, true);
	dev_info(ctx->dev, "%s -\n", __func__);

	DPU_ATRACE_END(__func__);

	return 0;
}

static int spanel_get_brightness(struct thermal_zone_device *tzd, int *temp)
{
	struct ct3a_panel *spanel;

	if (tzd == NULL)
		return -EINVAL;

	spanel = tzd->devdata;

	if (spanel && spanel->base.bl) {
		mutex_lock(&spanel->base.bl_state_lock);
		*temp = (spanel->base.bl->props.state & BL_STATE_STANDBY) ?
					0 : spanel->base.bl->props.brightness;
		mutex_unlock(&spanel->base.bl_state_lock);
	} else {
		return -EINVAL;
	}

	return 0;
}

static struct thermal_zone_device_ops spanel_tzd_ops = {
	.get_temp = spanel_get_brightness,
};

static void ct3a_panel_init(struct exynos_panel *ctx)
{
	const struct exynos_panel_mode *pmode = ctx->current_mode;

#ifdef PANEL_FACTORY_BUILD
	ctx->panel_idle_enabled = false;
#endif

	/* re-init panel to decouple bootloader settings */
	if (pmode)
		ct3a_set_panel_feat(ctx, pmode, 0, true);
}

static int ct3a_panel_probe(struct mipi_dsi_device *dsi)
{
	struct ct3a_panel *spanel;
	int ret;

	spanel = devm_kzalloc(&dsi->dev, sizeof(*spanel), GFP_KERNEL);
	if (!spanel)
		return -ENOMEM;

	spanel->base.op_hz = 120;
	spanel->is_pixel_off = false;
	spanel->hw.vrefresh = 60;
	spanel->hw.te_freq = 60;
	spanel->tzd = thermal_zone_device_register("inner_brightness",
				0, 0, spanel, &spanel_tzd_ops, NULL, 0, 0);
	if (IS_ERR(spanel->tzd))
		dev_err(spanel->base.dev, "failed to register inner"
			" display thermal zone: %ld", PTR_ERR(spanel->tzd));

	ret = thermal_zone_device_enable(spanel->tzd);
	if (ret) {
		dev_err(spanel->base.dev, "failed to enable inner"
					" display thermal zone ret=%d", ret);
		thermal_zone_device_unregister(spanel->tzd);
	}

	return exynos_panel_common_init(dsi, &spanel->base);
}

static const struct exynos_display_underrun_param underrun_param = {
	.te_idle_us = 350,
	.te_var = 1,
};

static const u32 ct3a_bl_range[] = {
	94, 180, 270, 360, 3307
};

static const int ct3a_vrefresh_range[] = {
	1, 10, 30, 60, 120
};

static const int ct3a_lp_vrefresh_range[] = {
	1, 30
};

static const u16 WIDTH_MM = 147, HEIGHT_MM = 141;

#define CT3A_DSC {\
	.enabled = true,\
	.dsc_count = 1,\
	.slice_count = 2,\
	.slice_height = 173,\
	.cfg = &pps_config,\
}

static const struct exynos_panel_mode ct3a_modes[] = {
/* MRR modes */
#ifdef PANEL_FACTORY_BUILD
	{
		.mode = {
			.name = "2152x2076@1:1",
			DRM_MODE_TIMING(1, 2152, 80, 30, 38, 2076, 6, 4, 14),
			.width_mm = WIDTH_MM,
			.height_mm = HEIGHT_MM,
		},
		.exynos_mode = {
			.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
			.vblank_usec = 120,
			.bpc = 8,
			.dsc = CT3A_DSC,
			.underrun_param = &underrun_param,
		},
		.idle_mode = IDLE_MODE_UNSUPPORTED,
	},
	{
		.mode = {
			.name = "2152x2076@10:10",
			DRM_MODE_TIMING(10, 2152, 80, 30, 38, 2076, 6, 4, 14),
			.width_mm = WIDTH_MM,
			.height_mm = HEIGHT_MM,
		},
		.exynos_mode = {
			.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
			.vblank_usec = 120,
			.bpc = 8,
			.dsc = CT3A_DSC,
			.underrun_param = &underrun_param,
		},
		.idle_mode = IDLE_MODE_UNSUPPORTED,
	},
	{
		.mode = {
			.name = "2152x2076@30:30",
			DRM_MODE_TIMING(30, 2152, 80, 30, 38, 2076, 6, 4, 14),
			.width_mm = WIDTH_MM,
			.height_mm = HEIGHT_MM,
		},
		.exynos_mode = {
			.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
			.vblank_usec = 120,
			.bpc = 8,
			.dsc = CT3A_DSC,
			.underrun_param = &underrun_param,
		},
		.idle_mode = IDLE_MODE_UNSUPPORTED,
	},
#endif
	{
		.mode = {
			.name = "2152x2076@60:60",
			DRM_MODE_TIMING(60, 2152, 80, 30, 38, 2076, 6, 4, 14),
			.width_mm = WIDTH_MM,
			.height_mm = HEIGHT_MM,
			.flags = DRM_MODE_FLAG_BTS_OP_RATE,
			.type = DRM_MODE_TYPE_PREFERRED,
		},
		.exynos_mode = {
			.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
			.vblank_usec = 120,
			.bpc = 8,
			.dsc = CT3A_DSC,
			.underrun_param = &underrun_param,
		},
		.idle_mode = IDLE_MODE_UNSUPPORTED,
	},
	{
		.mode = {
			.name = "2152x2076@120:120",
			DRM_MODE_TIMING(120, 2152, 80, 30, 38, 2076, 6, 4, 14),
			.flags = DRM_MODE_FLAG_BTS_OP_RATE,
			.width_mm = WIDTH_MM,
			.height_mm = HEIGHT_MM,
		},
		.exynos_mode = {
			.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
			.vblank_usec = 120,
			.te_usec = CT3A_TE_USEC_120HZ_HS,
			.bpc = 8,
			.dsc = CT3A_DSC,
			.underrun_param = &underrun_param,
		},
		.idle_mode = IDLE_MODE_UNSUPPORTED,
	},
#ifndef PANEL_FACTORY_BUILD
	/* VRR modes */
	{
		.mode = {
			.name = "2152x2076@120:240",
			DRM_MODE_TIMING(120, 2152, 80, 30, 38, 2076, 6, 4, 14),
			.flags = DRM_MODE_FLAG_TE_FREQ_X2,
			.type = DRM_MODE_TYPE_VRR | DRM_MODE_TYPE_PREFERRED,
			.width_mm = WIDTH_MM,
			.height_mm = HEIGHT_MM,
		},
		.exynos_mode = {
			.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
			.vblank_usec = 120,
			.te_usec = CT3A_TE_USEC_VRR_HS,
			.bpc = 8,
			.dsc = CT3A_DSC,
			.underrun_param = &underrun_param,
		},
		.idle_mode = IDLE_MODE_UNSUPPORTED,
	},
	{
		.mode = {
			.name = "2152x2076@120:120",
			DRM_MODE_TIMING(120, 2152, 80, 30, 38, 2076, 6, 4, 14),
			.flags = DRM_MODE_FLAG_TE_FREQ_X1,
			.type = DRM_MODE_TYPE_VRR,
			.width_mm = WIDTH_MM,
			.height_mm = HEIGHT_MM,
		},
		.exynos_mode = {
			.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
			.vblank_usec = 120,
			.te_usec = CT3A_TE_USEC_VRR_HS,
			.bpc = 8,
			.dsc = CT3A_DSC,
			.underrun_param = &underrun_param,
		},
		.idle_mode = IDLE_MODE_UNSUPPORTED,
	},
	{
		.mode = {
			.name = "2152x2076@60:240",
			DRM_MODE_TIMING(60, 2152, 80, 30, 38, 2076, 6, 4, 14),
			.flags = DRM_MODE_FLAG_TE_FREQ_X4 | DRM_MODE_FLAG_NS,
			.type = DRM_MODE_TYPE_VRR,
			.width_mm = WIDTH_MM,
			.height_mm = HEIGHT_MM,
		},
		.exynos_mode = {
			.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
			.vblank_usec = 120,
			.te_usec = CT3A_TE_USEC_VRR_NS,
			.bpc = 8,
			.dsc = CT3A_DSC,
			.underrun_param = &underrun_param,
		},
		.idle_mode = IDLE_MODE_UNSUPPORTED,
	},
#endif
};

const struct brightness_capability ct3a_brightness_capability = {
	.normal = {
		.nits = {
			.min = 2,
			.max = 1000,
		},
		.level = {
			.min = 174,
			.max = 3307,
		},
		.percentage = {
			.min = 0,
			.max = 63,
		},
	},
	.hbm = {
		.nits = {
			.min = 1000,
			.max = 1600,
		},
		.level = {
			.min = 3308,
			.max = 4095,
		},
		.percentage = {
			.min = 63,
			.max = 100,
		},
	},
};

static const struct exynos_panel_mode ct3a_lp_mode = {
	.mode = {
		.name = "2152x2076@30:30",
		DRM_MODE_TIMING(30, 2152, 80, 32, 36, 2076, 6, 4, 14),
		.width_mm = WIDTH_MM,
		.height_mm = HEIGHT_MM,
	},
	.exynos_mode = {
		.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
		.vblank_usec = 120,
		.bpc = 8,
		.dsc = CT3A_DSC,
		.underrun_param = &underrun_param,
		.is_lp_mode = true,
	}
};

static const struct drm_panel_funcs ct3a_drm_funcs = {
	.disable = ct3a_disable,
	.unprepare = exynos_panel_unprepare,
	.prepare = exynos_panel_prepare,
	.enable = ct3a_enable,
	.get_modes = exynos_panel_get_modes,
	.debugfs_init = ct3a_debugfs_init,
};


static const struct exynos_brightness_configuration ct3a_btr_configs[] = {
	{
		.panel_rev = PANEL_REV_EVT1_1 | PANEL_REV_LATEST,
		.dft_brightness = 1223,    /* 140 nits brightness */
		.brt_capability = {
			.normal = {
				.nits = {
					.min = 2,
					.max = 1000,
				},
				.level = {
					.min = 157,
					.max = 2988,
				},
				.percentage = {
					.min = 0,
					.max = 63,
				},
			},
			.hbm = {
				.nits = {
					.min = 1000,
					.max = 1600,
				},
				.level = {
					.min = 2989,
					.max = 3701,
				},
				.percentage = {
					.min = 63,
					.max = 100,
				},
			},
		},
	},
	{
		.panel_rev = PANEL_REV_PROTO1 | PANEL_REV_PROTO1_1 | PANEL_REV_PROTO1_2 | PANEL_REV_EVT1,
		.dft_brightness = 1353,    /* 140 nits brightness */
		.brt_capability = {
			.normal = {
				.nits = {
					.min = 2,
					.max = 1000,
				},
				.level = {
					.min = 174,
					.max = 3307,
				},
				.percentage = {
					.min = 0,
					.max = 63,
				},
			},
			.hbm = {
				.nits = {
					.min = 1000,
					.max = 1600,
				},
				.level = {
					.min = 3308,
					.max = 4095,
				},
				.percentage = {
					.min = 63,
					.max = 100,
				},
			},
		},
	},
};

static int ct3a_panel_config(struct exynos_panel *ctx);

static const struct exynos_panel_funcs ct3a_exynos_funcs = {
	.set_brightness = ct3a_set_brightness,
	.set_lp_mode = ct3a_set_lp_mode,
	.set_nolp_mode = ct3a_set_nolp_mode,
	.set_binned_lp = exynos_panel_set_binned_lp,
	.set_dimming_on = ct3a_set_dimming_on,
	.set_hbm_mode = ct3a_set_hbm_mode,
	.update_te2 = ct3a_update_te2,
	.commit_done = ct3a_commit_done,
	.atomic_check = ct3a_atomic_check,
	.set_self_refresh = ct3a_set_self_refresh,
	.refresh_ctrl = ct3a_refresh_ctrl,
	.set_op_hz = ct3a_set_op_hz,
	.is_mode_seamless = ct3a_is_mode_seamless,
	.mode_set = ct3a_mode_set,
	.get_panel_rev = ct3a_get_panel_rev,
	.read_id = ct3a_read_id,
	.panel_init = ct3a_panel_init,
	.panel_config = ct3a_panel_config,
};

struct exynos_panel_desc google_ct3a = {
	.data_lane_cnt = 4,
	/* supported HDR format bitmask : 1(DOLBY_VISION), 2(HDR10), 3(HLG) */
	.hdr_formats = BIT(2) | BIT(3),
	.max_luminance = 10000000,
	.max_avg_luminance = 1200000,
	.min_luminance = 5,
	.bl_range = ct3a_bl_range,
	.modes = ct3a_modes,
	.num_modes = ARRAY_SIZE(ct3a_modes),
	.vrefresh_range = ct3a_vrefresh_range,
	.vrefresh_range_count = ARRAY_SIZE(ct3a_vrefresh_range),
	.lp_mode = &ct3a_lp_mode,
	.lp_vrefresh_range = ct3a_lp_vrefresh_range,
	.lp_vrefresh_range_count = ARRAY_SIZE(ct3a_lp_vrefresh_range),
	.binned_lp = ct3a_binned_lp,
	.num_binned_lp = ARRAY_SIZE(ct3a_binned_lp),
	.is_panel_idle_supported = true,
	.off_cmd_set = &ct3a_off_cmd_set,
	.panel_func = &ct3a_drm_funcs,
	.exynos_panel_func = &ct3a_exynos_funcs,
	.reg_ctrl_enable = {
		{PANEL_REG_ID_VDDI, 1},
		{PANEL_REG_ID_VCI,  0},
		{PANEL_REG_ID_VDDR, 10},
	},
	.reg_ctrl_disable = {
		{PANEL_REG_ID_VDDR, 0},
		{PANEL_REG_ID_VDDI, 1},
		{PANEL_REG_ID_VCI, 15},
	},
};

static int ct3a_panel_config(struct exynos_panel *ctx)
{
	int ret;
	/* exynos_panel_model_init(ctx, PROJECT, 0); */

	ret = exynos_panel_init_brightness(&google_ct3a,
						ct3a_btr_configs,
						ARRAY_SIZE(ct3a_btr_configs),
						ctx->panel_rev);

	return ret;
}

static const struct of_device_id exynos_panel_of_match[] = {
	{ .compatible = "google,ct3a", .data = &google_ct3a },
	{ },
};

static struct mipi_dsi_driver exynos_panel_driver = {
	.probe = ct3a_panel_probe,
	.remove = exynos_panel_remove,
	.driver = {
		.name = "panel-google-ct3a",
		.of_match_table = exynos_panel_of_match,
	},
};
module_mipi_dsi_driver(exynos_panel_driver);

MODULE_AUTHOR("Leo Chen <yinchiuan@google.com>");
MODULE_DESCRIPTION("MIPI-DSI based Google ct3a panel driver");
MODULE_LICENSE("GPL");
