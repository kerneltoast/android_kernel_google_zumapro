/* SPDX-License-Identifier: MIT */

#include <drm/display/drm_dsc_helper.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <video/mipi_display.h>

#include "trace/dpu_trace.h"
#include "trace/panel_trace.h"

#include "gs_panel/drm_panel_funcs_defaults.h"
#include "gs_panel/gs_panel.h"
#include "gs_panel/gs_panel_funcs_defaults.h"

/* PPS Setting DSC 1.2a */
static const struct drm_dsc_config pps_config = {
	.line_buf_depth = 9,
	.bits_per_component = 8,
	.convert_rgb = true,
	.slice_width = 540,
	.slice_height = 101,
	.slice_count = 2,
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
	.initial_dec_delay = 526,
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
		{.range_min_qp = 5, .range_max_qp = 13, .range_bpg_offset = 52},
		{.range_min_qp = 7, .range_max_qp = 13, .range_bpg_offset = 52},
		{.range_min_qp = 13, .range_max_qp = 15, .range_bpg_offset = 52}
	},
	.rc_model_size = 8192,
	.flatness_min_qp = 3,
	.flatness_max_qp = 12,
	.initial_scale_value = 32,
	.scale_decrement_interval = 7,
	.scale_increment_interval = 2517,
	.nfl_bpg_offset = 246,
	.slice_bpg_offset = 258,
	.final_offset = 4336,
	.vbr_enable = false,
	.slice_chunk_size = 540,
	.dsc_version_minor = 2,
	.dsc_version_major = 1,
	.native_422 = false,
	.native_420 = false,
	.second_line_bpg_offset = 0,
	.nsl_bpg_offset = 0,
	.second_line_offset_adj = 0,
};

#define TK4C_WRCTRLD_DIMMING_BIT 0x08
#define TK4C_WRCTRLD_BCTRL_BIT 0x20

#define MIPI_DSI_FREQ_DEFAULT 756
#define MIPI_DSI_FREQ_ALTERNATIVE 776

static const u8 test_key_enable[] = { 0xF0, 0x5A, 0x5A };
static const u8 test_key_disable[] = { 0xF0, 0xA5, 0xA5 };
static const u8 test_key_fc_enable[] = { 0xFC, 0x5A, 0x5A };
static const u8 test_key_fc_disable[] = { 0xFC, 0xA5, 0xA5 };
static const u8 ltps_update[] = { 0xF7, 0x2F };
static const u8 pixel_off[] = { 0x22 };

static const struct gs_dsi_cmd tk4c_off_cmds[] = {
	GS_DSI_DELAY_CMD(MIPI_DCS_SET_DISPLAY_OFF),
	GS_DSI_DELAY_CMD(120, MIPI_DCS_ENTER_SLEEP_MODE),
};
static DEFINE_GS_CMDSET(tk4c_off);

static const struct gs_dsi_cmd tk4c_lp_cmds[] = {
	/* AOD Power Setting */
	GS_DSI_CMDLIST(test_key_enable),
	GS_DSI_CMD(0xB0, 0x00, 0x04, 0xF6),
	GS_DSI_CMD(0xF6, 0x25), /* Default */
	GS_DSI_CMDLIST(test_key_disable),

	/* AOD Mode On Setting */
	GS_DSI_CMD(MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x24),
};
static DEFINE_GS_CMDSET(tk4c_lp);

static const struct gs_dsi_cmd tk4c_lp_night_cmd[] = {
	GS_DSI_CMD(0x51, 0x00, 0xB8),
};

static const struct gs_dsi_cmd tk4c_lp_low_cmd[] = {
	GS_DSI_CMD(0x51, 0x01, 0x7E),
};
static const struct gs_dsi_cmd tk4c_lp_high_cmd[] = {
	GS_DSI_CMD(0x51, 0x03, 0x1A),
};

static const struct gs_binned_lp tk4c_binned_lp[] = {
	/* night threshold 4 nits */
	BINNED_LP_MODE_TIMING("night", 252, tk4c_lp_night_cmd, 12, 12 + 50),
	/* low threshold 40 nits */
	BINNED_LP_MODE_TIMING("low", 716, tk4c_lp_low_cmd, 12, 12 + 50),
	BINNED_LP_MODE_TIMING("high", 4095, tk4c_lp_high_cmd, 12, 12 + 50),
};

static const struct gs_dsi_cmd tk4c_init_cmds[] = {
	/* TE on */
	GS_DSI_CMD(MIPI_DCS_SET_TEAR_ON),

	/* TE width setting (MTP'ed) */
	/* TE2 width setting (MTP'ed) */

	/* CASET: 1080 */
	GS_DSI_CMD(MIPI_DCS_SET_COLUMN_ADDRESS, 0x00, 0x00, 0x04, 0x37),

	/* PASET: 2424 */
	GS_DSI_CMD(MIPI_DCS_SET_PAGE_ADDRESS, 0x00, 0x00, 0x09, 0x77),

	/* FFC Off (756Mpbs) Setting */
	GS_DSI_CMDLIST(test_key_enable),
	GS_DSI_CMDLIST(test_key_fc_enable),
	GS_DSI_CMD(0xB0, 0x00, 0x3A, 0xC5),
	GS_DSI_CMD(0xC5, 0x6C, 0x5C),
	GS_DSI_CMD(0xB0, 0x00, 0x36, 0xC5),
	GS_DSI_CMD(0xC5, 0x10),
	GS_DSI_CMDLIST(test_key_disable),
	GS_DSI_CMDLIST(test_key_fc_disable),

	/* VDDD LDO Setting, before PVT */
	GS_DSI_REV_CMDLIST(PANEL_REV_LT(PANEL_REV_PVT), test_key_fc_enable),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_PVT), 0xB0, 0x00, 0x58, 0xD7),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_PVT), 0xD7, 0x0A),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_PVT), 0xB0, 0x00, 0x5B, 0xD7),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_PVT), 0xD7, 0x0A),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_PVT), 0xFE, 0x80),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_PVT), 0xFE, 0x00),
	GS_DSI_REV_CMDLIST(PANEL_REV_LT(PANEL_REV_PVT), test_key_fc_disable),

	/* TSP HSYNC setting, MTP'ed from DVT */
	GS_DSI_REV_CMDLIST(PANEL_REV_LT(PANEL_REV_DVT1), test_key_enable),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_DVT1), 0xB0, 0x00, 0x42, 0xB9),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_DVT1), 0xB9, 0x19),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_DVT1), 0xB0, 0x00, 0x46, 0xB9),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_DVT1), 0xB9, 0xB0),

	/* FGZ common settings, MTP'ed from DVT */
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_DVT1), 0xB0, 0x00, 0x30, 0x68),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_DVT1), 0x68, 0x32, 0xFF, 0x04,
		0x08, 0x10, 0x15, 0x29, 0x67, 0xA5),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_DVT1), 0xB0, 0x00, 0x1C, 0x62),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_DVT1), 0x62, 0x1D, 0x5F),
	GS_DSI_REV_CMDLIST(PANEL_REV_LT(PANEL_REV_DVT1), test_key_disable),
};
static DEFINE_GS_CMDSET(tk4c_init);

/**
 * struct tk4c_panel - panel specific runtime info
 *
 * This struct maintains tk4c panel specific runtime info, any fixed details about panel
 * should most likely go into struct gs_panel_desc
 */
struct tk4c_panel {
	/** @base: base panel struct */
	struct gs_panel base;
	/**
	 * @is_pixel_off: pixel-off command is sent to panel. Only sending normal-on or resetting
	 *		  panel can recover to normal mode after entering pixel-off state.
	 */
	bool is_pixel_off;
};
#define to_spanel(ctx) container_of(ctx, struct tk4c_panel, base)

static void tk4c_change_frequency(struct gs_panel *ctx, const struct gs_panel_mode *pmode)
{
	struct device *dev = ctx->dev;

	u32 vrefresh = drm_mode_vrefresh(&pmode->mode);

	if (unlikely(!ctx))
		return;

	if (vrefresh != 60 && vrefresh != 120) {
		dev_warn(dev, "%s: invalid refresh rate %uhz\n", __func__, vrefresh);
		return;
	}

	GS_DCS_BUF_ADD_CMDLIST(dev, test_key_enable);
	GS_DCS_BUF_ADD_CMD(dev, 0x83, vrefresh == 60 ? 0x08 : 0x00);
	GS_DCS_BUF_ADD_CMDLIST(dev, ltps_update);
	GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, test_key_disable);

	dev_info(dev, "%s: change to %uHz\n", __func__, vrefresh);
	return;
}

static void tk4c_update_wrctrld(struct gs_panel *ctx)
{
	struct device *dev = ctx->dev;
	u8 val = TK4C_WRCTRLD_BCTRL_BIT;

	if (ctx->dimming_on)
		val |= TK4C_WRCTRLD_DIMMING_BIT;

	dev_dbg(dev, "%s(wrctrld:0x%x, hbm: %d, dimming: %d)\n", __func__, val,
		GS_IS_HBM_ON(ctx->hbm_mode), ctx->dimming_on);

	GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, MIPI_DCS_WRITE_CONTROL_DISPLAY, val);
}

static int tk4c_set_brightness(struct gs_panel *ctx, u16 br)
{
	u16 brightness;
	u32 max_brightness;
	struct tk4c_panel *spanel = to_spanel(ctx);
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
		dev_warn(dev, "%s: capped to dbv(%d)\n", __func__, max_brightness);
	}

	/* swap endianness because panel expects MSB first */
	brightness = swab16(br);

	return gs_dcs_set_brightness(ctx, brightness);
}

static void tk4c_set_hbm_mode(struct gs_panel *ctx, enum gs_hbm_mode mode)
{
	struct device *dev = ctx->dev;

	ctx->hbm_mode = mode;

	GS_DCS_BUF_ADD_CMDLIST(dev, test_key_enable);
	/* FGZ mode setting */
	GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x61, 0x68);
	if (GS_IS_HBM_ON(ctx->hbm_mode)) {
		if (GS_IS_HBM_ON_IRC_OFF(ctx->hbm_mode)) {
			/* FGZ Mode ON */
			if (ctx->panel_rev < PANEL_REV_DVT1) {
				GS_DCS_BUF_ADD_CMD(dev, 0x68, 0xB0, 0x2C, 0x6A, 0x80, 0x00, 0x00, 0xF5,
					0xC4);
			} else if (ctx->panel_rev == PANEL_REV_DVT1) {
				GS_DCS_BUF_ADD_CMD(dev, 0x68, 0xB0, 0x2C, 0x6A, 0x80, 0x00, 0x00, 0xE4,
					0xB6);
			} else { /* PVT/MP */
				GS_DCS_BUF_ADD_CMD(dev, 0x68, 0xB4, 0x2C, 0x6A, 0x80, 0x00, 0x00, 0x00,
					0xCD);
			}
		} else {
			/* FGZ Mode OFF */
			GS_DCS_BUF_ADD_CMD(dev, 0x68, 0xB0, 0x2C, 0x6A, 0x80, 0x00, 0x00, 0x00,
					   0x00);
		}
	} else {
		/* FGZ Mode OFF */
		GS_DCS_BUF_ADD_CMD(dev, 0x68, 0xB0, 0x2C, 0x6A, 0x80, 0x00, 0x00, 0x00, 0x00);
	}

	GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x01, 0xBD);
	GS_DCS_BUF_ADD_CMD(dev, 0xBD, ctx->hbm_mode ? 0x80 : 0x81);
	GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x2E, 0xBD);
	GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x00, ctx->hbm_mode ? 0x01 : 0x02);

	GS_DCS_BUF_ADD_CMDLIST(dev, ltps_update);
	GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, test_key_disable);

	dev_info(dev, "hbm_on=%d hbm_ircoff=%d.\n", GS_IS_HBM_ON(ctx->hbm_mode),
		 GS_IS_HBM_ON_IRC_OFF(ctx->hbm_mode));
}

static void tk4c_set_dimming(struct gs_panel *ctx, bool dimming_on)
{
	struct device *dev = ctx->dev;
	const struct gs_panel_mode *pmode = ctx->current_mode;

	ctx->dimming_on = dimming_on;

	if (pmode->gs_mode.is_lp_mode) {
		dev_warn(dev, "in lp mode, skip to update dimming usage\n");
		return;
	}

	tk4c_update_wrctrld(ctx);
}

static void tk4c_mode_set(struct gs_panel *ctx, const struct gs_panel_mode *pmode)
{
	tk4c_change_frequency(ctx, pmode);
}

static bool tk4c_is_mode_seamless(const struct gs_panel *ctx, const struct gs_panel_mode *pmode)
{
	/* seamless mode switch is possible if only changing refresh rate */
	return drm_mode_equal_no_clocks(&ctx->current_mode->mode, &pmode->mode);
}

static void tk4c_debugfs_init(struct drm_panel *panel, struct dentry *root)
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

	gs_panel_debugfs_create_cmdset(csroot, &tk4c_init_cmdset, "init");

	dput(csroot);
panel_out:
	dput(panel_root);
#endif
}

static void tk4c_get_panel_rev(struct gs_panel *ctx, u32 id)
{
	/* extract command 0xDB */
	u8 build_code = (id & 0xFF00) >> 8;
	u8 main = (build_code & 0xE0) >> 3;
	u8 sub = (build_code & 0x0C) >> 2;
	u8 rev = main | sub;

	gs_panel_get_panel_rev(ctx, rev);
}

static int tk4c_atomic_check(struct gs_panel *ctx, struct drm_atomic_state *state)
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

static void tk4c_set_nolp_mode(struct gs_panel *ctx, const struct gs_panel_mode *pmode)
{
	if (!gs_is_panel_active(ctx))
		return;

	/* AOD Mode Off Setting */
	tk4c_update_wrctrld(ctx);
	tk4c_change_frequency(ctx, pmode);

	dev_info(ctx->dev, "exit LP mode\n");
}

static int tk4c_enable(struct drm_panel *panel)
{
	struct gs_panel *ctx = container_of(panel, struct gs_panel, base);
	struct device *dev = ctx->dev;
	const struct gs_panel_mode *pmode = ctx->current_mode;

	if (!pmode) {
		dev_err(dev, "no current mode set\n");
		return -EINVAL;
	}

	dev_dbg(dev, "%s\n", __func__);

	/* toggle reset gpio */
	gs_panel_reset_helper(ctx);

	/* sleep out */
	GS_DCS_WRITE_DELAY_CMD(dev, 120, MIPI_DCS_EXIT_SLEEP_MODE);

	/* initial command */
	gs_panel_send_cmdset(ctx, &tk4c_init_cmdset);

	/* frequency */
	tk4c_change_frequency(ctx, pmode);

	/* DSC related configuration */
	mipi_dsi_compression_mode(to_mipi_dsi_device(dev), true);
	gs_dcs_write_dsc_config(dev, &pps_config);
	/* DSC Enable */
	GS_DCS_BUF_ADD_CMD(dev, 0x9D, 0x01);

	/* dimming and HBM */
	tk4c_update_wrctrld(ctx);

	/* display on */
	if (pmode->gs_mode.is_lp_mode)
		gs_panel_set_lp_mode_helper(ctx, pmode);

	GS_DCS_WRITE_CMD(dev, MIPI_DCS_SET_DISPLAY_ON);

	ctx->dsi_hs_clk_mbps = MIPI_DSI_FREQ_DEFAULT;

	return 0;
}

static int tk4c_panel_probe(struct mipi_dsi_device *dsi)
{
	struct tk4c_panel *spanel;

	spanel = devm_kzalloc(&dsi->dev, sizeof(*spanel), GFP_KERNEL);
	if (!spanel)
		return -ENOMEM;

	spanel->is_pixel_off = false;

	return gs_dsi_panel_common_init(dsi, &spanel->base);
}

static void tk4c_pre_update_ffc(struct gs_panel *ctx)
{
	struct device *dev = ctx->dev;

	dev_dbg(ctx->dev, "%s\n", __func__);

	DPU_ATRACE_BEGIN(__func__);

	/* FFC off */
	GS_DCS_BUF_ADD_CMDLIST(dev, test_key_enable);
	GS_DCS_BUF_ADD_CMDLIST(dev, test_key_fc_enable);
	GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x36, 0xC5);
	GS_DCS_BUF_ADD_CMD(dev, 0xC5, 0x10);
	GS_DCS_BUF_ADD_CMDLIST(dev, test_key_fc_disable);
	GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, test_key_disable);

	DPU_ATRACE_END(__func__);
}

static void tk4c_update_ffc(struct gs_panel *ctx, unsigned int hs_clk_mbps)
{
	struct device *dev = ctx->dev;

	dev_dbg(ctx->dev, "%s: hs_clk_mbps: current=%u, target=%u\n",
		__func__, ctx->dsi_hs_clk_mbps, hs_clk_mbps);

	DPU_ATRACE_BEGIN(__func__);

	GS_DCS_BUF_ADD_CMDLIST(dev, test_key_enable);
	GS_DCS_BUF_ADD_CMDLIST(dev, test_key_fc_enable);

	if (hs_clk_mbps != MIPI_DSI_FREQ_DEFAULT && hs_clk_mbps != MIPI_DSI_FREQ_ALTERNATIVE) {
		dev_warn(ctx->dev, "%s: invalid hs_clk_mbps=%u for FFC\n", __func__, hs_clk_mbps);
	} else if (ctx->dsi_hs_clk_mbps != hs_clk_mbps) {
		dev_info(ctx->dev, "%s: updating for hs_clk_mbps=%u\n", __func__, hs_clk_mbps);
		ctx->dsi_hs_clk_mbps = hs_clk_mbps;

		/* Update FFC */
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x3A, 0xC5);
		if (hs_clk_mbps == MIPI_DSI_FREQ_DEFAULT) {
			GS_DCS_BUF_ADD_CMD(dev, 0xC5, 0x6C, 0x5C);
		} else { /* MIPI_DSI_FREQ_ALTERNATIVE */
			GS_DCS_BUF_ADD_CMD(dev, 0xC5, 0x69, 0x91);
		}
	}
	GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x36, 0xC5);
	GS_DCS_BUF_ADD_CMD(dev, 0xC5, 0x11, 0x10, 0x50, 0x05);
	GS_DCS_BUF_ADD_CMDLIST(dev, test_key_fc_disable);
	GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, test_key_disable);

	DPU_ATRACE_END(__func__);
}

static void tk4c_set_ssc_en(struct gs_panel *ctx, bool enabled)
{
	struct device *dev = ctx->dev;
	const bool ssc_mode_update = ctx->ssc_en != enabled;

	if (!ssc_mode_update) {
		dev_dbg(ctx->dev, "ssc_mode skip update\n");
		return;
	}

	ctx->ssc_en = enabled;
	GS_DCS_BUF_ADD_CMDLIST(dev, test_key_enable);
	GS_DCS_BUF_ADD_CMDLIST(dev, test_key_fc_enable);
	GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x6E, 0xC5); /* global para */
	if (enabled)
		GS_DCS_BUF_ADD_CMD(dev, 0xC5, 0x07, 0x7F, 0x00, 0x00);
	else
		GS_DCS_BUF_ADD_CMD(dev, 0xC5, 0x04, 0x00);
	GS_DCS_BUF_ADD_CMDLIST(dev, test_key_fc_disable);
	GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, test_key_disable);
	dev_info(dev, "ssc_mode=%d\n", ctx->ssc_en);
}

static const struct gs_display_underrun_param underrun_param = {
	.te_idle_us = 350,
	.te_var = 1,
};

static const u16 WIDTH_MM = 65, HEIGHT_MM = 146;
static const u16 HDISPLAY = 1080, VDISPLAY = 2424;
static const u16 HFP = 32, HSA = 12, HBP = 16;
static const u16 VFP = 8, VSA = 4, VBP = 16;

#define TK4C_DSC {\
	.enabled = true,\
	.dsc_count = 1,\
	.cfg = &pps_config,\
}

static const struct gs_panel_mode_array tk4c_modes = {
	.num_modes = 2,
	.modes = {
		{
			.mode = {
				.name = "1080x2424@60:60",
				DRM_MODE_TIMING(60, HDISPLAY, HFP, HSA, HBP,
						VDISPLAY, VFP, VSA, VBP),
				/* aligned to bootloader setting */
				.type = DRM_MODE_TYPE_PREFERRED,
				.width_mm = WIDTH_MM,
				.height_mm = HEIGHT_MM,
			},
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.te_usec = 8360,
				.bpc = 8,
				.dsc = TK4C_DSC,
				.underrun_param = &underrun_param,
			},
		},
		{
			.mode = {
				.name = "1080x2424@120:120",
				DRM_MODE_TIMING(120, HDISPLAY, HFP, HSA, HBP,
						VDISPLAY, VFP, VSA, VBP),
				.width_mm = WIDTH_MM,
				.height_mm = HEIGHT_MM,
			},
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.te_usec = 276,
				.bpc = 8,
				.dsc = TK4C_DSC,
				.underrun_param = &underrun_param,
			},
		},
	},/* modes */
};

const struct brightness_capability tk4c_brightness_capability = {
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

static const struct gs_panel_mode_array tk4c_lp_modes = {
	.num_modes = 1,
	.modes = {
		{
			.mode = {
				.name = "1080x2424@30:30",
				DRM_MODE_TIMING(30, HDISPLAY, HFP, HSA, HBP,
						VDISPLAY, VFP, VSA, VBP),
				.width_mm = WIDTH_MM,
				.height_mm = HEIGHT_MM,
			},
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.te_usec = 1109,
				.bpc = 8,
				.dsc = TK4C_DSC,
				.underrun_param = &underrun_param,
				.is_lp_mode = true,
			},
		},
	}, /* modes */
};

static const struct drm_panel_funcs tk4c_drm_funcs = {
	.disable = gs_panel_disable,
	.unprepare = gs_panel_unprepare,
	.prepare = gs_panel_prepare,
	.enable = tk4c_enable,
	.get_modes = gs_panel_get_modes,
	.debugfs_init = tk4c_debugfs_init,
};

static const struct gs_panel_funcs tk4c_gs_funcs = {
	.set_brightness = tk4c_set_brightness,
	.set_lp_mode = gs_panel_set_lp_mode_helper,
	.set_nolp_mode = tk4c_set_nolp_mode,
	.set_binned_lp = gs_panel_set_binned_lp_helper,
	.set_dimming = tk4c_set_dimming,
	.set_hbm_mode = tk4c_set_hbm_mode,
	.is_mode_seamless = tk4c_is_mode_seamless,
	.mode_set = tk4c_mode_set,
	.get_panel_rev = tk4c_get_panel_rev,
	.read_id = gs_panel_read_slsi_ddic_id,
	.atomic_check = tk4c_atomic_check,
	.pre_update_ffc = tk4c_pre_update_ffc,
	.update_ffc = tk4c_update_ffc,
	.set_ssc_en = tk4c_set_ssc_en,
};

const struct gs_panel_brightness_desc tk4c_brightness_desc = {
	.max_brightness = 4095,
	.min_brightness = 2,
	.max_luminance = 10000000,
	.max_avg_luminance = 1200000,
	.min_luminance = 5,
	.default_brightness = 1290, /* 140 nits */
	.brt_capability = &tk4c_brightness_capability,
};

const struct gs_panel_reg_ctrl_desc tk4c_reg_ctrl_desc = {
	.reg_ctrl_enable = {
		{PANEL_REG_ID_VDDI, 0},
		{PANEL_REG_ID_VCI, 10},
	},
	.reg_ctrl_post_enable = {
		{PANEL_REG_ID_VDDD, 5},
	},
	.reg_ctrl_pre_disable = {
		{PANEL_REG_ID_VDDD, 0},
	},
	.reg_ctrl_disable = {
		{PANEL_REG_ID_VCI, 0},
		{PANEL_REG_ID_VDDI, 0},
	},
};

const struct gs_panel_desc google_tk4c = {
	.data_lane_cnt = 4,
	/* supported HDR format bitmask : 1(DOLBY_VISION), 2(HDR10), 3(HLG) */
	.hdr_formats = BIT(2) | BIT(3),
	.brightness_desc = &tk4c_brightness_desc,
	.modes = &tk4c_modes,
	.off_cmdset = &tk4c_off_cmdset,
	.lp_modes = &tk4c_lp_modes,
	.lp_cmdset = &tk4c_lp_cmdset,
	.binned_lp = tk4c_binned_lp,
	.num_binned_lp = ARRAY_SIZE(tk4c_binned_lp),
	.reg_ctrl_desc = &tk4c_reg_ctrl_desc,
	.panel_func = &tk4c_drm_funcs,
	.gs_panel_func = &tk4c_gs_funcs,
	.default_dsi_hs_clk_mbps = MIPI_DSI_FREQ_DEFAULT,
	.reset_timing_ms = { -1, 1, 1 },
};

static const struct of_device_id gs_panel_of_match[] = {
	{ .compatible = "google,gs-tk4c", .data = &google_tk4c },
	{ }
};
MODULE_DEVICE_TABLE(of, gs_panel_of_match);

static struct mipi_dsi_driver gs_panel_driver = {
	.probe = tk4c_panel_probe,
	.remove = gs_dsi_panel_common_remove,
	.driver = {
		.name = "panel-gs-tk4c",
		.of_match_table = gs_panel_of_match,
	},
};
module_mipi_dsi_driver(gs_panel_driver);

MODULE_AUTHOR("Taylor Nelms <tknelms@google.com>");
MODULE_DESCRIPTION("MIPI-DSI based Google tk4c panel driver");
MODULE_LICENSE("Dual MIT/GPL");
