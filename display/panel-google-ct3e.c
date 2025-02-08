// SPDX-License-Identifier: GPL-2.0-only
/*
 * MIPI-DSI based ct3e AMOLED LCD panel driver.
 *
 * Copyright (c) 2024 Google LLC
 */

#include <drm/display/drm_dsc_helper.h>
#include <linux/debugfs.h>
#include <linux/module.h>
#include <video/mipi_display.h>

#include "trace/dpu_trace.h"
#include "panel/panel-samsung-drv.h"

/* DSC1.2 */
static const struct drm_dsc_config pps_config = {
	.line_buf_depth = 9,
	.bits_per_component = 8,
	.convert_rgb = true,
	.slice_width = 540,
	.slice_height = 101,
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


#define CT3E_WRCTRLD_DIMMING_BIT    0x08
#define CT3E_WRCTRLD_BCTRL_BIT      0x20

static const u8 test_key_enable[] = { 0xF0, 0x5A, 0x5A };
static const u8 test_key_disable[] = { 0xF0, 0xA5, 0xA5 };
static const u8 pixel_off[] = { 0x22 };

static const struct exynos_dsi_cmd ct3e_off_cmds[] = {
	EXYNOS_DSI_CMD_SEQ_DELAY(MIPI_DCS_SET_DISPLAY_OFF),
	EXYNOS_DSI_CMD_SEQ_DELAY(120, MIPI_DCS_ENTER_SLEEP_MODE),
};
static DEFINE_EXYNOS_CMD_SET(ct3e_off);

static const struct exynos_dsi_cmd ct3e_lp_cmds[] = {
	EXYNOS_DSI_CMD_SEQ(MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x24),
};
static DEFINE_EXYNOS_CMD_SET(ct3e_lp);

static const struct exynos_dsi_cmd ct3e_lp_low_cmds[] = {
	EXYNOS_DSI_CMD_SEQ(MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x01, 0x7E),
};

static const struct exynos_dsi_cmd ct3e_lp_high_cmds[] = {
	EXYNOS_DSI_CMD_SEQ(MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x03, 0x1A),
};

static const struct exynos_binned_lp ct3e_binned_lp[] = {
	/* low threshold 40 nits */
	BINNED_LP_MODE_TIMING("low", 717, ct3e_lp_low_cmds,
			      12, 12 + 50),
	BINNED_LP_MODE_TIMING("high", 3427, ct3e_lp_high_cmds,
			      12, 12 + 50),
};

static const struct exynos_dsi_cmd ct3e_init_cmds[] = {
	/* TE on */
	EXYNOS_DSI_CMD_SEQ(MIPI_DCS_SET_TEAR_ON),

	/* TE2 setting */
	EXYNOS_DSI_CMD0(test_key_enable),
	EXYNOS_DSI_CMD_SEQ(0xB0, 0x00, 0x26, 0xB9),
	EXYNOS_DSI_CMD_SEQ(0xB9, 0x00, 0x00, 0x10, 0x00, 0x00,
                        0x3D, 0x00, 0x09, 0x90, 0x00, 0x09, 0x90),

	/* CASET: 1080 */
	EXYNOS_DSI_CMD_SEQ(MIPI_DCS_SET_COLUMN_ADDRESS, 0x00, 0x00, 0x04, 0x37),

	/* PASET: 2424 */
	EXYNOS_DSI_CMD_SEQ(MIPI_DCS_SET_PAGE_ADDRESS, 0x00, 0x00, 0x09, 0x77),

	/* FFC 865Mbps */
	EXYNOS_DSI_CMD_SEQ(0xFC, 0x5A, 0x5A),
	EXYNOS_DSI_CMD_SEQ(0xB0, 0x00, 0x3A, 0xC5),
	EXYNOS_DSI_CMD_SEQ(0xC5, 0x5E, 0xB5), /* 865Mbps FFC Setting */
	EXYNOS_DSI_CMD_SEQ(0xB0, 0x00, 0x36, 0xC5),
	EXYNOS_DSI_CMD_SEQ(0xC5, 0x11, 0x10, 0x50, 0x05),
	EXYNOS_DSI_CMD_SEQ(0xFC, 0xA5, 0xA5),

	/* TSP HSYNC setting */
	EXYNOS_DSI_CMD_SEQ(0xB0, 0x00, 0x42, 0xB9),
	EXYNOS_DSI_CMD_SEQ(0xB9, 0x19),
	EXYNOS_DSI_CMD_SEQ(0xB0, 0x00, 0x46, 0xB9),
	EXYNOS_DSI_CMD_SEQ(0xB9, 0xB0),
	EXYNOS_DSI_CMD0(test_key_disable),
};
static DEFINE_EXYNOS_CMD_SET(ct3e_init);

/**
 * struct ct3e_panel - panel specific runtime info
 *
 * This struct maintains ct3e panel specific runtime info, any fixed details about panel
 * should most likely go into struct exynos_panel_desc
 */
struct ct3e_panel {
	/** @base: base panel struct */
	struct exynos_panel base;
	/**
	 * @is_pixel_off: pixel-off command is sent to panel. Only sending normal-on or resetting
	 *		  panel can recover to normal mode after entering pixel-off state.
	 */
	bool is_pixel_off;
};
#define to_spanel(ctx) container_of(ctx, struct ct3e_panel, base)

static void ct3e_change_frequency(struct exynos_panel *ctx,
                        const struct exynos_panel_mode *pmode)
{
	u32 vrefresh = drm_mode_vrefresh(&pmode->mode);

	if (!ctx || (vrefresh != 60 && vrefresh != 120))
		return;

	EXYNOS_DCS_BUF_ADD_SET(ctx, test_key_enable);
	EXYNOS_DCS_BUF_ADD(ctx, 0x83, (vrefresh == 120) ? 0x00 : 0x08);
	EXYNOS_DCS_BUF_ADD(ctx, 0xF7, 0x2F);
	EXYNOS_DCS_BUF_ADD_SET_AND_FLUSH(ctx, test_key_disable);

	dev_info(ctx->dev, "%s: change to %uHz\n", __func__, vrefresh);
	return;
}

static void ct3e_update_wrctrld(struct exynos_panel *ctx)
{
	u8 val = CT3E_WRCTRLD_BCTRL_BIT;

	if (ctx->dimming_on)
		val |= CT3E_WRCTRLD_DIMMING_BIT;

	dev_dbg(ctx->dev,
		"%s(wrctrld:0x%x, hbm: %s, dimming: %s\n",
		__func__, val, IS_HBM_ON(ctx->hbm_mode) ? "on" : "off",
		ctx->dimming_on ? "on" : "off");

	EXYNOS_DCS_BUF_ADD_AND_FLUSH(ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY, val);
}

static int ct3e_set_brightness(struct exynos_panel *ctx, u16 br)
{
	u16 brightness;
	u32 max_brightness;
	struct ct3e_panel *spanel = to_spanel(ctx);

	if (ctx->current_mode && ctx->current_mode->exynos_mode.is_lp_mode) {
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

	if (!ctx->desc->brt_capability) {
		dev_err(ctx->dev, "no available brightness capability\n");
		return -EINVAL;
	}

	max_brightness = ctx->desc->brt_capability->hbm.level.max;

	if (br > max_brightness) {
		br = max_brightness;
		dev_warn(ctx->dev, "%s: capped to dbv(%d)\n", __func__,
			max_brightness);
	}

	brightness = __builtin_bswap16(br);

	return exynos_dcs_set_brightness(ctx, brightness);
}

static void ct3e_set_hbm_mode(struct exynos_panel *ctx,
				enum exynos_hbm_mode mode)
{
	ctx->hbm_mode = mode;

	EXYNOS_DCS_BUF_ADD_SET(ctx, test_key_enable);

	EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x00, 0x01, 0xBD);
	if (IS_HBM_ON_IRC_OFF(ctx->hbm_mode)) {
		EXYNOS_DCS_BUF_ADD(ctx, 0xBD, 0x80); /* HBM EM Cyc Set */
		EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x00, 0x2E, 0xBD); /* Global para */
		EXYNOS_DCS_BUF_ADD(ctx, 0xBD, 0x00, 0x01); /* HBM EM Cyc Set */
	} else {
		EXYNOS_DCS_BUF_ADD(ctx, 0xBD, 0x81); /* Normal EM Cyc Set */
		EXYNOS_DCS_BUF_ADD(ctx, 0xB0, 0x00, 0x2E, 0xBD); /* Global para */
		EXYNOS_DCS_BUF_ADD(ctx, 0xBD, 0x00, 0x02); /* Normal EM Cyc Set */
	}
	EXYNOS_DCS_BUF_ADD(ctx, 0xF7, 0x2F);

	EXYNOS_DCS_BUF_ADD_SET_AND_FLUSH(ctx, test_key_disable);

	dev_info(ctx->dev, "hbm_on=%d hbm_ircoff=%d.\n", IS_HBM_ON(ctx->hbm_mode),
		 IS_HBM_ON_IRC_OFF(ctx->hbm_mode));
}

static void ct3e_set_dimming_on(struct exynos_panel *exynos_panel,
                                bool dimming_on)
{
	const struct exynos_panel_mode *pmode = exynos_panel->current_mode;
	exynos_panel->dimming_on = dimming_on;

	if (pmode->exynos_mode.is_lp_mode) {
		dev_warn(exynos_panel->dev, "in lp mode, skip to update\n");
		return;
	}

	ct3e_update_wrctrld(exynos_panel);
}

static void ct3e_mode_set(struct exynos_panel *ctx,
                          const struct exynos_panel_mode *pmode)
{
	ct3e_change_frequency(ctx, pmode);
}

static bool ct3e_is_mode_seamless(const struct exynos_panel *ctx,
                                  const struct exynos_panel_mode *pmode)
{
	/* seamless mode switch is possible if only changing refresh rate */
	return drm_mode_equal_no_clocks(&ctx->current_mode->mode, &pmode->mode);
}

static void ct3e_debugfs_init(struct drm_panel *panel, struct dentry *root)
{
    if (IS_ENABLED(CONFIG_DEBUG_FS)) {
        struct exynos_panel *ctx = container_of(panel, struct exynos_panel, panel);
        struct dentry *panel_root, *csroot;

        if (!ctx)
            return;

        panel_root = debugfs_lookup("panel", root);
        if (!panel_root)
            return;

        csroot = debugfs_lookup("cmdsets", panel_root);
        if (!csroot)
            goto panel_out;

        exynos_panel_debugfs_create_cmdset(ctx, csroot, &ct3e_init_cmd_set, "init");

        dput(csroot);

panel_out:
	    dput(panel_root);
    }
}

static void ct3e_get_panel_rev(struct exynos_panel *ctx, u32 id)
{
	/* extract command 0xDB */
	u8 build_code = (id & 0xFF00) >> 8;
	u8 main = (build_code & 0xE0) >> 3;
	u8 sub = (build_code & 0x0C) >> 2;
	u8 rev = main | sub;

	exynos_panel_get_panel_rev(ctx, rev);
}

static void ct3e_set_lp_mode(struct exynos_panel *ctx,
					const struct exynos_panel_mode *pmode)
{
	exynos_panel_set_lp_mode(ctx, pmode);
}

static void ct3e_set_nolp_mode(struct exynos_panel *ctx,
				  const struct exynos_panel_mode *pmode)
{
	const struct exynos_panel_mode *current_mode = ctx->current_mode;
	unsigned int vrefresh = current_mode ? drm_mode_vrefresh(&current_mode->mode) : 30;
	unsigned int te_usec = current_mode ? current_mode->exynos_mode.te_usec : 1109;

	if (!is_panel_active(ctx))
		return;

	/* AOD Mode Off Setting */
	EXYNOS_DCS_BUF_ADD_SET(ctx, test_key_enable);
	EXYNOS_DCS_BUF_ADD(ctx, 0x53, 0x20);
	EXYNOS_DCS_BUF_ADD_SET_AND_FLUSH(ctx, test_key_disable);

	/* backlight control and dimming */
	ct3e_update_wrctrld(ctx);
	ct3e_change_frequency(ctx, pmode);

	DPU_ATRACE_BEGIN("ct3e_wait_one_vblank");
	exynos_panel_wait_for_vsync_done(ctx, te_usec,
			EXYNOS_VREFRESH_TO_PERIOD_USEC(vrefresh));

	/* Additional sleep time to account for TE variability */
	usleep_range(1000, 1010);
	DPU_ATRACE_END("ct3e_wait_one_vblank");

	dev_info(ctx->dev, "exit LP mode\n");
}

static int ct3e_enable(struct drm_panel *panel)
{
	struct exynos_panel *ctx = container_of(panel, struct exynos_panel, panel);
	const struct exynos_panel_mode *pmode = ctx->current_mode;
	struct drm_dsc_picture_parameter_set pps_payload;

	if (!pmode) {
		dev_err(ctx->dev, "no current mode set\n");
		return -EINVAL;
	}

	dev_info(ctx->dev, "%s\n", __func__);

	exynos_panel_reset(ctx);

	/* sleep out */
	EXYNOS_DCS_WRITE_SEQ_DELAY(ctx, 120, MIPI_DCS_EXIT_SLEEP_MODE);

	/* initial command */
	exynos_panel_send_cmd_set(ctx, &ct3e_init_cmd_set);

	/* frequency */
	ct3e_change_frequency(ctx, pmode);

	/* DSC related configuration */
	exynos_dcs_compression_mode(ctx, 0x1);
	drm_dsc_pps_payload_pack(&pps_payload, &pps_config);
	EXYNOS_PPS_WRITE_BUF(ctx, &pps_payload);
	/* DSC Enable */
	EXYNOS_DCS_BUF_ADD(ctx, 0xC2, 0x14);
	EXYNOS_DCS_BUF_ADD(ctx, 0x9D, 0x01);

	/* dimming and HBM */
	ct3e_update_wrctrld(ctx);

	if (pmode->exynos_mode.is_lp_mode)
		exynos_panel_set_lp_mode(ctx, pmode);

	/* display on */
	EXYNOS_DCS_WRITE_SEQ(ctx, MIPI_DCS_SET_DISPLAY_ON);

	return 0;
}

static int ct3e_panel_probe(struct mipi_dsi_device *dsi)
{
	struct ct3e_panel *spanel;

	spanel = devm_kzalloc(&dsi->dev, sizeof(*spanel), GFP_KERNEL);
	if (!spanel)
		return -ENOMEM;

	spanel->is_pixel_off = false;

	return exynos_panel_common_init(dsi, &spanel->base);
}

static const struct exynos_display_underrun_param underrun_param = {
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
	.slice_count = 2,\
	.slice_height = 101,\
	.cfg = &pps_config,\
}

static const struct exynos_panel_mode ct3e_modes[] = {
	{
		.mode = {
			.name = "1080x2424x60",
			.clock = 170520,
			.hdisplay = HDISPLAY,
			.hsync_start = HDISPLAY + HFP,
			.hsync_end = HDISPLAY + HFP + HSA,
			.htotal = HDISPLAY + HFP + HSA + HBP,
			.vdisplay = VDISPLAY,
			.vsync_start = VDISPLAY + VFP,
			.vsync_end = VDISPLAY + VFP + VSA,
			.vtotal = VDISPLAY + VFP + VSA + VBP,
			.flags = 0,
			.width_mm = WIDTH_MM,
			.height_mm = HEIGHT_MM,
		},
		.exynos_mode = {
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
			.name = "1080x2424x120",
			.clock = 341040,
			.hdisplay = HDISPLAY,
			.hsync_start = HDISPLAY + HFP,
			.hsync_end = HDISPLAY + HFP + HSA,
			.htotal = HDISPLAY + HFP + HSA + HBP,
			.vdisplay = VDISPLAY,
			.vsync_start = VDISPLAY + VFP,
			.vsync_end = VDISPLAY + VFP + VSA,
			.vtotal = VDISPLAY + VFP + VSA + VBP,
			.flags = 0,
			.width_mm = WIDTH_MM,
			.height_mm = HEIGHT_MM,
		},
		.exynos_mode = {
			.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
			.vblank_usec = 120,
			.te_usec = 276,
			.bpc = 8,
			.dsc = CT3E_DSC,
			.underrun_param = &underrun_param,
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

static const struct exynos_panel_mode ct3e_lp_mode = {
	.mode = {
		.name = "1080x2424x30",
		.clock = 85260,
		.hdisplay = HDISPLAY,
		.hsync_start = HDISPLAY + HFP,
		.hsync_end = HDISPLAY + HFP + HSA,
		.htotal = HDISPLAY + HFP + HSA + HBP,
		.vdisplay = VDISPLAY,
		.vsync_start = VDISPLAY + VFP,
		.vsync_end = VDISPLAY + VFP + VSA,
		.vtotal = VDISPLAY + VFP + VSA + VBP,
		.flags = 0,
		.width_mm = WIDTH_MM,
		.height_mm = HEIGHT_MM,
	},
	.exynos_mode = {
		.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
		.vblank_usec = 120,
		.te_usec = 1109,
		.bpc = 8,
		.dsc = CT3E_DSC,
		.underrun_param = &underrun_param,
		.is_lp_mode = true,
	}
};

static const struct drm_panel_funcs ct3e_drm_funcs = {
	.disable = exynos_panel_disable,
	.unprepare = exynos_panel_unprepare,
	.prepare = exynos_panel_prepare,
	.enable = ct3e_enable,
	.get_modes = exynos_panel_get_modes,
	.debugfs_init = ct3e_debugfs_init,
};

static const struct exynos_panel_funcs ct3e_exynos_funcs = {
	.set_brightness = ct3e_set_brightness,
	.set_lp_mode = ct3e_set_lp_mode,
	.set_nolp_mode = ct3e_set_nolp_mode,
	.set_binned_lp = exynos_panel_set_binned_lp,
	.set_dimming_on = ct3e_set_dimming_on,
	.set_hbm_mode = ct3e_set_hbm_mode,
	.is_mode_seamless = ct3e_is_mode_seamless,
	.mode_set = ct3e_mode_set,
	.get_panel_rev = ct3e_get_panel_rev,
	.read_id = exynos_panel_read_ddic_id,
};

const struct exynos_panel_desc google_ct3e = {
	.data_lane_cnt = 4,
	.max_brightness = 4095,
	.min_brightness = 2,
	.dft_brightness = 1290,    /* 140 nits */
	.brt_capability = &ct3e_brightness_capability,
	/* supported HDR format bitmask : 1(DOLBY_VISION), 2(HDR10), 3(HLG) */
	.hdr_formats = BIT(2) | BIT(3),
	.max_luminance = 10000000,
	.max_avg_luminance = 1200000,
	.min_luminance = 5,
	.modes = ct3e_modes,
	.num_modes = ARRAY_SIZE(ct3e_modes),
	.off_cmd_set = &ct3e_off_cmd_set,
	.lp_mode = &ct3e_lp_mode,
	.lp_cmd_set = &ct3e_lp_cmd_set,
	.binned_lp = ct3e_binned_lp,
	.num_binned_lp = ARRAY_SIZE(ct3e_binned_lp),
	.panel_func = &ct3e_drm_funcs,
	.exynos_panel_func = &ct3e_exynos_funcs,
	.reset_timing_ms = {-1, 1, 6},
	.reg_ctrl_enable = {
		{PANEL_REG_ID_VDDI, 0},
		{PANEL_REG_ID_VCI, 2},
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

static const struct of_device_id exynos_panel_of_match[] = {
	{ .compatible = "google,ct3e", .data = &google_ct3e },
	{ }
};
MODULE_DEVICE_TABLE(of, exynos_panel_of_match);

static struct mipi_dsi_driver exynos_panel_driver = {
	.probe = ct3e_panel_probe,
	.remove = exynos_panel_remove,
	.driver = {
		.name = "panel-google-ct3e",
		.of_match_table = exynos_panel_of_match,
	},
};
module_mipi_dsi_driver(exynos_panel_driver);

MODULE_AUTHOR("Cathy Hsu <cathsu@google.com>");
MODULE_DESCRIPTION("MIPI-DSI based Google ct3e panel driver");
MODULE_LICENSE("GPL");
