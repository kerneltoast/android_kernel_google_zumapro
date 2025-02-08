/* SPDX-License-Identifier: MIT */

#include <drm/drm_vblank.h>
#include <drm/display/drm_dsc_helper.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/thermal.h>
#include <video/mipi_display.h>

#include "trace/panel_trace.h"

#include "gs_panel/drm_panel_funcs_defaults.h"
#include "gs_panel/gs_panel.h"
#include "gs_panel/gs_panel_funcs_defaults.h"

#define VLIN_CMD_SIZE 3

/* one frame durtion(us) at 30Hz */
#define DELAY_30HZ_ONE_FRAME 34000

static const u8 vlin_7v7[] = { 0x46, 0x23, 0x06 };
static const u8 vlin_7v9[] = { 0x46, 0x23, 0x02 };
static const u8 vgh_7v1[]  = { 0xF4, 0x15, 0x15, 0x15, 0x15 };
static const u8 vgh_7v4[]  = { 0xF4, 0x18, 0x18, 0x18, 0x18 };
static const u8 vreg_6v9[] = { 0xF4, 0x18 };

/**
 * struct ct3a_panel - panel specific runtime info
 *
 * This struct maintains ct3a panel specific runtime info, any fixed details about panel
 * should most likely go into struct gs_panel_desc
 */
struct ct3a_panel {
	/** @base: base panel struct */
	struct gs_panel base;

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
	/** @panel_voltage: panel default voltage
	 *			1st byte: the fixed address 0x46.
	 *			2nd byte: the fixed data 0x23.
	 *			3th byte: read from panel.
	 */
	struct panel_voltage {
		u8 vlin_default[VLIN_CMD_SIZE];
	} panel_voltage;
};

#define to_spanel(ctx) container_of(ctx, struct ct3a_panel, base)

static const struct drm_dsc_config pps_config = {
		.line_buf_depth = 9,
		.bits_per_component = 8,
		.convert_rgb = true,
		.slice_width = 1076,
		.slice_height = 173,
		.slice_count = 2,
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

static const struct gs_dsi_cmd ct3a_lp_low_cmds[] = {
	/* AOD Low Mode, 10nit */
	GS_DSI_CMD(0x51, 0x03, 0x8A),
};

static const struct gs_dsi_cmd ct3a_lp_high_cmds[] = {
	/* AOD Low Mode, 10nit */
	GS_DSI_CMD(0x51, 0x07, 0xFF),
};

static const struct gs_binned_lp ct3a_binned_lp[] = {
	/* low threshold 40 nits */
	BINNED_LP_MODE("low", 689, ct3a_lp_low_cmds),
	BINNED_LP_MODE("high", 2988, ct3a_lp_high_cmds)
};

static const struct gs_dsi_cmd ct3a_off_cmds[] = {
	GS_DSI_CMD(MIPI_DCS_SET_DISPLAY_OFF),
	GS_DSI_DELAY_CMD(120, MIPI_DCS_ENTER_SLEEP_MODE),
};
static DEFINE_GS_CMDSET(ct3a_off);

static const struct gs_dsi_cmd ct3a_init_cmds[] = {
	GS_DSI_CMD(MIPI_DCS_SET_TEAR_ON),

	/* CASET: 2151 */
	GS_DSI_CMD(MIPI_DCS_SET_COLUMN_ADDRESS, 0x00, 0x00, 0x08, 0x67),
	/* PASET: 2075 */
	GS_DSI_CMD(MIPI_DCS_SET_PAGE_ADDRESS, 0x00, 0x00, 0x08, 0x1B),

	GS_DSI_CMDLIST(unlock_cmd_f0),
	/* manual TE, fixed TE2 setting */
	GS_DSI_REV_CMD(PANEL_REV_PROTO1, 0xB9, 0x00, 0x51, 0x00, 0x00),
	GS_DSI_REV_CMD(PANEL_REV_PROTO1_1|PANEL_REV_PROTO1_2, 0xB9, 0x04, 0x51, 0x00, 0x00),
	GS_DSI_REV_CMD(PANEL_REV_GE(PANEL_REV_EVT1), 0xB9, 0x04),
	GS_DSI_CMD(0xB0, 0x00, 0x08, 0xB9),
	GS_DSI_CMD(0xB9, 0x08, 0x1C, 0x00, 0x00, 0x08, 0x1C, 0x00, 0x00),
	GS_DSI_REV_CMD(PANEL_REV_GE(PANEL_REV_EVT1), 0xB0, 0x00, 0x01, 0xB9),
	GS_DSI_REV_CMD(PANEL_REV_GE(PANEL_REV_EVT1), 0xB9, 0x51),
	GS_DSI_CMD(0xB0, 0x00, 0x22, 0xB9),
	GS_DSI_CMD(0xB9, 0x00, 0x2F, 0x00, 0x82, 0x00, 0x2F, 0x00, 0x82),

	/* early exit off */
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_PROTO1_2), 0xB0, 0x00, 0x01, 0xBD),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_PROTO1_2), 0xBD, 0x81),
	GS_DSI_CMDLIST(ltps_update),

	/* gamma improvement setting */
	GS_DSI_CMD(0xB0, 0x00, 0x24, 0xF8),
	GS_DSI_CMD(0xF8, 0x05),

	/* HLPM transition preset*/
	GS_DSI_REV_CMD(PANEL_REV_GE(PANEL_REV_EVT1), 0xB0, 0x00, 0x03, 0xBB),
	GS_DSI_REV_CMD(PANEL_REV_GE(PANEL_REV_EVT1), 0xBB, 0x45, 0x0E),

	GS_DSI_CMDLIST(lock_cmd_f0),
};
static DEFINE_GS_CMDSET(ct3a_init);

static u8 ct3a_get_te2_option(struct gs_panel *ctx)
{
	struct ct3a_panel *spanel = to_spanel(ctx);

	if (!ctx || !ctx->current_mode || spanel->force_changeable_te2)
		return TEX_OPT_CHANGEABLE;

	if (ctx->current_mode->gs_mode.is_lp_mode ||
	    (test_bit(FEAT_EARLY_EXIT, ctx->sw_status.feat) &&
		spanel->auto_mode_vrefresh < 30))
		return TEX_OPT_FIXED;

	return TEX_OPT_CHANGEABLE;
}

static void ct3a_update_te2(struct gs_panel *ctx)
{
	ctx->te2.option = ct3a_get_te2_option(ctx);

	dev_dbg(ctx->dev,
		"TE2 updated: op=%d, is_changeable=%d, idle=%d\n",
		test_bit(FEAT_OP_NS, ctx->sw_status.feat), (ctx->te2.option == TEX_OPT_CHANGEABLE),
		ctx->idle_data.panel_idle_vrefresh);
}

static inline bool is_auto_mode_allowed(struct gs_panel *ctx)
{
	/* don't want to enable auto mode/early exit during dimming on */
	if (ctx->dimming_on)
		return false;

	if (ctx->idle_data.idle_delay_ms) {
		const unsigned int delta_ms = gs_panel_get_idle_time_delta(ctx);

		if (delta_ms < ctx->idle_data.idle_delay_ms)
			return false;
	}

	return ctx->idle_data.panel_idle_enabled;
}

static u32 ct3a_get_min_idle_vrefresh(struct gs_panel *ctx,
				     const struct gs_panel_mode *pmode)
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
 * @ctx: gs_panel struct
 * @pmode: gs_panel_mode struct, target panel mode
 * @idle_vrefresh: target vrefresh rate in auto mode, 0 if disabling auto mode
 * @enforce: force to write all of registers even if no feature state changes
 *
 * Configure panel features based on the context.
 */
static void ct3a_set_panel_feat(struct gs_panel *ctx,
	const struct gs_panel_mode *pmode, u32 idle_vrefresh, bool enforce)
{
	struct ct3a_panel *spanel = to_spanel(ctx);
	struct device *dev = ctx->dev;
	unsigned long *feat = ctx->sw_status.feat;
	u32 vrefresh = drm_mode_vrefresh(&pmode->mode);
	u32 te_freq = gs_drm_mode_te_freq(&pmode->mode);
	bool is_vrr = gs_is_vrr_mode(pmode);
	u8 val;
	DECLARE_BITMAP(changed_feat, FEAT_MAX);

#ifndef PANEL_FACTORY_BUILD
	vrefresh = 1;
	idle_vrefresh = 0;
	set_bit(FEAT_EARLY_EXIT, feat);
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
		bitmap_xor(changed_feat, feat, ctx->hw_status.feat, FEAT_MAX);
		if (bitmap_empty(changed_feat, FEAT_MAX) &&
			vrefresh == ctx->hw_status.vrefresh &&
			idle_vrefresh == ctx->hw_status.idle_vrefresh &&
			te_freq == ctx->hw_status.te.rate_hz) {
			dev_dbg(ctx->dev, "%s: no changes, skip update\n", __func__);
			return;
		}
	}

	dev_dbg(ctx->dev,
		"op=%d ee=%d fi=%d fps=%u idle_fps=%u te=%u vrr=%d\n",
		test_bit(FEAT_OP_NS, feat), test_bit(FEAT_EARLY_EXIT, feat),
		test_bit(FEAT_FRAME_AUTO, feat),
		vrefresh, idle_vrefresh, te_freq, is_vrr);

	GS_DCS_BUF_ADD_CMDLIST(dev, unlock_cmd_f0);
	/* TE setting */
	ctx->sw_status.te.rate_hz = te_freq;
	if (test_bit(FEAT_EARLY_EXIT, changed_feat) ||
		test_bit(FEAT_OP_NS, changed_feat) || ctx->hw_status.te.rate_hz != te_freq) {
		if (test_bit(FEAT_EARLY_EXIT, feat) && !spanel->force_changeable_te) {
			if (is_vrr && te_freq == 240) {
				/* 240Hz multi TE */
				GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x08, 0xB9);
				if (test_bit(FEAT_OP_NS, feat))
					GS_DCS_BUF_ADD_CMD(dev, 0xB9, 0x08, 0x1C, 0x00, 0x00, 0x01,
							0xC6, 0x00, 0x01);
				else
					GS_DCS_BUF_ADD_CMD(dev, 0xB9, 0x08, 0x1C, 0x00, 0x00, 0x03,
							0xE0, 0x00, 0x01);
				GS_DCS_BUF_ADD_CMD(dev, 0xB9, 0x61);
			} else {
				if (ctx->panel_rev < PANEL_REV_EVT1)
					GS_DCS_BUF_ADD_CMD(dev, 0xB9, 0x51, 0x51, 0x00, 0x00);
				else
					GS_DCS_BUF_ADD_CMD(dev, 0xB9, 0x51);

				GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x02, 0xB9);
				/* Fixed TE */
				if (test_bit(FEAT_OP_NS, feat) || te_freq != 60) {
					GS_DCS_BUF_ADD_CMD(dev, 0xB9, 0x00);
				} else {
					GS_DCS_BUF_ADD_CMD(dev, 0xB9, 0x01);
				}
			}
			ctx->hw_status.te.option = TEX_OPT_FIXED;
		} else {
			/* Changeable TE */
			if (ctx->panel_rev == PANEL_REV_PROTO1)
				GS_DCS_BUF_ADD_CMD(dev, 0xB9, 0x00, 0x51, 0x00, 0x00);
			else if (ctx->panel_rev == PANEL_REV_PROTO1_1 ||
					    ctx->panel_rev == PANEL_REV_PROTO1_2)
				GS_DCS_BUF_ADD_CMD(dev, 0xB9, 0x04, 0x51, 0x00, 0x00);
			else
				GS_DCS_BUF_ADD_CMD(dev, 0xB9, 0x04);
			ctx->hw_status.te.option = TEX_OPT_CHANGEABLE;
		}
	}

	/*
	 * Early-exit: enable or disable
	 */
	if (is_vrr) {
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x01, 0xBD);
		GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x41);
	} else {
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x01, 0xBD);
		val = test_bit(FEAT_EARLY_EXIT, feat) ? 0x01 : 0x81;
		GS_DCS_BUF_ADD_CMD(dev, 0xBD, val);
	}

	/*
	 * Frequency setting: FI, frequency, idle frequency
	 *
	 * Description: this sequence possibly overrides some configs early-exit
	 * and operation set, depending on FI mode.
	 */
	if (test_bit(FEAT_FRAME_AUTO, feat)) {
		if (test_bit(FEAT_OP_NS, feat))
			GS_DCS_BUF_ADD_CMD(dev, 0x60, 0x18);
		else
			GS_DCS_BUF_ADD_CMD(dev, 0x60, 0x00);
		/* frame insertion on */
		GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0xE3);
		/* target frequency */
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x13, 0xBD);
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
			GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x00, val);
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
			GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x00, val);
		}
		/* step setting */
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x9E, 0xBD);
		if (test_bit(FEAT_OP_NS, feat))
			GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x00, 0x00, 0x00, 0x04, 0x00, 0x14, 0x00, 0x00);
		else
			GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x00, 0x00, 0x00, 0x02, 0x00, 0x06, 0x00, 0x16);

		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0xAE, 0xBD);
		if (test_bit(FEAT_OP_NS, feat))
			GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x00, 0x02, 0x00, 0x00);
		else
			GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x00, 0x00, 0x02, 0x00);

		if (ctx->panel_rev >= PANEL_REV_PROTO1_2) {
			if (test_bit(FEAT_OP_NS, feat)) {
				if (ctx->panel_rev == PANEL_REV_PROTO1_2)
					GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x85);
				else
					GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x85, 0xBD);
			} else {
				if (ctx->panel_rev == PANEL_REV_PROTO1_2)
					GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x83);
				else
					GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x83, 0xBD);
			}
			GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x00);
		}
	} else { /* manual */
		if (!is_vrr) {
			GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0xE1);
		}
		if (test_bit(FEAT_OP_NS, feat)) {
			if(is_vrr) {
				GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x83, 0xBD);
				GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x00);
				GS_DCS_BUF_ADD_CMD(dev, 0xB9, 0x61);
			}

			GS_DCS_BUF_ADD_CMD(dev, 0xF2, 0x01);

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
			GS_DCS_BUF_ADD_CMD(dev, 0xF2, 0x01);

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
			GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, 0x60, val);
		else
			GS_DCS_BUF_ADD_CMD(dev, 0x60, val);
	}
	/* TODO: b/321871919 - send in the same batch */
	if(is_vrr)
		GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, ltps_update);
	else
		GS_DCS_BUF_ADD_CMDLIST(dev, ltps_update);
	GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, lock_cmd_f0);

	ctx->hw_status.vrefresh = vrefresh;
	ctx->hw_status.idle_vrefresh = idle_vrefresh;
	ctx->hw_status.te.rate_hz = te_freq;
	bitmap_copy(ctx->hw_status.feat, feat, FEAT_MAX);
}

/**
 * ct3a_update_panel_feat - configure panel features with current refresh rate
 * @ctx: gs_panel struct
 * @enforce: force to write all of registers even if no feature state changes
 *
 * Configure panel features based on the context without changing current refresh rate
 * and idle setting.
 */
static void ct3a_update_panel_feat(struct gs_panel *ctx, bool enforce)
{
	const struct gs_panel_mode *pmode = ctx->current_mode;
	struct ct3a_panel *spanel = to_spanel(ctx);
	u32 idle_vrefresh = spanel->auto_mode_vrefresh;

	ct3a_set_panel_feat(ctx, pmode, idle_vrefresh, enforce);
}

static void ct3a_update_refresh_mode(struct gs_panel *ctx,
				const struct gs_panel_mode *pmode,
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
	ctx->idle_data.panel_idle_vrefresh = idle_vrefresh;
	ct3a_set_panel_feat(ctx, pmode, idle_vrefresh, false);
	notify_panel_mode_changed(ctx);

	dev_dbg(ctx->dev, "%s: display state is notified\n", __func__);
}

static void ct3a_change_frequency(struct gs_panel *ctx,
				const struct gs_panel_mode *pmode)
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

	if (pmode->idle_mode == GIDLE_MODE_ON_INACTIVITY)
		idle_vrefresh = ct3a_get_min_idle_vrefresh(ctx, pmode);

	ct3a_update_refresh_mode(ctx, pmode, idle_vrefresh);
	ctx->sw_status.te.rate_hz = gs_drm_mode_te_freq(&pmode->mode);

	dev_dbg(ctx->dev, "%s: change to %uHz\n", __func__, vrefresh);
}

static void ct3a_panel_idle_notification(struct gs_panel *ctx,
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

static void ct3a_wait_one_vblank(struct gs_panel *ctx)
{
	struct drm_crtc *crtc = NULL;

	if (ctx->gs_connector->base.state)
		crtc = ctx->gs_connector->base.state->crtc;

	PANEL_ATRACE_BEGIN(__func__);
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
	PANEL_ATRACE_END(__func__);
}

static bool ct3a_set_self_refresh(struct gs_panel *ctx, bool enable)
{
	const struct gs_panel_mode *pmode = ctx->current_mode;
	struct ct3a_panel *spanel = to_spanel(ctx);
	u32 idle_vrefresh;

	dev_dbg(ctx->dev, "%s: %d\n", __func__, enable);

	if (unlikely(!pmode))
		return false;

	/* self refresh is not supported in lp mode since that always makes use of early exit */
	if (pmode->gs_mode.is_lp_mode) {
		/* set 1Hz while self refresh is active, otherwise clear it */
		ctx->idle_data.panel_idle_vrefresh = enable ? 1 : 0;
		notify_panel_mode_changed(ctx);
		return false;
	}

	idle_vrefresh = ct3a_get_min_idle_vrefresh(ctx, pmode);

	if (pmode->idle_mode != GIDLE_MODE_ON_SELF_REFRESH) {
		/*
		 * if idle mode is on inactivity, may need to update the target fps for auto mode,
		 * or switch to manual mode if idle should be disabled (idle_vrefresh=0)
		 */
		if ((pmode->idle_mode == GIDLE_MODE_ON_INACTIVITY) &&
			(spanel->auto_mode_vrefresh != idle_vrefresh)) {
			ct3a_update_refresh_mode(ctx, pmode, idle_vrefresh);
			return true;
		}
		return false;
	}

	if (!enable)
		idle_vrefresh = 0;

	/* if there's no change in idle state then skip cmds */
	if (ctx->idle_data.panel_idle_vrefresh == idle_vrefresh)
		return false;

	PANEL_ATRACE_BEGIN(__func__);
	ct3a_update_refresh_mode(ctx, pmode, idle_vrefresh);

	if (idle_vrefresh) {
		const int vrefresh = drm_mode_vrefresh(&pmode->mode);

		ct3a_panel_idle_notification(ctx, 0, vrefresh, 120);
	} else if (ctx->idle_data.panel_need_handle_idle_exit) {
		/*
		 * after exit idle mode with fixed TE at non-120hz, TE may still keep at 120hz.
		 * If any layer that already be assigned to DPU that can't be handled at 120hz,
		 * panel_need_handle_idle_exit will be set then we need to wait one vblank to
		 * avoid underrun issue.
		 */
		dev_dbg(ctx->dev, "wait one vblank after exit idle\n");
		ct3a_wait_one_vblank(ctx);
	}

	PANEL_ATRACE_END(__func__);

	return true;
}

static int ct3a_atomic_check(struct gs_panel *ctx, struct drm_atomic_state *state)
{
	struct drm_connector *conn = &ctx->gs_connector->base;
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

static void ct3a_update_wrctrld(struct gs_panel *ctx)
{
	struct device *dev = ctx->dev;
	u8 val = CT3A_WRCTRLD_BCTRL_BIT;

	if (ctx->dimming_on)
		val |= CT3A_WRCTRLD_DIMMING_BIT;

	dev_dbg(dev,
		"%s(wrctrld:0x%x, hbm=%d, dimming=%d)\n",
		__func__, val, GS_IS_HBM_ON(ctx->hbm_mode), ctx->dimming_on);

	GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, MIPI_DCS_WRITE_CONTROL_DISPLAY, val);
}

static void ct3a_set_lp_mode(struct gs_panel *ctx, const struct gs_panel_mode *pmode)
 {
	struct device *dev = ctx->dev;

	dev_dbg(ctx->dev, "%s\n", __func__);

	PANEL_ATRACE_BEGIN(__func__);

	GS_DCS_BUF_ADD_CMDLIST(dev, unlock_cmd_f0);

	if (ctx->panel_rev == PANEL_REV_PROTO1_1) {
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x02, 0xAE, 0xCB);
		GS_DCS_BUF_ADD_CMD(dev, 0xCB, 0x11, 0x70);
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x05, 0xBD);
		GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x03);
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x52, 0x64);
		GS_DCS_BUF_ADD_CMD(dev, 0x64, 0x01, 0x03, 0x0A, 0x03);
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x5E, 0xBD);
		GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x00, 0x00, 0x00, 0x08, 0x00, 0x74);
	}

	GS_DCS_BUF_ADD_CMD(dev, 0x53, 0x24);
	GS_DCS_BUF_ADD_CMD(dev, 0x60, 0x00, 0x00);

	/* Fixed TE */
	if (ctx->panel_rev < PANEL_REV_EVT1)
		GS_DCS_BUF_ADD_CMD(dev, 0xB9, 0x51, 0x51, 0x00, 0x00);
	else
		GS_DCS_BUF_ADD_CMD(dev, 0xB9, 0x51);

	/* Enable early exit */
	GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x01, 0xBD);
	GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x01);

	/* Auto frame insertion: 1Hz */
	GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0xE5);
	GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x19, 0xBD);
	GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x00, 0x74);
	GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0xB8, 0xBD);
	GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x00, 0x00, 0x00, 0x08, 0x00, 0x74);
	GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0xC8, 0xBD);
	GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x02, 0x01);
	GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x85, 0xBD);
	GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x01);
	GS_DCS_BUF_ADD_CMDLIST(dev, ltps_update);
	GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, lock_cmd_f0);

	ctx->hw_status.vrefresh = 30;
	ctx->sw_status.te.rate_hz = 30;
	ctx->hw_status.te.rate_hz = 30;

	PANEL_ATRACE_END(__func__);

	dev_info(ctx->dev, "enter %dhz LP mode\n", drm_mode_vrefresh(&pmode->mode));
}

static void ct3a_set_nolp_mode(struct gs_panel *ctx,
			      const struct gs_panel_mode *pmode)
{
	struct ct3a_panel *spanel = to_spanel(ctx);
	struct device *dev = ctx->dev;
	u32 idle_vrefresh = spanel->auto_mode_vrefresh;

	if (!gs_is_panel_active(ctx))
		return;

	PANEL_ATRACE_BEGIN(__func__);

	GS_DCS_BUF_ADD_CMDLIST(dev, unlock_cmd_f0);
	/* manual mode */
	GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0xE1);

	/* Disable early exit */
	GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x01, 0xBD);
	GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x81);

	/* changeable TE*/
	if (ctx->panel_rev == PANEL_REV_PROTO1)
		GS_DCS_BUF_ADD_CMD(dev, 0xB9, 0x00, 0x51, 0x00, 0x00);
	else if (ctx->panel_rev == PANEL_REV_PROTO1_1 || ctx->panel_rev == PANEL_REV_PROTO1_2)
		GS_DCS_BUF_ADD_CMD(dev, 0xB9, 0x04, 0x51, 0x00, 0x00);
	else
		GS_DCS_BUF_ADD_CMD(dev, 0xB9, 0x04);

	/* AoD off */
	if (ctx->panel_rev == PANEL_REV_PROTO1_1) {
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x52, 0x64);
		GS_DCS_BUF_ADD_CMD(dev, 0x64, 0x00);
	}
	ct3a_update_wrctrld(ctx);
	GS_DCS_BUF_ADD_CMDLIST(dev, ltps_update);
	GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, lock_cmd_f0);
	usleep_range(DELAY_30HZ_ONE_FRAME, DELAY_30HZ_ONE_FRAME + 10);
	ct3a_set_panel_feat(ctx, pmode, idle_vrefresh, true);
	ct3a_change_frequency(ctx, pmode);

	PANEL_ATRACE_END(__func__);

	dev_info(ctx->dev, "exit LP mode\n");
}

static int ct3a_set_op_hz(struct gs_panel *ctx, unsigned int hz)
{
	const unsigned int vrefresh = drm_mode_vrefresh(&ctx->current_mode->mode);

	if (gs_is_vrr_mode(ctx->current_mode)) {
		dev_warn(ctx->dev, "set_op_hz: should be set by mode switch when in vrr mode\n");
		return -EINVAL;
	}

	if ((vrefresh > hz) || ((hz != 60) && (hz != 120))) {
		dev_err(ctx->dev, "invalid op_hz=%u for vrefresh=%u\n",
			hz, vrefresh);
		return -EINVAL;
	}

	PANEL_ATRACE_BEGIN(__func__);

	ctx->op_hz = hz;
	if (hz == 60)
		set_bit(FEAT_OP_NS, ctx->sw_status.feat);
	else
		clear_bit(FEAT_OP_NS, ctx->sw_status.feat);

	if (gs_is_panel_active(ctx))
		ct3a_update_panel_feat(ctx, false);

	dev_info(ctx->dev, "%s op_hz at %d\n",
		gs_is_panel_active(ctx) ? "set" : "cache", hz);

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

	PANEL_ATRACE_END(__func__);

	return 0;
}

static int ct3a_set_brightness(struct gs_panel *ctx, u16 br)
{
	struct device *dev = ctx->dev;
	u16 brightness;
	struct ct3a_panel *spanel = to_spanel(ctx);

	if (ctx->current_mode->gs_mode.is_lp_mode) {

		/* don't stay at pixel-off state in AOD, or black screen is possibly seen */
		if (spanel->is_pixel_off) {
			GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, MIPI_DCS_ENTER_NORMAL_MODE);
			spanel->is_pixel_off = false;
		}

		if (gs_panel_has_func(ctx, set_binned_lp))
			ctx->desc->gs_panel_func->set_binned_lp(ctx, br);
		return 0;
	}

	/* Use pixel off command instead of setting DBV 0 */
	if (!br) {
		if (!spanel->is_pixel_off) {
			GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, pixel_off);
			spanel->is_pixel_off = true;
			dev_dbg(ctx->dev, "%s: pixel off instead of dbv 0\n", __func__);
		}
		return 0;
	} else if (br && spanel->is_pixel_off) {
		GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, MIPI_DCS_ENTER_NORMAL_MODE);
		spanel->is_pixel_off = false;
	}

	brightness = swab16(br);

	return gs_dcs_set_brightness(ctx, brightness);
}

static void ct3a_set_default_voltage(struct gs_panel *ctx, bool enable)
{
	struct ct3a_panel *spanel = to_spanel(ctx);
	struct device *dev = ctx->dev;
	const u8 *vlin_default = spanel->panel_voltage.vlin_default;

	if(ctx->panel_rev < PANEL_REV_EVT1)
		return;
	dev_dbg(dev, "%s enable = %d\n", __func__, enable);

	GS_DCS_BUF_ADD_CMDLIST(dev, unlock_cmd_f0);

	if (enable) {
		/* 3.4 VLIN / VGH / VREG Return Setting */
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x03, 0x48);
		GS_DCS_BUF_ADD_CMD(dev, 0x48, 0x08);
		GS_DCS_BUF_ADD_CMD(dev, 0x48, 0xF1);
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x01, 0x46);
		if(ctx->panel_rev < PANEL_REV_EVT1_1)
			GS_DCS_BUF_ADD_CMDLIST(dev, vlin_7v7);
		else
			GS_DCS_BUF_ADD_CMD(dev, 0x46, 0x23, vlin_default[2]);
		GS_DCS_BUF_ADD_CMD(dev, 0x46, 0x00);
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x0F, 0xF4);
		GS_DCS_BUF_ADD_CMDLIST(dev, vgh_7v1);
		GS_DCS_BUF_ADD_CMD(dev, 0x48, 0x80);
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x32, 0xF4);
		GS_DCS_BUF_ADD_CMDLIST(dev, vreg_6v9);
	} else {
		/* 3.3 VGH / VLIN / VREG Setting */
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x03, 0x48);
		GS_DCS_BUF_ADD_CMD(dev, 0x48, 0x08);
		GS_DCS_BUF_ADD_CMD(dev, 0x48, 0xF1);
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x01, 0x46);
		GS_DCS_BUF_ADD_CMDLIST(dev, vlin_7v9);
		GS_DCS_BUF_ADD_CMD(dev, 0x46, 0x00);
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x0F, 0xF4);
		GS_DCS_BUF_ADD_CMDLIST(dev, vgh_7v4);
		GS_DCS_BUF_ADD_CMD(dev, 0x48, 0x80);
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x32, 0xF4);
		GS_DCS_BUF_ADD_CMDLIST(dev, vreg_6v9);
	}

	GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, lock_cmd_f0);
}

static int ct3a_disable(struct drm_panel *panel)
{
	struct gs_panel *ctx = container_of(panel, struct gs_panel, base);
	int ret;

	dev_info(ctx->dev, "%s\n", __func__);

	/* skip disable sequence if going through RR */
	if (ctx->mode_in_progress == MODE_RR_IN_PROGRESS) {
		dev_dbg(ctx->dev, "%s: RRS in progress, skip\n", __func__);
		return 0;
	}

	ct3a_set_default_voltage(ctx, false);
	ret = gs_panel_disable(panel);
	if (ret)
		return ret;

	/* panel register state gets reset after disabling hardware */
	bitmap_clear(ctx->hw_status.feat, 0, FEAT_MAX);
	ctx->hw_status.vrefresh = 60;
	ctx->sw_status.te.rate_hz = 60;
	ctx->hw_status.te.rate_hz = 60;
	ctx->hw_status.idle_vrefresh = 0;

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
static void ct3a_update_idle_state(struct gs_panel *ctx)
{
	struct device *dev = ctx->dev;
	s64 delta_us;
	struct ct3a_panel *spanel = to_spanel(ctx);

	ctx->idle_data.panel_idle_vrefresh = 0;
	if (!test_bit(FEAT_FRAME_AUTO, ctx->sw_status.feat))
		return;

	delta_us = ktime_us_delta(ktime_get(), ctx->timestamps.last_commit_ts);
	if (delta_us < EARLY_EXIT_THRESHOLD_US) {
		dev_dbg(dev, "skip early exit. %lldus since last commit\n",
			delta_us);
		return;
	}

	/* triggering early exit causes a switch to 120hz */
	ctx->timestamps.last_mode_set_ts = ktime_get();

	PANEL_ATRACE_BEGIN(__func__);

	if (!ctx->idle_data.idle_delay_ms && spanel->force_changeable_te) {
		dev_dbg(dev, "sending early exit out cmd\n");
		GS_DCS_BUF_ADD_CMDLIST(dev, unlock_cmd_f0);
		GS_DCS_BUF_ADD_CMDLIST(dev, ltps_update);
		GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, lock_cmd_f0);
	} else {
		/* turn off auto mode to prevent panel from lowering frequency too fast */
		ct3a_update_refresh_mode(ctx, ctx->current_mode, 0);
	}

	PANEL_ATRACE_END(__func__);
}

static void ct3a_commit_done(struct gs_panel *ctx)
{
	if (ctx->current_mode->gs_mode.is_lp_mode)
		return;

	ct3a_update_idle_state(ctx);
}

static void ct3a_set_hbm_mode(struct gs_panel *ctx, enum gs_hbm_mode mode)
{
	struct device *dev = ctx->dev;
	const bool irc_update = (GS_IS_HBM_ON_IRC_OFF(ctx->hbm_mode) != GS_IS_HBM_ON_IRC_OFF(mode));

	if (mode == ctx->hbm_mode)
		return;

	ctx->hbm_mode = mode;

	if ((ctx->panel_rev >= PANEL_REV_PROTO1_2) && irc_update) {
		GS_DCS_BUF_ADD_CMDLIST(dev, unlock_cmd_f0);
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0xAF, 0x93);
		GS_DCS_BUF_ADD_CMD(dev, 0x93, GS_IS_HBM_ON_IRC_OFF(mode) ? 0x0B : 0x2B);
		GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, unlock_cmd_f0);
	}

	dev_info(dev, "hbm_on=%d hbm_ircoff=%d\n", GS_IS_HBM_ON(ctx->hbm_mode),
		 GS_IS_HBM_ON_IRC_OFF(ctx->hbm_mode));
}

static void ct3a_set_dimming_on(struct gs_panel *gs_panel,
				bool dimming_on)
{
	const struct gs_panel_mode *pmode = gs_panel->current_mode;
	gs_panel->dimming_on = dimming_on;

	if (pmode->gs_mode.is_lp_mode) {
		dev_warn(gs_panel->dev, "in lp mode; skip updating dimming_on\n");
		return;
	}

	ct3a_update_wrctrld(gs_panel);
}

static void ct3a_mode_set(struct gs_panel *ctx, const struct gs_panel_mode *pmode)
{
	ct3a_change_frequency(ctx, pmode);
}

static bool ct3a_is_mode_seamless(const struct gs_panel *ctx,
			const struct gs_panel_mode *pmode)
{
	const struct drm_display_mode *c = &ctx->current_mode->mode;
	const struct drm_display_mode *n = &pmode->mode;

	/* seamless mode set can happen if active region resolution is same */
	return (c->vdisplay == n->vdisplay) && (c->hdisplay == n->hdisplay);
}

static void ct3a_debugfs_init(struct drm_panel *panel, struct dentry *root)
{
#ifdef CONFIG_DEBUG_FS
	struct gs_panel *ctx = container_of(panel, struct gs_panel, base);
	struct dentry *panel_root, *csroot;

	if (!ctx)
		return;

	panel_root = debugfs_lookup("panel", root);
	if (!panel_root)
		return;

	csroot = debugfs_lookup("cmdsets", panel_root);
	if (!csroot) {
		goto panel_out;
	}

	gs_panel_debugfs_create_cmdset(csroot, &ct3a_init_cmdset, "init");
	dput(csroot);
panel_out:
	dput(panel_root);
#endif
}

static void ct3a_get_panel_rev(struct gs_panel *ctx, u32 id)
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

static int ct3a_read_default_voltage(struct gs_panel *ctx)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	struct ct3a_panel *spanel = to_spanel(ctx);
	struct device *dev = ctx->dev;
	u8 *vlin = spanel->panel_voltage.vlin_default;
	int ret;

	GS_DCS_BUF_ADD_CMDLIST(dev, unlock_cmd_f0);
	GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x46, 0x48);
	vlin[0] = 0x46;
	vlin[1] = 0x23;
	ret = mipi_dsi_dcs_read(dsi, 0x48, vlin + 2, VLIN_CMD_SIZE - 2);
	if (ret == (VLIN_CMD_SIZE-2)) {
		dev_info(dev, "%s: vlin: 0x%x\n", __func__, vlin[2]);
	} else {
		vlin[2] = 0x06; /* use vlin 7.7v as default */
		dev_err(dev, "unable to read vlin\n");
	}
	GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, lock_cmd_f0);

	return 0;
}

static int ct3a_read_id(struct gs_panel *ctx)
{
	int ret = gs_panel_read_slsi_ddic_id(ctx);
	if(ret)
		return ret;

	dev_dbg(ctx->dev, "%s: 0x%x\n", __func__, ctx->panel_rev);
	if(ctx->panel_rev < PANEL_REV_EVT1_1)
		return 0;

	ct3a_read_default_voltage(ctx);
	return 0;
}

static int ct3a_enable(struct drm_panel *panel)
{
	struct gs_panel *ctx = container_of(panel, struct gs_panel, base);
	const struct gs_panel_mode *pmode = ctx->current_mode;
	struct device *dev = ctx->dev;

	if (!pmode) {
		dev_err(ctx->dev, "no current mode set\n");
		return -EINVAL;
	}

	dev_info(ctx->dev, "%s +\n", __func__);

	PANEL_ATRACE_BEGIN(__func__);

	gs_panel_reset_helper(ctx);

	/* TODO: b/277158216, Use 0x9E for PPS setting */
	/* DSC related configuration */
	gs_dcs_write_dsc_config(dev, &pps_config);
	GS_DCS_WRITE_CMD(dev, 0x9D, 0x01); /* DSC Enable */

	if(ctx->panel_rev < PANEL_REV_EVT1) {
		GS_DCS_WRITE_DELAY_CMD(dev, 120, MIPI_DCS_EXIT_SLEEP_MODE);
	}
	else {
		GS_DCS_WRITE_DELAY_CMD(dev,  10, MIPI_DCS_EXIT_SLEEP_MODE);
		ct3a_set_default_voltage(ctx, false);
		usleep_range(110000, 110010);
	}

	gs_panel_send_cmdset(ctx, &ct3a_init_cmdset);

	ct3a_update_panel_feat(ctx, true);

	/* dimming and HBM */
	ct3a_update_wrctrld(ctx);

	/* frequency */
	ct3a_change_frequency(ctx, pmode);

	if (pmode->gs_mode.is_lp_mode)
		ct3a_set_lp_mode(ctx, pmode);

	GS_DCS_WRITE_CMD(dev, MIPI_DCS_SET_DISPLAY_ON);

	ct3a_set_default_voltage(ctx, true);
	dev_info(ctx->dev, "%s -\n", __func__);

	PANEL_ATRACE_END(__func__);

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

static void ct3a_panel_init(struct gs_panel *ctx)
{
	const struct gs_panel_mode *pmode = ctx->current_mode;

#ifdef PANEL_FACTORY_BUILD
	ctx->idle_data.panel_idle_enabled = false;
#endif

	/* re-init panel to decouple bootloader settings */
	if (pmode)
		ct3a_set_panel_feat(ctx, pmode, 0, true);
}

static int ct3a_panel_probe(struct mipi_dsi_device *dsi)
{
	struct ct3a_panel *spanel;
	struct gs_panel *ctx;
	int ret;

	spanel = devm_kzalloc(&dsi->dev, sizeof(*spanel), GFP_KERNEL);
	if (!spanel)
		return -ENOMEM;

	ctx = &spanel->base;

	ctx->thermal = devm_kzalloc(&dsi->dev, sizeof(*ctx->thermal), GFP_KERNEL);
	if (!ctx->thermal) {
		devm_kfree(&dsi->dev, spanel);
		return -ENOMEM;
	}

	spanel->base.op_hz = 120;
	spanel->is_pixel_off = false;
	ctx->hw_status.vrefresh = 60;
	ctx->hw_status.te.rate_hz = 60;
	clear_bit(FEAT_ZA, ctx->hw_status.feat);

	ctx->thermal->tz = thermal_zone_device_register("inner_brightness",
				0, 0, spanel, &spanel_tzd_ops, NULL, 0, 0);
	if (IS_ERR(ctx->thermal->tz))
		dev_err(ctx->dev, "failed to register inner"
			" display thermal zone: %ld", PTR_ERR(ctx->thermal->tz));

	ret = thermal_zone_device_enable(ctx->thermal->tz);
	if (ret) {
		dev_err(ctx->dev, "failed to enable inner"
					" display thermal zone ret=%d", ret);
		thermal_zone_device_unregister(ctx->thermal->tz);
	}

	return gs_dsi_panel_common_init(dsi, ctx);
}

static const struct gs_display_underrun_param underrun_param = {
	.te_idle_us = 350,
	.te_var = 1,
};

static const u32 ct3a_bl_range[] = {
	94, 180, 270, 360, 3307
};

static const u16 WIDTH_MM = 147, HEIGHT_MM = 141;

#define CT3A_DSC {\
	.enabled = true,\
	.dsc_count = 1,\
	.cfg = &pps_config,\
}

static const struct gs_panel_mode_array ct3a_modes = {
	.num_modes = 5,
	.modes = {
/* MRR modes */
#ifdef PANEL_FACTORY_BUILD
		{
			.mode = {
				.name = "2152x2076@1:1",
				DRM_MODE_TIMING(1, 2152, 80, 30, 38, 2076, 6, 4, 14),
				.width_mm = WIDTH_MM,
				.height_mm = HEIGHT_MM,
			},
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.bpc = 8,
				.dsc = CT3A_DSC,
				.underrun_param = &underrun_param,
			},
			.idle_mode = GIDLE_MODE_UNSUPPORTED,
		},
		{
			.mode = {
				.name = "2152x2076@10:10",
				DRM_MODE_TIMING(10, 2152, 80, 30, 38, 2076, 6, 4, 14),
				.width_mm = WIDTH_MM,
				.height_mm = HEIGHT_MM,
			},
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.bpc = 8,
				.dsc = CT3A_DSC,
				.underrun_param = &underrun_param,
			},
			.idle_mode = GIDLE_MODE_UNSUPPORTED,
		},
		{
			.mode = {
				.name = "2152x2076@30:30",
				DRM_MODE_TIMING(30, 2152, 80, 30, 38, 2076, 6, 4, 14),
				.width_mm = WIDTH_MM,
				.height_mm = HEIGHT_MM,
			},
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.bpc = 8,
				.dsc = CT3A_DSC,
				.underrun_param = &underrun_param,
			},
			.idle_mode = GIDLE_MODE_UNSUPPORTED,
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
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.bpc = 8,
				.dsc = CT3A_DSC,
				.underrun_param = &underrun_param,
			},
			.idle_mode = GIDLE_MODE_UNSUPPORTED,
		},
		{
			.mode = {
				.name = "2152x2076@120:120",
				DRM_MODE_TIMING(120, 2152, 80, 30, 38, 2076, 6, 4, 14),
				.flags = DRM_MODE_FLAG_BTS_OP_RATE,
				.width_mm = WIDTH_MM,
				.height_mm = HEIGHT_MM,
			},
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.te_usec = CT3A_TE_USEC_120HZ_HS,
				.bpc = 8,
				.dsc = CT3A_DSC,
				.underrun_param = &underrun_param,
			},
			.idle_mode = GIDLE_MODE_UNSUPPORTED,
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
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.te_usec = CT3A_TE_USEC_VRR_HS,
				.bpc = 8,
				.dsc = CT3A_DSC,
				.underrun_param = &underrun_param,
			},
			.idle_mode = GIDLE_MODE_UNSUPPORTED,
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
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.te_usec = CT3A_TE_USEC_VRR_HS,
				.bpc = 8,
				.dsc = CT3A_DSC,
				.underrun_param = &underrun_param,
			},
			.idle_mode = GIDLE_MODE_UNSUPPORTED,
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
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.te_usec = CT3A_TE_USEC_VRR_NS,
				.bpc = 8,
				.dsc = CT3A_DSC,
				.underrun_param = &underrun_param,
			},
			.idle_mode = GIDLE_MODE_UNSUPPORTED,
		},
#endif
	},/* modes */
};

static const struct gs_panel_mode_array ct3a_lp_mode = {
	.num_modes = 1,
	.modes = {
		{
			.mode = {
				.name = "2152x2076@30:30",
				DRM_MODE_TIMING(30, 2152, 80, 32, 36, 2076, 6, 4, 14),
				.width_mm = WIDTH_MM,
				.height_mm = HEIGHT_MM,
			},
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.bpc = 8,
				.dsc = CT3A_DSC,
				.underrun_param = &underrun_param,
				.is_lp_mode = true,
			}
		},
	},
};

static const struct drm_panel_funcs ct3a_drm_funcs = {
	.disable = ct3a_disable,
	.unprepare = gs_panel_unprepare,
	.prepare = gs_panel_prepare,
	.enable = ct3a_enable,
	.get_modes = gs_panel_get_modes,
	.debugfs_init = ct3a_debugfs_init,
};


static const struct gs_brightness_configuration ct3a_btr_configs[] = {
	{
		.panel_rev = PANEL_REV_EVT1_1 | PANEL_REV_LATEST,
		.default_brightness = 1223,    /* 140 nits brightness */
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
		.default_brightness = 1353,    /* 140 nits brightness */
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

static struct gs_panel_brightness_desc ct3a_brightness_desc = {
	.max_luminance = 10000000,
	.max_avg_luminance = 1200000,
	.min_luminance = 5,
};

static int ct3a_panel_config(struct gs_panel *ctx)
{
	/* b/300383405 Currently, we can't support multiple
	 *  displays in `display_layout_configuration.xml`.
	 */
	/* gs_panel_model_init(ctx, PROJECT, 0); */

	return gs_panel_update_brightness_desc(&ct3a_brightness_desc, ct3a_btr_configs,
						ARRAY_SIZE(ct3a_btr_configs), ctx->panel_rev);
}


static const struct gs_panel_funcs ct3a_gs_funcs = {
	.set_brightness = ct3a_set_brightness,
	.set_lp_mode = ct3a_set_lp_mode,
	.set_nolp_mode = ct3a_set_nolp_mode,
	.set_binned_lp = gs_panel_set_binned_lp_helper,
	.set_dimming = ct3a_set_dimming_on,
	.set_hbm_mode = ct3a_set_hbm_mode,
	.update_te2 = ct3a_update_te2,
	.commit_done = ct3a_commit_done,
	.atomic_check = ct3a_atomic_check,
	.set_self_refresh = ct3a_set_self_refresh,
	.set_op_hz = ct3a_set_op_hz,
	.is_mode_seamless = ct3a_is_mode_seamless,
	.mode_set = ct3a_mode_set,
	.get_panel_rev = ct3a_get_panel_rev,
	.read_id = ct3a_read_id,
	.panel_init = ct3a_panel_init,
	.panel_config = ct3a_panel_config,
};

static struct gs_panel_reg_ctrl_desc ct3a_reg_ctrl_desc = {
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

struct gs_panel_desc gs_ct3a = {
	.data_lane_cnt = 4,
	.brightness_desc = &ct3a_brightness_desc,
	.reg_ctrl_desc = &ct3a_reg_ctrl_desc,
	/* supported HDR format bitmask : 1(DOLBY_VISION), 2(HDR10), 3(HLG) */
	.hdr_formats = BIT(2) | BIT(3),
	.bl_range = ct3a_bl_range,
	.modes = &ct3a_modes,
	.lp_modes = &ct3a_lp_mode,
	.binned_lp = ct3a_binned_lp,
	.num_binned_lp = ARRAY_SIZE(ct3a_binned_lp),
	.is_idle_supported = true,
	.off_cmdset = &ct3a_off_cmdset,
	.panel_func = &ct3a_drm_funcs,
	.gs_panel_func = &ct3a_gs_funcs,
	.reset_timing_ms = { -1, 1, 10 },
};

static const struct of_device_id gs_panel_of_match[] = {
	{ .compatible = "google,gs-ct3a", .data = &gs_ct3a },
	{ },
};

static struct mipi_dsi_driver gs_panel_driver = {
	.probe = ct3a_panel_probe,
	.remove = gs_dsi_panel_common_remove,
	.driver = {
		.name = "panel-gs-ct3a",
		.of_match_table = gs_panel_of_match,
	},
};
module_mipi_dsi_driver(gs_panel_driver);

MODULE_AUTHOR("Weizhung Ding <weizhungding@google.com>");
MODULE_DESCRIPTION("MIPI-DSI based Google ct3a panel driver");
MODULE_LICENSE("Dual MIT/GPL");
