// SPDX-License-Identifier: GPL-2.0-only
/*
 * MIPI-DSI based tk4a AMOLED LCD panel driver.
 *
 * Copyright (c) 2023 Google LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <drm/display/drm_dsc_helper.h>
#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <video/mipi_display.h>

#include "trace/dpu_trace.h"
#include "trace/panel_trace.h"

#include "gs_panel/drm_panel_funcs_defaults.h"
#include "gs_panel/gs_panel.h"
#include "gs_panel/gs_panel_funcs_defaults.h"

static const struct drm_dsc_config pps_config = {
	.line_buf_depth = 9,
	.bits_per_component = 8,
	.convert_rgb = true,
	.slice_width = 1080,
	.slice_count = 1,
	.slice_height = 24,
	.simple_422 = false,
	.pic_width = 1080,
	.pic_height = 2424,
	.rc_tgt_offset_high = 3,
	.rc_tgt_offset_low = 3,
	.bits_per_pixel = 128,
	.rc_edge_factor = 6,
	.rc_quant_incr_limit1 = 11,
	.rc_quant_incr_limit0 = 11,
	.initial_xmit_delay = 512,
	.initial_dec_delay = 796,
	.block_pred_enable = true,
	.first_line_bpg_offset = 12,
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
		{.range_min_qp = 5, .range_max_qp = 11, .range_bpg_offset = 54},
		{.range_min_qp = 5, .range_max_qp = 12, .range_bpg_offset = 52},
		{.range_min_qp = 5, .range_max_qp = 15, .range_bpg_offset = 52},
		{.range_min_qp = 7, .range_max_qp = 13, .range_bpg_offset = 52},
		{.range_min_qp = 13, .range_max_qp = 15, .range_bpg_offset = 52}
	},
	.rc_model_size = 8192,
	.flatness_min_qp = 3,
	.flatness_max_qp = 12,
	.initial_scale_value = 32,
	.scale_decrement_interval = 15,
	.scale_increment_interval = 786,
	.nfl_bpg_offset = 1069,
	.slice_bpg_offset = 543,
	.final_offset = 4336,
	.vbr_enable = false,
	.slice_chunk_size = 1080,
	.dsc_version_minor = 1,
	.dsc_version_major = 1,
	.native_422 = false,
	.native_420 = false,
	.second_line_bpg_offset = 0,
	.nsl_bpg_offset = 0,
	.second_line_offset_adj = 0,
};

#define TK4A_WRCTRLD_DIMMING_BIT    0x08
#define TK4A_WRCTRLD_BCTRL_BIT      0x20

static const u8 test_key_enable[] = { 0xF0, 0x5A, 0x5A };
static const u8 test_key_disable[] = { 0xF0, 0xA5, 0xA5 };
static const u8 ltps_update[] = { 0xF7, 0x0F };
static const u8 pixel_off[] = { 0x22 };

static const struct gs_dsi_cmd tk4a_off_cmds[] = {
	GS_DSI_DELAY_CMD(MIPI_DCS_SET_DISPLAY_OFF),
	GS_DSI_DELAY_CMD(120, MIPI_DCS_ENTER_SLEEP_MODE),
};
static DEFINE_GS_CMDSET(tk4a_off);

static const struct gs_dsi_cmd tk4a_lp_cmds[] = {
	GS_DSI_CMD(MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x24),
};
static DEFINE_GS_CMDSET(tk4a_lp);

static const struct gs_dsi_cmd tk4a_lp_night_cmd[] = {
	GS_DSI_CMD(0x51, 0x00, 0xB8),
};

static const struct gs_dsi_cmd tk4a_lp_low_cmd[] = {
	GS_DSI_CMD(0x51, 0x01, 0x7E),
};
static const struct gs_dsi_cmd tk4a_lp_high_cmd[] = {
	GS_DSI_CMD(0x51, 0x03, 0x1A),
};

static const struct gs_binned_lp tk4a_binned_lp[] = {
	/* night threshold 4 nits */
	BINNED_LP_MODE_TIMING("night", 252, tk4a_lp_night_cmd, 12, 12 + 50),
	/* low threshold 40 nits */
	BINNED_LP_MODE_TIMING("low", 716, tk4a_lp_low_cmd, 12, 12 + 50),
	BINNED_LP_MODE_TIMING("high", 4095, tk4a_lp_high_cmd, 12, 12 + 50),
};

static const struct gs_dsi_cmd tk4a_init_cmds[] = {
	/* TE on */
	GS_DSI_CMD(MIPI_DCS_SET_TEAR_ON),

	/* TE width setting */
	GS_DSI_CMDLIST(test_key_enable),
	GS_DSI_CMD(0xB9, 0x01), /* 120HS, 60HS, AOD */
	GS_DSI_CMDLIST(test_key_disable),

	/* TE2 setting */
	GS_DSI_CMDLIST(test_key_enable),
	GS_DSI_CMD(0xB0, 0x00, 0x69, 0xCB),
	GS_DSI_CMD(0xCB, 0x10, 0x00, 0x2D), /* 60HS TE2 ON */
	GS_DSI_CMD(0xB0, 0x00, 0xE9, 0xCB),
	GS_DSI_CMD(0xCB, 0x10, 0x00, 0x2D), /* 120HS & 90HS TE2 ON */
	GS_DSI_CMD(0xB0, 0x01, 0x69, 0xCB),
	GS_DSI_CMD(0xCB, 0x10, 0x00, 0x2D), /* AOD TE2 ON */
	GS_DSI_CMDLIST(ltps_update),
	GS_DSI_CMDLIST(test_key_disable),

	/* CASET: 1080 */
	GS_DSI_CMD(MIPI_DCS_SET_COLUMN_ADDRESS, 0x00, 0x00, 0x04, 0x37),

	/* PASET: 2424 */
	GS_DSI_CMD(MIPI_DCS_SET_PAGE_ADDRESS, 0x00, 0x00, 0x09, 0x77),

	/* FFC 756Mbps @ fosc 180Mhz */
	GS_DSI_CMDLIST(test_key_enable),
	GS_DSI_CMD(0xFC, 0x5A, 0x5A),
	GS_DSI_CMD(0xB0, 0x00, 0x2A, 0xC5),
	GS_DSI_CMD(0xC5, 0x0D, 0x10, 0x80, 0x05),
	GS_DSI_CMD(0xB0, 0x00, 0x2E, 0xC5),
	GS_DSI_CMD(0xC5, 0x79, 0xE8),
	GS_DSI_CMD(0xFC, 0xA5, 0xA5),
	GS_DSI_CMDLIST(test_key_disable),

	/* FREQ CON Set */
	GS_DSI_CMDLIST(test_key_enable),
	GS_DSI_CMD(0xB0, 0x00, 0x27, 0xF2),
	GS_DSI_CMD(0xF2, 0x02),
	GS_DSI_CMDLIST(ltps_update),
	GS_DSI_CMDLIST(test_key_disable),
};
static DEFINE_GS_CMDSET(tk4a_init);

/**
 * struct tk4a_panel - panel specific runtime info
 *
 * This struct maintains tk4a panel specific runtime info, any fixed details about panel
 * should most likely go into struct gs_panel_desc
 */
struct tk4a_panel {
	/** @base: base panel struct */
	struct gs_panel base;
	/**
	 * @is_pixel_off: pixel-off command is sent to panel. Only sending normal-on or resetting
	 *		  panel can recover to normal mode after entering pixel-off state.
	 */
	bool is_pixel_off;
};
#define to_spanel(ctx) container_of(ctx, struct tk4a_panel, base)

static void tk4a_change_frequency(struct gs_panel *ctx,
									const struct gs_panel_mode *pmode)
{
	struct device *dev = ctx->dev;
	u32 vrefresh = drm_mode_vrefresh(&pmode->mode);

	if (unlikely(!ctx))
		return;

	if (vrefresh != 60 && vrefresh != 120) {
		dev_warn(ctx->dev, "%s: invalid refresh rate %uhz\n", __func__, vrefresh);
		return;
	}

	GS_DCS_BUF_ADD_CMDLIST(dev, test_key_enable);
	GS_DCS_BUF_ADD_CMD(dev, 0x60, vrefresh == 60 ? 0x00 : 0x08, 0x00);
	GS_DCS_BUF_ADD_CMDLIST(dev, ltps_update);
	GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, test_key_disable);

	dev_info(dev, "%s: change to %uHz\n", __func__, vrefresh);
	return;
}

static void tk4a_update_wrctrld(struct gs_panel *ctx)
{
	struct device *dev = ctx->dev;
	u8 val = TK4A_WRCTRLD_BCTRL_BIT;

	if (ctx->dimming_on)
		val |= TK4A_WRCTRLD_DIMMING_BIT;

	dev_dbg(dev,
		"%s(wrctrld:0x%x, hbm: %s, dimming: %s)\n",
		__func__, val, GS_IS_HBM_ON(ctx->hbm_mode) ? "on" : "off",
		ctx->dimming_on ? "on" : "off");

	GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, MIPI_DCS_WRITE_CONTROL_DISPLAY, val);
}

static int tk4a_set_brightness(struct gs_panel *ctx, u16 br)
{
	u16 brightness;
	u32 max_brightness;
	struct tk4a_panel *spanel = to_spanel(ctx);
	struct device *dev = ctx->dev;

	if (ctx->current_mode && ctx->current_mode->gs_mode.is_lp_mode) {
		/* don't stay at pixel-off state in AOD, or black screen is possibly seen */
		if (spanel->is_pixel_off) {
			GS_DCS_WRITE_CMD(dev, MIPI_DCS_ENTER_NORMAL_MODE);
			spanel->is_pixel_off = false;
		}
		if (gs_panel_has_func(ctx, set_binned_lp))
			ctx->desc->gs_panel_func->set_binned_lp(ctx, br);
		return 0;
	}

	/* Use pixel off command instead of setting DBV 0 */
	if (!br) {
		if (!spanel->is_pixel_off) {
			GS_DCS_WRITE_CMDLIST(dev, pixel_off);
			spanel->is_pixel_off = true;
			dev_dbg(dev, "%s: pixel off instead of dbv 0\n", __func__);
		}
		return 0;
	} else if (br && spanel->is_pixel_off) {
		GS_DCS_WRITE_CMD(dev, MIPI_DCS_ENTER_NORMAL_MODE);
		spanel->is_pixel_off = false;
	}

	if (!ctx->desc->brightness_desc->brt_capability) {
		dev_err(dev, "no available brightness capability\n");
		return -EINVAL;
	}

	max_brightness = ctx->desc->brightness_desc->brt_capability->hbm.level.max;

	if (br > max_brightness) {
		br = max_brightness;
		dev_warn(dev, "%s: capped to dbv(%d)\n", __func__,
			max_brightness);
	}

	/* swap endianness because panel expects MSB first */
	brightness = swab16(br);

	return gs_dcs_set_brightness(ctx, brightness);
}

static void tk4a_set_hbm_mode(struct gs_panel *ctx,
				enum gs_hbm_mode mode)
{
	struct device *dev = ctx->dev;

	ctx->hbm_mode = mode;

	/* FGZ mode */
	GS_DCS_BUF_ADD_CMDLIST(dev, test_key_enable);
	GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x01, 0x22, 0x68);
	if (GS_IS_HBM_ON_IRC_OFF(ctx->hbm_mode)) {
		if (ctx->panel_rev == PANEL_REV_EVT1)
			GS_DCS_BUF_ADD_CMD(dev, 0x68, 0x1C, 0xE3, 0xFF, 0x94); /* FGZ Mode ON */
		else
			GS_DCS_BUF_ADD_CMD(dev, 0x68, 0x28, 0xED, 0xFF, 0x94); /* FGZ Mode ON */;
	} else {
		GS_DCS_BUF_ADD_CMD(dev, 0x68, 0x00, 0x00, 0xFF, 0x90); /* FGZ Mode OFF */
	}
	GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, test_key_disable);

	dev_info(dev, "hbm_on=%d hbm_ircoff=%d.\n", GS_IS_HBM_ON(ctx->hbm_mode),
		 GS_IS_HBM_ON_IRC_OFF(ctx->hbm_mode));
}

static void tk4a_set_dimming_on(struct gs_panel *gs_panel,
                                bool dimming_on)
{
	const struct gs_panel_mode *pmode = gs_panel->current_mode;

	gs_panel->dimming_on = dimming_on;

	if (pmode->gs_mode.is_lp_mode) {
		dev_warn(gs_panel->dev, "in lp mode, skip to update dimming usage\n");
		return;
	}

	tk4a_update_wrctrld(gs_panel);
}

static void tk4a_mode_set(struct gs_panel *ctx,
                          const struct gs_panel_mode *pmode)
{
	tk4a_change_frequency(ctx, pmode);
}

static bool tk4a_is_mode_seamless(const struct gs_panel *ctx,
                                  const struct gs_panel_mode *pmode)
{
	/* seamless mode switch is possible if only changing refresh rate */
	return drm_mode_equal_no_clocks(&ctx->current_mode->mode, &pmode->mode);
}

static void tk4a_debugfs_init(struct drm_panel *panel, struct dentry *root)
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

	gs_panel_debugfs_create_cmdset(csroot, &tk4a_init_cmdset, "init");

	dput(csroot);
panel_out:
	dput(panel_root);
#endif
}

static void tk4a_get_panel_rev(struct gs_panel *ctx, u32 id)
{
	/* extract command 0xDB */
	u8 build_code = (id & 0xFF00) >> 8;
	u8 main = (build_code & 0xE0) >> 3;
	u8 sub = (build_code & 0x0C) >> 2;
	u8 rev = main | sub;

	gs_panel_get_panel_rev(ctx, rev);
}

static int tk4a_atomic_check(struct gs_panel *ctx, struct drm_atomic_state *state)
{
	struct drm_connector *conn = &ctx->gs_connector->base;
	struct drm_connector_state *new_conn_state = drm_atomic_get_new_connector_state(state, conn);
	struct drm_crtc_state *old_crtc_state, *new_crtc_state;
	const struct gs_panel_mode *pmode;
	bool was_lp_mode, is_lp_mode;

	if (!ctx->current_mode || drm_mode_vrefresh(&ctx->current_mode->mode) == 120 ||
		!new_conn_state || !new_conn_state->crtc)
		return 0;

	new_crtc_state = drm_atomic_get_new_crtc_state(state, new_conn_state->crtc);
	old_crtc_state = drm_atomic_get_old_crtc_state(state, new_conn_state->crtc);
	if (!old_crtc_state || !new_crtc_state || !new_crtc_state->active)
		return 0;

	was_lp_mode = ctx->current_mode->gs_mode.is_lp_mode;
	/* don't skip update when switching between AoD and normal mode */
	pmode = gs_panel_get_mode(ctx, &new_crtc_state->mode);
	if (pmode) {
		is_lp_mode = pmode->gs_mode.is_lp_mode;
		if ((was_lp_mode && !is_lp_mode) || (!was_lp_mode && is_lp_mode))
			new_crtc_state->color_mgmt_changed = true;
	} else {
		dev_err(ctx->dev, "%s: no new mode\n", __func__);
	}

	if (!drm_atomic_crtc_effectively_active(old_crtc_state) ||
		(was_lp_mode && drm_mode_vrefresh(&new_crtc_state->mode) == 60)) {
		struct drm_display_mode *mode = &new_crtc_state->adjusted_mode;

		/* set clock to max refresh rate on resume or AOD exit to 60Hz */
		mode->clock = mode->htotal * mode->vtotal * 120 / 1000;
		if (mode->clock != new_crtc_state->mode.clock) {
			new_crtc_state->mode_changed = true;
			ctx->gs_connector->needs_commit = true;
			dev_dbg(ctx->dev, "raise mode (%s) clock to 120hz on %s\n",
				mode->name,
				!drm_atomic_crtc_effectively_active(old_crtc_state) ?
				"resume" : "lp exit");
		}
	} else if (old_crtc_state->adjusted_mode.clock != old_crtc_state->mode.clock) {
		/* clock hacked in last commit due to resume or lp exit, undo that */
		new_crtc_state->mode_changed = true;
		new_crtc_state->adjusted_mode.clock = new_crtc_state->mode.clock;
		ctx->gs_connector->needs_commit = false;
		dev_dbg(ctx->dev, "restore mode (%s) clock after resume or lp exit\n",
			new_crtc_state->mode.name);
	}

	return 0;
}

static void tk4a_set_nolp_mode(struct gs_panel *ctx, const struct gs_panel_mode *pmode)
{
	struct device *dev = ctx->dev;
	const struct gs_panel_mode *current_mode = ctx->current_mode;
	unsigned int vrefresh = current_mode ? drm_mode_vrefresh(&current_mode->mode) : 30;
	unsigned int te_usec = current_mode ? current_mode->gs_mode.te_usec : 1109;

	if (!gs_is_panel_active(ctx))
		return;

	GS_DCS_BUF_ADD_CMD(dev, MIPI_DCS_SET_DISPLAY_OFF);

	/* backlight control and dimming */
	tk4a_update_wrctrld(ctx);
	tk4a_change_frequency(ctx, pmode);

	DPU_ATRACE_BEGIN("tk4a_wait_one_vblank");
	gs_panel_wait_for_vsync_done(ctx, te_usec,
			GS_VREFRESH_TO_PERIOD_USEC(vrefresh));

	/* Additional sleep time to account for TE variability */
	usleep_range(1000, 1010);
	DPU_ATRACE_END("tk4a_wait_one_vblank");

	GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, MIPI_DCS_SET_DISPLAY_ON);

	dev_info(dev, "exit LP mode\n");
}

static void tk4a_10bit_set(struct gs_panel *ctx)
{
	struct device *dev = ctx->dev;

	GS_DCS_BUF_ADD_CMDLIST(dev, test_key_enable);
	GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x28, 0xF2);
	GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, 0xF2, 0xCC);  /* 10bit */
	GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, test_key_disable);
}

static int tk4a_enable(struct drm_panel *panel)
{
	struct gs_panel *ctx = container_of(panel, struct gs_panel, base);
	struct device *dev = ctx->dev;
	const struct gs_panel_mode *pmode = ctx->current_mode;

	if (!pmode) {
		dev_err(dev, "no current mode set\n");
		return -EINVAL;
	}

	dev_info(dev, "%s\n", __func__);

	gs_panel_reset_helper(ctx);

	/* sleep out */
	GS_DCS_WRITE_DELAY_CMD(dev, 120, MIPI_DCS_EXIT_SLEEP_MODE);

	tk4a_10bit_set(ctx);

	/* initial command */
	gs_panel_send_cmdset(ctx, &tk4a_init_cmdset);

	/* frequency */
	tk4a_change_frequency(ctx, pmode);

	/* DSC related configuration */
	mipi_dsi_compression_mode(to_mipi_dsi_device(dev), true);
	gs_dcs_write_dsc_config(dev, &pps_config);
	/* DSC Enable */
	GS_DCS_BUF_ADD_CMD(dev, 0x9D, 0x01);

	/* dimming and HBM */
	tk4a_update_wrctrld(ctx);

	if (pmode->gs_mode.is_lp_mode)
		gs_panel_set_lp_mode_helper(ctx, pmode);

	/* display on */
	GS_DCS_WRITE_CMD(dev, MIPI_DCS_SET_DISPLAY_ON);

	return 0;
}

static int tk4a_panel_probe(struct mipi_dsi_device *dsi)
{
	struct tk4a_panel *spanel;

	spanel = devm_kzalloc(&dsi->dev, sizeof(*spanel), GFP_KERNEL);
	if (!spanel)
		return -ENOMEM;

	spanel->is_pixel_off = false;

	return gs_dsi_panel_common_init(dsi, &spanel->base);
}

static const struct gs_display_underrun_param underrun_param = {
	.te_idle_us = 500,
	.te_var = 1,
};

static const u16 WIDTH_MM = 65, HEIGHT_MM = 146;
static const u16 HDISPLAY = 1080, VDISPLAY = 2424;
static const u16 HFP = 32, HSA = 12, HBP = 16;
static const u16 VFP = 12, VSA = 4, VBP = 15;

#define TK4A_DSC {\
	.enabled = true,\
	.dsc_count = 1,\
	.cfg = &pps_config,\
}

static const struct gs_panel_mode_array tk4a_modes = {
	.num_modes = 2,
	.modes = {
		{
			.mode = {
				.name = "1080x2424@60:60",
				DRM_MODE_TIMING(60, HDISPLAY, HFP, HSA, HBP, VDISPLAY, VFP, VSA, VBP),
				/* aligned to bootloader setting */
				.type = DRM_MODE_TYPE_PREFERRED,
				.width_mm = WIDTH_MM,
				.height_mm = HEIGHT_MM,
			},
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.te_usec = 8450,
				.bpc = 8,
				.dsc = TK4A_DSC,
				.underrun_param = &underrun_param,
			},
		},
		{
			.mode = {
				.name = "1080x2424@120:120",
				DRM_MODE_TIMING(120, HDISPLAY, HFP, HSA, HBP, VDISPLAY, VFP, VSA, VBP),
				.width_mm = WIDTH_MM,
				.height_mm = HEIGHT_MM,
			},
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.te_usec = 276,
				.bpc = 8,
				.dsc = TK4A_DSC,
				.underrun_param = &underrun_param,
			},
		},
	}, /* modes */
};

const struct brightness_capability tk4a_brightness_capability = {
	.normal = {
		.nits = {
			.min = 2,
			.max = 1200,
		},
		.level = {
			.min = 184,
			.max = 3427,
		},
		.percentage = {
			.min = 0,
			.max = 67,
		},
	},
	.hbm = {
		.nits = {
			.min = 1200,
			.max = 1800,
		},
		.level = {
			.min = 3428,
			.max = 4095,
		},
		.percentage = {
			.min = 67,
			.max = 100,
		},
	},
};

static const struct gs_panel_mode_array tk4a_lp_modes = {
	.num_modes = 2,
	.modes = {
		{
			.mode = {
				.name = "1080x2424@30:30",
				DRM_MODE_TIMING(30, HDISPLAY, HFP, HSA, HBP, VDISPLAY, VFP, VSA, VBP),
				.width_mm = WIDTH_MM,
				.height_mm = HEIGHT_MM,
			},
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.te_usec = 1109,
				.bpc = 8,
				.dsc = TK4A_DSC,
				.underrun_param = &underrun_param,
				.is_lp_mode = true,
			},
		},
	},
};

static const struct drm_panel_funcs tk4a_drm_funcs = {
	.disable = gs_panel_disable,
	.unprepare = gs_panel_unprepare,
	.prepare = gs_panel_prepare,
	.enable = tk4a_enable,
	.get_modes = gs_panel_get_modes,
	.debugfs_init = tk4a_debugfs_init,
};

static const struct gs_panel_funcs tk4a_gs_funcs = {
	.set_brightness = tk4a_set_brightness,
	.set_lp_mode = gs_panel_set_lp_mode_helper,
	.set_nolp_mode = tk4a_set_nolp_mode,
	.set_binned_lp = gs_panel_set_binned_lp_helper,
	.set_dimming = tk4a_set_dimming_on,
	.set_hbm_mode = tk4a_set_hbm_mode,
	.is_mode_seamless = tk4a_is_mode_seamless,
	.mode_set = tk4a_mode_set,
	.get_panel_rev = tk4a_get_panel_rev,
	.read_id = gs_panel_read_slsi_ddic_id,
	.atomic_check = tk4a_atomic_check,
};

const struct gs_panel_brightness_desc tk4a_brightness_desc = {
	.max_brightness = 4095,
	.min_brightness = 2,
	.max_luminance = 10000000,
	.max_avg_luminance = 1200000,
	.min_luminance = 5,
	.default_brightness = 1290,    /* 140 nits */
	.brt_capability = &tk4a_brightness_capability,
};

const struct gs_panel_reg_ctrl_desc tk4a_reg_ctrl_desc = {
	.reg_ctrl_enable = {
		{PANEL_REG_ID_VDDI, 0},
		{PANEL_REG_ID_VCI, 0},
		{PANEL_REG_ID_VDDD, 10},
	},
	.reg_ctrl_disable = {
		{PANEL_REG_ID_VDDD, 0},
		{PANEL_REG_ID_VCI, 0},
		{PANEL_REG_ID_VDDI, 0},
	},
};

const struct gs_panel_desc google_tk4a = {
	.data_lane_cnt = 4,
	/* supported HDR format bitmask : 1(DOLBY_VISION), 2(HDR10), 3(HLG) */
	.hdr_formats = BIT(2) | BIT(3),
	.brightness_desc = &tk4a_brightness_desc,
	.modes = &tk4a_modes,
	.off_cmdset = &tk4a_off_cmdset,
	.lp_modes = &tk4a_lp_modes,
	.lp_cmdset = &tk4a_lp_cmdset,
	.binned_lp = tk4a_binned_lp,
	.num_binned_lp = ARRAY_SIZE(tk4a_binned_lp),
	.reg_ctrl_desc = &tk4a_reg_ctrl_desc,
	.panel_func = &tk4a_drm_funcs,
	.gs_panel_func = &tk4a_gs_funcs,
	.reset_timing_ms = { 1, 1, 5 },
};

static const struct of_device_id gs_panel_of_match[] = {
	{ .compatible = "google,gs-tk4a", .data = &google_tk4a },
	{ }
};
MODULE_DEVICE_TABLE(of, gs_panel_of_match);

static struct mipi_dsi_driver gs_panel_driver = {
	.probe = tk4a_panel_probe,
	.remove = gs_dsi_panel_common_remove,
	.driver = {
		.name = "panel-gs-tk4a",
		.of_match_table = gs_panel_of_match,
	},
};
module_mipi_dsi_driver(gs_panel_driver);

MODULE_AUTHOR("Safayat Ullah <safayat@google.com>");
MODULE_DESCRIPTION("MIPI-DSI based Google tk4a panel driver");
MODULE_LICENSE("GPL");
