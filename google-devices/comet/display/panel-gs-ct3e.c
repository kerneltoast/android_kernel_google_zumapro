// SPDX-License-Identifier: MIT
/*
 * MIPI-DSI based ct3e panel driver.
 *
 * Copyright (c) 2024 Google LLC
 */

#include <drm/display/drm_dsc_helper.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/swab.h>
#include <video/mipi_display.h>

#include "trace/panel_trace.h"

#include "gs_panel/drm_panel_funcs_defaults.h"
#include "gs_panel/gs_panel.h"
#include "gs_panel/gs_panel_funcs_defaults.h"

/* DSC1.2 */
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


#define CT3E_WRCTRLD_DIMMING_BIT	0x08
#define CT3E_WRCTRLD_BCTRL_BIT	  0x20

static const u8 test_key_enable[] = { 0xF0, 0x5A, 0x5A };
static const u8 test_key_disable[] = { 0xF0, 0xA5, 0xA5 };
static const u8 pixel_off[] = { 0x22 };

static const struct gs_dsi_cmd ct3e_off_cmds[] = {
	GS_DSI_DELAY_CMD(MIPI_DCS_SET_DISPLAY_OFF),
	GS_DSI_DELAY_CMD(120, MIPI_DCS_ENTER_SLEEP_MODE),
};
static DEFINE_GS_CMDSET(ct3e_off);

static const struct gs_dsi_cmd ct3e_lp_cmds[] = {
	GS_DSI_CMD(MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x24),
};
static DEFINE_GS_CMDSET(ct3e_lp);

static const struct gs_dsi_cmd ct3e_lp_low_cmds[] = {
	GS_DSI_CMD(MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x01, 0x7E),
};

static const struct gs_dsi_cmd ct3e_lp_high_cmds[] = {
	GS_DSI_CMD(MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x03, 0x1A),
};

static const struct gs_binned_lp ct3e_binned_lp[] = {
	/* low threshold 40 nits */
	BINNED_LP_MODE_TIMING("low", 717, ct3e_lp_low_cmds,
				  12, 12 + 50),
	BINNED_LP_MODE_TIMING("high", 3427, ct3e_lp_high_cmds,
				  12, 12 + 50),
};

static const struct gs_dsi_cmd ct3e_init_cmds[] = {
	/* TE on */
	GS_DSI_CMD(MIPI_DCS_SET_TEAR_ON),

	/* TE2 setting */
	GS_DSI_CMDLIST(test_key_enable),
	GS_DSI_CMD(0xB0, 0x00, 0x26, 0xB9),
	GS_DSI_CMD(0xB9, 0x00, 0x00, 0x10, 0x00, 0x00,
			0x3D, 0x00, 0x09, 0x90, 0x00, 0x09, 0x90),

	/* CASET: 1080 */
	GS_DSI_CMD(MIPI_DCS_SET_COLUMN_ADDRESS, 0x00, 0x00, 0x04, 0x37),

	/* PASET: 2424 */
	GS_DSI_CMD(MIPI_DCS_SET_PAGE_ADDRESS, 0x00, 0x00, 0x09, 0x77),

	/* TSP HSYNC setting */
	GS_DSI_CMD(0xB0, 0x00, 0x42, 0xB9),
	GS_DSI_CMD(0xB9, 0x19),
	GS_DSI_CMD(0xB0, 0x00, 0x46, 0xB9),
	GS_DSI_CMD(0xB9, 0xB0),
	GS_DSI_CMDLIST(test_key_disable),
};
static DEFINE_GS_CMDSET(ct3e_init);

/**
 * struct ct3e_panel - panel specific runtime info
 *
 * This struct maintains ct3e panel specific runtime info, any fixed details about panel
 * should most likely go into struct gs_panel_desc
 */
struct ct3e_panel {
	/** @base: base panel struct */
	struct gs_panel base;
	/**
	 * @is_pixel_off: pixel-off command is sent to panel. Only sending normal-on or resetting
	 *		  panel can recover to normal mode after entering pixel-off state.
	 */
	bool is_pixel_off;
};
#define to_spanel(ctx) container_of(ctx, struct ct3e_panel, base)

static void ct3e_change_frequency(struct gs_panel *ctx,
				const struct gs_panel_mode *pmode)
{
	struct device *dev = ctx->dev;
	u32 vrefresh = drm_mode_vrefresh(&pmode->mode);

	if (!ctx || (vrefresh != 60 && vrefresh != 120))
		return;

	GS_DCS_BUF_ADD_CMDLIST(dev, test_key_enable);
	GS_DCS_BUF_ADD_CMD(dev, 0x83, (vrefresh == 120) ? 0x00 : 0x08);
	GS_DCS_BUF_ADD_CMD(dev, 0xF7, 0x2F);
	GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, test_key_disable);

	dev_info(dev, "%s: change to %uHz\n", __func__, vrefresh);
	return;
}

static void ct3e_update_wrctrld(struct gs_panel *ctx)
{
	struct device *dev = ctx->dev;
	u8 val = CT3E_WRCTRLD_BCTRL_BIT;

	if (ctx->dimming_on)
		val |= CT3E_WRCTRLD_DIMMING_BIT;

	dev_dbg(dev, "wrctrld: %#x, hbm: %d, dimming: %d\n", val,
		GS_IS_HBM_ON(ctx->hbm_mode), ctx->dimming_on);

	GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, MIPI_DCS_WRITE_CONTROL_DISPLAY, val);
}

static int ct3e_set_brightness(struct gs_panel *ctx, u16 br)
{
	u16 brightness;
	u32 max_brightness;
	struct ct3e_panel *spanel = to_spanel(ctx);
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

	brightness = swab16(br);

	return gs_dcs_set_brightness(ctx, brightness);
}

static void ct3e_set_hbm_mode(struct gs_panel *ctx,
				enum gs_hbm_mode mode)
{
	struct device *dev = ctx->dev;

	ctx->hbm_mode = mode;

	GS_DCS_BUF_ADD_CMDLIST(dev, test_key_enable);

	GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x01, 0xBD);
	if (GS_IS_HBM_ON(ctx->hbm_mode)) {
		GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x80); /* HBM EM Cyc Set */
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x2E, 0xBD); /* Global para */
		GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x00, 0x01); /* HBM EM Cyc Set */
	} else {
		GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x81); /* Normal EM Cyc Set */
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x2E, 0xBD); /* Global para */
		GS_DCS_BUF_ADD_CMD(dev, 0xBD, 0x00, 0x02); /* Normal EM Cyc Set */
	}
	GS_DCS_BUF_ADD_CMD(dev, 0xF7, 0x2F);

	GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, test_key_disable);

	dev_dbg(ctx->dev, "hbm_on=%d hbm_ircoff=%d.\n", GS_IS_HBM_ON(ctx->hbm_mode),
		 GS_IS_HBM_ON_IRC_OFF(ctx->hbm_mode));
}

static void ct3e_set_dimming(struct gs_panel *ctx, bool dimming_on)
{
	struct device *dev = ctx->dev;
	const struct gs_panel_mode *pmode = ctx->current_mode;

	ctx->dimming_on = dimming_on;

	if (pmode->gs_mode.is_lp_mode) {
		dev_warn(dev, "in lp mode; skip updating dimming_on\n");
		return;
	}

	ct3e_update_wrctrld(ctx);
}

static void ct3e_mode_set(struct gs_panel *ctx,
				const struct gs_panel_mode *pmode)
{
	ct3e_change_frequency(ctx, pmode);
}

static void ct3e_debugfs_init(struct drm_panel *panel, struct dentry *root)
{
	if (IS_ENABLED(CONFIG_DEBUG_FS)) {
		struct gs_panel *ctx = container_of(panel, struct gs_panel, base);
		struct dentry *panel_root, *csroot;

		if (!ctx)
			return;

		panel_root = debugfs_lookup("panel", root);
		if (!panel_root)
			return;

		csroot = debugfs_lookup("cmdsets", panel_root);
		if (!csroot)
			goto panel_out;

		gs_panel_debugfs_create_cmdset(csroot, &ct3e_init_cmdset, "init");

		dput(csroot);

panel_out:
		dput(panel_root);
	}
}

static void ct3e_get_panel_rev(struct gs_panel *ctx, u32 id)
{
	/* extract command 0xDB */
	u8 build_code = (id & 0xFF00) >> 8;
	u8 main = (build_code & 0xE0) >> 3;
	u8 sub = (build_code & 0x0C) >> 2;
	u8 rev = main | sub;

	gs_panel_get_panel_rev(ctx, rev);
}


static void ct3e_set_nolp_mode(struct gs_panel *ctx,
				  const struct gs_panel_mode *pmode)
{
	const struct gs_panel_mode *current_mode = ctx->current_mode;
	unsigned int vrefresh = current_mode ? drm_mode_vrefresh(&current_mode->mode) : 30;
	unsigned int te_usec = current_mode ? current_mode->gs_mode.te_usec : 1109;
	struct device *dev = ctx->dev;

	if (!gs_is_panel_active(ctx))
		return;

	/* AOD Mode Off Setting */
	GS_DCS_BUF_ADD_CMDLIST(dev, test_key_enable);
	GS_DCS_BUF_ADD_CMD(dev, 0x53, 0x20);
	GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, test_key_disable);

	/* backlight control and dimming */
	ct3e_update_wrctrld(ctx);
	ct3e_change_frequency(ctx, pmode);

	gs_panel_wait_for_vsync_done(ctx, te_usec,
			GS_VREFRESH_TO_PERIOD_USEC(vrefresh));

	/* Additional sleep time to account for TE variability */
	usleep_range(1000, 1010);

	dev_info(dev, "exit LP mode\n");
}

static int ct3e_enable(struct drm_panel *panel)
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

	/* initial command */
	gs_panel_send_cmdset(ctx, &ct3e_init_cmdset);

	/* frequency */
	ct3e_change_frequency(ctx, pmode);

	/* DSC related configuration */
	mipi_dsi_compression_mode(to_mipi_dsi_device(dev), true);
	gs_dcs_write_dsc_config(dev, &pps_config);
	/* DSC Enable */
	GS_DCS_BUF_ADD_CMD(dev, 0xC2, 0x14);
	GS_DCS_BUF_ADD_CMD(dev, 0x9D, 0x01);

	/* dimming and HBM */
	ct3e_update_wrctrld(ctx);

	if (pmode->gs_mode.is_lp_mode)
		gs_panel_set_lp_mode_helper(ctx, pmode);

	/* display on */
	GS_DCS_WRITE_CMD(dev, MIPI_DCS_SET_DISPLAY_ON);

	return 0;
}

static int ct3e_panel_probe(struct mipi_dsi_device *dsi)
{
	struct ct3e_panel *spanel;

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
static const u16 HFP = 44, HSA = 16, HBP = 20;
static const u16 VFP = 10, VSA = 6, VBP = 10;

#define CT3E_DSC {\
	.enabled = true,\
	.dsc_count = 1,\
	.cfg = &pps_config,\
}

static const struct gs_panel_mode_array ct3e_modes = {
	.num_modes = 2,
	.modes = {
		{
			.mode = {
				.name = "1080x2424@60:60",
				DRM_MODE_TIMING(60, HDISPLAY, HFP, HSA, HBP,
							VDISPLAY, VFP, VSA, VBP),
				.flags = 0,
				.width_mm = WIDTH_MM,
				.height_mm = HEIGHT_MM,
			},
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.te_usec = 8605,
				.bpc = 8,
				.dsc = CT3E_DSC,
				.underrun_param = &underrun_param,
			},
		},
		{
			.mode = {
				.name = "1080x2424@120:120",
				DRM_MODE_TIMING(120, HDISPLAY, HFP, HSA, HBP,
							VDISPLAY, VFP, VSA, VBP),
				.flags = 0,
				.width_mm = WIDTH_MM,
				.height_mm = HEIGHT_MM,
			},
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.te_usec = 276,
				.bpc = 8,
				.dsc = CT3E_DSC,
				.underrun_param = &underrun_param,
			},
		},
	},
};

const struct brightness_capability ct3e_brightness_capability = {
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

static const struct gs_panel_mode_array ct3e_lp_modes = {
	.num_modes = 1,
	.modes = {
		{
			.mode = {
				.name = "1080x2424@30:30",
				DRM_MODE_TIMING(30, HDISPLAY, HFP, HSA, HBP,
						VDISPLAY, VFP, VSA, VBP),
				.flags = 0,
				.width_mm = WIDTH_MM,
				.height_mm = HEIGHT_MM,
			},
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.te_usec = 1109,
				.bpc = 8,
				.dsc = CT3E_DSC,
				.underrun_param = &underrun_param,
				.is_lp_mode = true,
			},
		},
	},
};

static const struct drm_panel_funcs ct3e_drm_funcs = {
	.disable = gs_panel_disable,
	.unprepare = gs_panel_unprepare,
	.prepare = gs_panel_prepare,
	.enable = ct3e_enable,
	.get_modes = gs_panel_get_modes,
	.debugfs_init = ct3e_debugfs_init,
};

static const struct gs_panel_funcs ct3e_gs_funcs = {
	.set_brightness = ct3e_set_brightness,
	.set_lp_mode = gs_panel_set_lp_mode_helper,
	.set_nolp_mode = ct3e_set_nolp_mode,
	.set_binned_lp = gs_panel_set_binned_lp_helper,
	.set_dimming = ct3e_set_dimming,
	.set_hbm_mode = ct3e_set_hbm_mode,
	.is_mode_seamless = gs_panel_is_mode_seamless_helper,
	.mode_set = ct3e_mode_set,
	.get_panel_rev = ct3e_get_panel_rev,
	.read_id = gs_panel_read_slsi_ddic_id,
};

const struct gs_panel_brightness_desc ct3e_brightness_desc = {
	.max_brightness = 4095,
	.min_brightness = 2,
	.max_luminance = 10000000,
	.max_avg_luminance = 1200000,
	.min_luminance = 5,
	.default_brightness = 1290, /* 140 nits */
	.brt_capability = &ct3e_brightness_capability,
};

const struct gs_panel_reg_ctrl_desc ct3e_reg_ctrl_desc = {
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

const struct gs_panel_desc google_ct3e = {
	.data_lane_cnt = 4,
	/* supported HDR format bitmask : 1(DOLBY_VISION), 2(HDR10), 3(HLG) */
	.hdr_formats = BIT(2) | BIT(3),
	.brightness_desc = &ct3e_brightness_desc,
	.modes = &ct3e_modes,
	.off_cmdset = &ct3e_off_cmdset,
	.lp_modes = &ct3e_lp_modes,
	.lp_cmdset = &ct3e_lp_cmdset,
	.binned_lp = ct3e_binned_lp,
	.num_binned_lp = ARRAY_SIZE(ct3e_binned_lp),
	.reg_ctrl_desc = &ct3e_reg_ctrl_desc,
	.panel_func = &ct3e_drm_funcs,
	.gs_panel_func = &ct3e_gs_funcs,
	.reset_timing_ms = {1, 1, 1},
};

static const struct of_device_id gs_panel_of_match[] = {
	{ .compatible = "google,gs-ct3e", .data = &google_ct3e },
	{ }
};
MODULE_DEVICE_TABLE(of, gs_panel_of_match);

static struct mipi_dsi_driver gs_panel_driver = {
	.probe = ct3e_panel_probe,
	.remove = gs_dsi_panel_common_remove,
	.driver = {
		.name = "panel-gs-ct3e",
		.of_match_table = gs_panel_of_match,
	},
};
module_mipi_dsi_driver(gs_panel_driver);

MODULE_AUTHOR("Cathy Hsu <cathsu@google.com>");
MODULE_DESCRIPTION("MIPI-DSI based Google ct3e panel driver");
MODULE_LICENSE("Dual MIT/GPL");
