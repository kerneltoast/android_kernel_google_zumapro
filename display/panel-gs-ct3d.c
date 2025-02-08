/* SPDX-License-Identifier: MIT */
/*
 * MIPI-DSI based CT3D panel driver.
 *
 * Copyright 2024 Google LLC
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#include <drm/display/drm_dsc_helper.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/thermal.h>
#include <video/mipi_display.h>

#include "trace/panel_trace.h"

#include "gs_panel/drm_panel_funcs_defaults.h"
#include "gs_panel/gs_panel.h"
#include "gs_panel/gs_panel_funcs_defaults.h"

#define CT3D_DDIC_ID_LEN 8
#define CT3D_DIMMING_FRAME 32

#define WIDTH_MM 64
#define HEIGHT_MM 145

#define MIPI_DSI_FREQ_MBPS_DEFAULT 865
#define MIPI_DSI_FREQ_MBPS_ALTERNATIVE 756

#define PROJECT "CT3D"

/**
 * struct ct3d_panel - panel specific runtime info
 *
 * This struct maintains ct3d panel specific runtime info, any fixed details about panel
 * should most likely go into struct gs_panel_desc.
 */
struct ct3d_panel {
	/** @base: base panel struct */
	struct gs_panel base;
	/** @is_hbm2_enabled: indicates panel is running in HBM mode 2 */
	bool is_hbm2_enabled;
};

#define to_spanel(ctx) container_of(ctx, struct ct3d_panel, base)

static const struct gs_dsi_cmd ct3d_lp_cmds[] = {
	/* Disable the Black insertion in AoD */
	GS_DSI_CMD(0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00),
	GS_DSI_CMD(0xC0, 0x54),

	/* disable dimming */
	GS_DSI_CMD(0x53, 0x20),
	/* enter AOD */
	GS_DSI_CMD(MIPI_DCS_ENTER_IDLE_MODE),
	/* Settings AOD Hclk */
	GS_DSI_CMD(0xFF, 0xAA, 0x55, 0xA5, 0x81),
	GS_DSI_CMD(0x6F, 0x0E),
	GS_DSI_CMD(0xF5, 0x20),
	/* Lock TE2 30Hz */
	GS_DSI_CMD(0x5A, 0x04),
};
static DEFINE_GS_CMDSET(ct3d_lp);

static const struct gs_dsi_cmd ct3d_lp_off_cmds[] = {
	GS_DSI_CMD(0x6F, 0x04),
	GS_DSI_CMD(MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x00, 0x00),
};

static const struct gs_dsi_cmd ct3d_lp_night_cmds[] = {
	/* 2 nit */
	GS_DSI_CMD(0x6F, 0x04),
	GS_DSI_REV_CMD(PANEL_REV_GE(PANEL_REV_EVT1_1),
				MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x00, 0x03),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_EVT1_1),
				MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x03, 0x33),
};

static const struct gs_dsi_cmd ct3d_lp_low_cmds[] = {
	/* 10 nit */
	GS_DSI_CMD(0x6F, 0x04),
	GS_DSI_REV_CMD(PANEL_REV_GE(PANEL_REV_EVT1_1),
				MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x07, 0xB2),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_EVT1_1),
				MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x03, 0x33),
};

static const struct gs_dsi_cmd ct3d_lp_high_cmds[] = {
	/* 50 nit */
	GS_DSI_CMD(0x6F, 0x04),
	GS_DSI_CMD(MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x0F, 0xFE),
};

static const struct gs_binned_lp ct3d_binned_lp[] = {
	BINNED_LP_MODE("off", 0, ct3d_lp_off_cmds),
	/* night threshold 4 nits */
	BINNED_LP_MODE_TIMING("night", 105, ct3d_lp_night_cmds, 0, 32),
	/* low threshold 40 nits */
	BINNED_LP_MODE_TIMING("low", 871, ct3d_lp_low_cmds, 0, 32),
	/* rising = 0, falling = 32 */
	BINNED_LP_MODE_TIMING("high", 3628, ct3d_lp_high_cmds, 0, 32),
};

static const struct gs_dsi_cmd ct3d_off_cmds[] = {
	GS_DSI_DELAY_CMD(100, MIPI_DCS_SET_DISPLAY_OFF),
	GS_DSI_DELAY_CMD(120, MIPI_DCS_ENTER_SLEEP_MODE),
};
static DEFINE_GS_CMDSET(ct3d_off);

static const struct gs_dsi_cmd ct3d_init_cmds[] = {
	/* CMD2, Page0 */
	GS_DSI_CMD(0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00),
	GS_DSI_CMD(0x6F, 0x06),
	GS_DSI_CMD(0xB5, 0x7F, 0x00, 0x2C, 0x00),
	GS_DSI_CMD(0x6F, 0x11),
	GS_DSI_CMD(0xB5, 0x2C, 0x2C, 0x2C, 0x2C, 0x2C),

	GS_DSI_CMD(0x6F, 0x1B),
	GS_DSI_CMD(0xBA, 0x18),

	GS_DSI_CMD(0x6F, 0x2D),
	GS_DSI_CMD(0xB5, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A,
				0x2A, 0x2A, 0x2A, 0x25, 0x25, 0x1B, 0x1B, 0x13, 0x13, 0x0C, 0x0C,
				0x0C, 0x0C, 0x07),
	GS_DSI_CMD(0x6F, 0x44),
	GS_DSI_CMD(0xB5, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A,
				0x2A, 0x2A, 0x2A, 0x25, 0x25, 0x1B, 0x1B, 0x13, 0x13, 0x0C, 0x0C,
				0x0C, 0x0C, 0x07),

	/* CMD2, Page1 */
	GS_DSI_CMD(0xF0, 0x55, 0xAA, 0x52, 0x08, 0x01),
	GS_DSI_CMD(0x6F, 0x05),
	GS_DSI_CMD(0xC5, 0x15, 0x15, 0x15, 0xDD),

	/* FFC Off */
	GS_DSI_CMD(0xC3, 0x00),
	/* FFC setting (MIPI: 865Mbps) */
	GS_DSI_CMD(0xC3, 0xDD, 0x06, 0x20, 0x0E, 0xFF,
				0x00, 0x06, 0x20, 0x0E, 0xFF, 0x00,
				0x04, 0x79, 0x0E, 0x06, 0x12, 0x13,
				0x04, 0x79, 0x0E, 0x06, 0x12, 0x13,
				0x04, 0x79, 0x0E, 0x06, 0x12, 0x13,
				0x04, 0x79, 0x0E, 0x06, 0x12, 0x13,
				0x04, 0x79, 0x0E, 0x06, 0x12, 0x13),

	/* CMD2, Page3 */
	GS_DSI_CMD(0xF0, 0x55, 0xAA, 0x52, 0x08, 0x03),
	/* Extend AOD TE width to 1.9ms */
	GS_DSI_CMD(0x6F, 0x22),
	GS_DSI_CMD(0xB3, 0x70, 0x7F),
	/* Disable AOD power saving */
	GS_DSI_CMD(0xC7, 0x00),

	/* CMD2, Page4 */
	GS_DSI_CMD(0xF0, 0x55, 0xAA, 0x52, 0x08, 0x04),
	/* Extend DBI flash data update cycle time */
	GS_DSI_CMD(0xBB, 0xB3, 0x01, 0xBC),

	/* CMD2, Page7 */
	GS_DSI_CMD(0xF0, 0x55, 0xAA, 0x52, 0x08, 0x07),
	/* Round algorithm OFF */
	GS_DSI_CMD(0xC0, 0x00),

	/* CMD3, Page0 */
	GS_DSI_CMD(0xFF, 0xAA, 0x55, 0xA5, 0x80),
	GS_DSI_CMD(0x6F, 0x19),
	GS_DSI_CMD(0xF2, 0x00),
	GS_DSI_CMD(0x6F, 0x1A),
	GS_DSI_CMD(0xF4, 0x55),
	GS_DSI_CMD(0x6F, 0x2D),
	GS_DSI_CMD(0xFC, 0x44),
	GS_DSI_CMD(0x6F, 0x11),
	GS_DSI_CMD(0xF8, 0x01, 0x7B),
	GS_DSI_CMD(0x6F, 0x2D),
	GS_DSI_CMD(0xF8, 0x01, 0x1D),

	/* CMD3, Page1 */
	GS_DSI_CMD(0xFF, 0xAA, 0x55, 0xA5, 0x81),
	GS_DSI_CMD(0x6F, 0x05),
	GS_DSI_CMD(0xFE, 0x3C),
	GS_DSI_CMD(0x6F, 0x02),
	GS_DSI_CMD(0xF9, 0x04),
	GS_DSI_CMD(0x6F, 0x1E),
	GS_DSI_CMD(0xFB, 0x0F),
	GS_DSI_CMD(0x6F, 0x0D),
	GS_DSI_CMD(0xFB, 0x84),
	GS_DSI_CMD(0x6F, 0x0F),
	GS_DSI_CMD(0xF5, 0x20),
	/* CMD3, Page2 */
	GS_DSI_CMD(0xFF, 0xAA, 0x55, 0xA5, 0x82),
	GS_DSI_CMD(0x6F, 0x09),
	GS_DSI_CMD(0xF2, 0x55),
	/* CMD3, Page3 */
	GS_DSI_CMD(0xFF, 0xAA, 0x55, 0xA5, 0x83),
	GS_DSI_CMD(0x6F, 0x12),
	GS_DSI_CMD(0xFE, 0x41),

	/* CMD, Disable */
	GS_DSI_CMD(0xFF, 0xAA, 0x55, 0xA5, 0x00),

	GS_DSI_CMD(MIPI_DCS_SET_TEAR_SCANLINE, 0x00, 0x00),
	GS_DSI_CMD(MIPI_DCS_SET_TEAR_ON, 0x00, 0x2D),
	/* BC Dimming OFF */
	GS_DSI_CMD(MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x20),
	GS_DSI_CMD(MIPI_DCS_SET_COLUMN_ADDRESS, 0x00, 0x00, 0x04, 0x37),
	GS_DSI_CMD(MIPI_DCS_SET_PAGE_ADDRESS, 0x00, 0x00, 0x09, 0x77),
	/* Normal GMA */
	GS_DSI_CMD(MIPI_DCS_SET_GAMMA_CURVE, 0x00),

	/* CMD1, DPC Temperature 25 */
	GS_DSI_CMD(0x81, 0x01, 0x19),
	GS_DSI_CMD(0x03, 0x01),
	GS_DSI_CMD(0x90, 0x03, 0x03),
	/* 2DSC & slice high 24 DSC v1.2a */
	GS_DSI_CMD(0x91, 0x89, 0xA8, 0x00, 0x18, 0xD2, 0x00, 0x02, 0x25, 0x02,
				0x35, 0x00, 0x07, 0x04, 0x86, 0x04, 0x3D, 0x10, 0xF0),
	GS_DSI_CMD(0x2F, 0x02),
	GS_DSI_DELAY_CMD(60, MIPI_DCS_EXIT_SLEEP_MODE)
};
static DEFINE_GS_CMDSET(ct3d_init);

static void ct3d_update_te2(struct gs_panel *ctx)
{
	struct gs_panel_te2_timing timing;
	struct device *dev = ctx->dev;
	u8 width = 0x2D; /* default width */
	u32 rising = 0, falling;
	int ret;

	ret = gs_panel_get_current_mode_te2(ctx, &timing);
	if (!ret) {
		falling = timing.falling_edge;
		if (falling >= timing.rising_edge) {
			rising = timing.rising_edge;
			width = falling - rising;
		} else {
			dev_warn(dev, "invalid timing, use default setting\n");
		}
	} else if (ret == -EAGAIN) {
		dev_dbg(dev, "Panel is not ready, use default setting\n");
	} else {
		return;
	}

	dev_dbg(dev, "TE2 updated: rising= 0x%x, width= 0x%x", rising, width);

	GS_DCS_BUF_ADD_CMD(dev, MIPI_DCS_SET_TEAR_SCANLINE, 0x00, rising);
	GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, MIPI_DCS_SET_TEAR_ON, 0x00, width);
}

static void ct3d_update_irc(struct gs_panel *ctx,
				const enum gs_hbm_mode hbm_mode,
				const int vrefresh)
{
	struct device *dev = ctx->dev;
	struct ct3d_panel *spanel = to_spanel(ctx);
	const u16 level = gs_panel_get_brightness(ctx);

	if (GS_IS_HBM_ON_IRC_OFF(hbm_mode)) {
		/* sync from bigSurf panel_rev >= PANEL_REV_EVT */
		if (level == ctx->desc->brightness_desc->brt_capability->hbm.level.max) {
			/* set brightness to hbm2 */
			GS_DCS_BUF_ADD_CMD(dev, MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x0F, 0xFF);
			spanel->is_hbm2_enabled = true;
			/* set ACD Level 3 */
			GS_DCS_BUF_ADD_CMD(dev, 0x55, 0x04);
			GS_DCS_BUF_ADD_CMD(dev, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00);
			GS_DCS_BUF_ADD_CMD(dev, 0x6F, 0x0C);
			GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x0E, 0x2C, 0x32);
		} else {
			if (spanel->is_hbm2_enabled) {
				/* set ACD off */
				GS_DCS_BUF_ADD_CMD(dev, 0x55, 0x00);
			}
			spanel->is_hbm2_enabled = false;
		}

		dev_info(ctx->dev, "%s: is HBM2 enabled: %d\n",
					__func__, spanel->is_hbm2_enabled);

		GS_DCS_BUF_ADD_CMD(dev, 0x5F, 0x01);
		if (vrefresh == 120) {
			GS_DCS_BUF_ADD_CMD(dev, 0x2F, 0x00);
			GS_DCS_BUF_ADD_CMD(dev, MIPI_DCS_SET_GAMMA_CURVE, 0x02);
			if (ctx->panel_rev < PANEL_REV_PVT) {
				GS_DCS_BUF_ADD_CMD(dev, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00);
				GS_DCS_BUF_ADD_CMD(dev, 0x6F, 0x03);
				GS_DCS_BUF_ADD_CMD(dev, 0xC0, 0x40);
			}
		} else {
			GS_DCS_BUF_ADD_CMD(dev, 0x2F, 0x02);
			GS_DCS_BUF_ADD_CMD(dev, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00);
			GS_DCS_BUF_ADD_CMD(dev, 0x6F, 0x03);
			GS_DCS_BUF_ADD_CMD(dev, 0xC0, 0x40);
		}
	} else {
		const u8 val1 = level >> 8;
		const u8 val2 = level & 0xff;

		GS_DCS_BUF_ADD_CMD(dev, 0x5F, 0x00);
		if (vrefresh == 120) {
			GS_DCS_BUF_ADD_CMD(dev, 0x2F, 0x00);
			GS_DCS_BUF_ADD_CMD(dev, MIPI_DCS_SET_GAMMA_CURVE, 0x00);
			if (ctx->panel_rev < PANEL_REV_PVT) {
				GS_DCS_BUF_ADD_CMD(dev, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00);
				GS_DCS_BUF_ADD_CMD(dev, 0x6F, 0x03);
				GS_DCS_BUF_ADD_CMD(dev, 0xC0, 0x10);
			}
		} else {
			GS_DCS_BUF_ADD_CMD(dev, 0x2F, 0x02);
			GS_DCS_BUF_ADD_CMD(dev, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00);
			GS_DCS_BUF_ADD_CMD(dev, 0x6F, 0x03);
			GS_DCS_BUF_ADD_CMD(dev, 0xC0, 0x10);
		}
		/* sync from bigSurf panel_rev >= PANEL_REV_EVT */
		GS_DCS_BUF_ADD_CMD(dev, MIPI_DCS_SET_DISPLAY_BRIGHTNESS, val1, val2);
	}
	/* Empty command is for flush */
	GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, 0x00);
}

static void ct3d_change_frequency(struct gs_panel *ctx,
				    const struct gs_panel_mode *pmode)
{
	struct device *dev = ctx->dev;
	int vrefresh = drm_mode_vrefresh(&pmode->mode);

	if (vrefresh != 60 && vrefresh != 120)
		return;

	if (!GS_IS_HBM_ON(ctx->hbm_mode)) {
		if (vrefresh == 120) {
			GS_DCS_BUF_ADD_CMD(dev, 0x2F, 0x00);
			GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, MIPI_DCS_SET_GAMMA_CURVE, 0x00);
		} else {
			GS_DCS_BUF_ADD_CMD(dev, 0x2F, 0x02);
			GS_DCS_BUF_ADD_CMD(dev, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00);
			GS_DCS_BUF_ADD_CMD(dev, 0x6F, 0x03);
			GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, 0xC0, 0x10);
		}
	} else {
		ct3d_update_irc(ctx, ctx->hbm_mode, vrefresh);
	}

	dev_dbg(dev, "%s: change to %uhz\n", __func__, vrefresh);
}

static void ct3d_set_dimming(struct gs_panel *ctx,
				 bool dimming_on)
{
	const struct gs_panel_mode *pmode = ctx->current_mode;
	struct device *dev = ctx->dev;

	ctx->dimming_on = dimming_on;

	if (pmode->gs_mode.is_lp_mode) {
		dev_warn(dev, "in lp mode, skip dimming update\n");
		return;
	}

	GS_DCS_WRITE_CMD(dev, MIPI_DCS_WRITE_CONTROL_DISPLAY,
				ctx->dimming_on ? 0x28 : 0x20);
	dev_dbg(dev, "%s dimming_on=%d\n", __func__, dimming_on);
}

static void ct3d_set_nolp_mode(struct gs_panel *ctx,
				  const struct gs_panel_mode *pmode)
{
	struct device *dev = ctx->dev;

	int vrefresh = drm_mode_vrefresh(&pmode->mode);

	if (!gs_is_panel_active(ctx))
		return;

	/* exit AOD */
	GS_DCS_BUF_ADD_CMD(dev, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00);
	GS_DCS_BUF_ADD_CMD(dev, 0xC0, 0x54);
	GS_DCS_BUF_ADD_CMD(dev, MIPI_DCS_EXIT_IDLE_MODE);
	GS_DCS_BUF_ADD_CMD(dev, 0xFF, 0xAA, 0x55, 0xA5, 0x81);
	GS_DCS_BUF_ADD_CMD(dev, 0x6F, 0x0E);
	GS_DCS_BUF_ADD_CMD(dev, 0xF5, 0x2B);
	GS_DCS_BUF_ADD_CMD(dev, 0x5A, 0x04);
	GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, MIPI_DCS_WRITE_CONTROL_DISPLAY,
				  ctx->dimming_on ? 0x28 : 0x20);

	ct3d_change_frequency(ctx, pmode);
	ctx->timestamps.idle_exit_dimming_delay_ts = ktime_add_us(
		ktime_get(), 100 + GS_VREFRESH_TO_PERIOD_USEC(vrefresh) * 2);

	dev_info(dev, "exit LP mode\n");
}

static void ct3d_dimming_frame_setting(struct gs_panel *ctx, u8 dimming_frame)
{
	struct device *dev = ctx->dev;

	/* Fixed time 1 frame */
	if (!dimming_frame)
		dimming_frame = 0x01;

	GS_DCS_BUF_ADD_CMD(dev, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00);
	GS_DCS_BUF_ADD_CMD(dev, 0xB2, 0x19);
	GS_DCS_BUF_ADD_CMD(dev, 0x6F, 0x05);
	GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, 0xB2, dimming_frame, dimming_frame);
}

static int ct3d_enable(struct drm_panel *panel)
{
	struct gs_panel *ctx = container_of(panel, struct gs_panel, base);
	struct device *dev = ctx->dev;
	const struct gs_panel_mode *pmode = ctx->current_mode;

	if (!pmode) {
		dev_err(dev, "no current mode set\n");
		return -EINVAL;
	}

	dev_dbg(dev, "%s\n", __func__);

	gs_panel_reset_helper(ctx);
	gs_panel_send_cmdset(ctx, &ct3d_init_cmdset);
	ct3d_change_frequency(ctx, pmode);
	ct3d_dimming_frame_setting(ctx, CT3D_DIMMING_FRAME);

	if (pmode->gs_mode.is_lp_mode)
		gs_panel_set_lp_mode_helper(ctx, pmode);

	GS_DCS_WRITE_CMD(dev, MIPI_DCS_SET_DISPLAY_ON);

	ctx->dsi_hs_clk_mbps = MIPI_DSI_FREQ_MBPS_DEFAULT;

	return 0;
}

static int ct3d_disable(struct drm_panel *panel)
{
	struct gs_panel *ctx = container_of(panel, struct gs_panel, base);
	struct ct3d_panel *spanel = to_spanel(ctx);
	int ret;

	spanel->is_hbm2_enabled = false;

	ret = gs_panel_disable(panel);
	if (ret)
		return ret;

	return 0;
}

static int ct3d_atomic_check(struct gs_panel *ctx, struct drm_atomic_state *state)
{
	struct drm_connector *conn = &ctx->gs_connector->base;
	struct drm_connector_state *new_conn_state =
					drm_atomic_get_new_connector_state(state, conn);
	struct drm_crtc_state *old_crtc_state, *new_crtc_state;
	struct device *dev = ctx->dev;

	if (!ctx->current_mode || drm_mode_vrefresh(&ctx->current_mode->mode) == 120 ||
	    !new_conn_state || !new_conn_state->crtc)
		return 0;

	new_crtc_state = drm_atomic_get_new_crtc_state(state, new_conn_state->crtc);
	old_crtc_state = drm_atomic_get_old_crtc_state(state, new_conn_state->crtc);
	if (!old_crtc_state || !new_crtc_state || !new_crtc_state->active)
		return 0;

	if (!drm_atomic_crtc_effectively_active(old_crtc_state) ||
	    (ctx->current_mode->gs_mode.is_lp_mode &&
		drm_mode_vrefresh(&new_crtc_state->mode) == 60)) {
		struct drm_display_mode *mode = &new_crtc_state->adjusted_mode;

		mode->clock = mode->htotal * mode->vtotal * 120 / 1000;
		if (mode->clock != new_crtc_state->mode.clock) {
			new_crtc_state->mode_changed = true;
			ctx->gs_connector->needs_commit = true;
			dev_dbg(dev, "raise mode (%s) clock to 120hz on %s\n",
				mode->name,
				!drm_atomic_crtc_effectively_active(old_crtc_state) ?
				"resume" : "lp exit");
		}
	} else if (old_crtc_state->adjusted_mode.clock != old_crtc_state->mode.clock) {
		/* clock hacked in last commit due to resume or lp exit, undo that */
		new_crtc_state->mode_changed = true;
		new_crtc_state->adjusted_mode.clock = new_crtc_state->mode.clock;
		ctx->gs_connector->needs_commit = false;
		dev_dbg(dev, "restore mode (%s) clock after resume or lp exit\n",
			new_crtc_state->mode.name);
	}

	return 0;
}

static void ct3d_pre_update_ffc(struct gs_panel *ctx)
{
	struct device *dev = ctx->dev;

	dev_dbg(ctx->dev, "%s\n", __func__);

	/* FFC off */
	GS_DCS_BUF_ADD_CMD(dev, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x01);
	GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, 0xC3, 0x00);
}

static void ct3d_update_ffc(struct gs_panel *ctx, unsigned int hs_clk_mbps)
{
	struct device *dev = ctx->dev;

	dev_dbg(ctx->dev, "%s: hs_clk_mbps: current=%d, target=%d\n",
		__func__, ctx->dsi_hs_clk_mbps, hs_clk_mbps);

	if (hs_clk_mbps != MIPI_DSI_FREQ_MBPS_DEFAULT &&
	    hs_clk_mbps != MIPI_DSI_FREQ_MBPS_ALTERNATIVE) {
		dev_warn(ctx->dev, "invalid hs_clk_mbps=%d for FFC\n", hs_clk_mbps);
	} else if (ctx->dsi_hs_clk_mbps != hs_clk_mbps) {
		dev_info(ctx->dev, "%s: updating for hs_clk_mbps=%d\n", __func__, hs_clk_mbps);
		ctx->dsi_hs_clk_mbps = hs_clk_mbps;

		/* Update FFC */
		GS_DCS_BUF_ADD_CMD(dev, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x01);
		if (hs_clk_mbps == MIPI_DSI_FREQ_MBPS_DEFAULT)
			GS_DCS_BUF_ADD_CMD(dev, 0xC3, 0xDD, 0x06, 0x20, 0x0E, 0xFF,
						0x00, 0x06, 0x20, 0x0E, 0xFF, 0x00,
						0x04, 0x79, 0x0E, 0x06, 0x12, 0x13,
						0x04, 0x79, 0x0E, 0x06, 0x12, 0x13,
						0x04, 0x79, 0x0E, 0x06, 0x12, 0x13,
						0x04, 0x79, 0x0E, 0x06, 0x12, 0x13,
						0x04, 0x79, 0x0E, 0x06, 0x12, 0x13);
		else /* MIPI_DSI_FREQ_MBPS_ALTERNATIVE */
			GS_DCS_BUF_ADD_CMD(dev, 0xC3, 0xDD, 0x06, 0x20, 0x0C, 0xFF,
						0x00, 0x06, 0x20, 0x0C, 0xFF, 0x00,
						0x04, 0x63, 0x0C, 0x05, 0xD9, 0x10,
						0x04, 0x63, 0x0C, 0x05, 0xD9, 0x10,
						0x04, 0x63, 0x0C, 0x05, 0xD9, 0x10,
						0x04, 0x63, 0x0C, 0x05, 0xD9, 0x10,
						0x04, 0x63, 0x0C, 0x05, 0xD9, 0x10);
	}

	/* FFC on */
	GS_DCS_BUF_ADD_CMD(dev, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x01);
	GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, 0xC3, 0xDD);
}

static int ct3d_set_brightness(struct gs_panel *ctx, u16 br)
{
	struct device *dev = ctx->dev;
	struct ct3d_panel *spanel = to_spanel(ctx);

	if (ctx->current_mode->gs_mode.is_lp_mode) {
		if (gs_panel_has_func(ctx, set_binned_lp))
			ctx->desc->gs_panel_func->set_binned_lp(ctx, br);
		return 0;
	}

	if (GS_IS_HBM_ON_IRC_OFF(ctx->hbm_mode)
			&& br == ctx->desc->brightness_desc->brt_capability->hbm.level.max) {

		/* set brightness to hbm2 */
		GS_DCS_BUF_ADD_CMD(dev, MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x0F, 0xFF);
		spanel->is_hbm2_enabled = true;

		/* set ACD Level 3 */
		GS_DCS_BUF_ADD_CMD(dev, 0x55, 0x04);
		GS_DCS_BUF_ADD_CMD(dev, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00);
		GS_DCS_BUF_ADD_CMD(dev, 0x6F, 0x0C);
		GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, 0xB0, 0x0E, 0x2C, 0x32);
		dev_info(ctx->dev, "%s: is HBM2 enabled : %d\n",
				__func__, spanel->is_hbm2_enabled);
	} else {

		if (spanel->is_hbm2_enabled) {
			/* set ACD off */
			GS_DCS_BUF_ADD_CMD(dev, 0x55, 0x00);
			dev_info(ctx->dev, "%s: is HBM2 enabled: off\n", __func__);
		}
		spanel->is_hbm2_enabled = false;
		GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, MIPI_DCS_SET_DISPLAY_BRIGHTNESS,
						br >> 8, br & 0xff);
	}
	return 0;
}

static void ct3d_set_hbm_mode(struct gs_panel *ctx,
				 enum gs_hbm_mode hbm_mode)
{
	const struct gs_panel_mode *pmode = ctx->current_mode;
	struct device *dev = ctx->dev;
	int vrefresh = drm_mode_vrefresh(&pmode->mode);

	if (ctx->hbm_mode == hbm_mode)
		return;

	if (hbm_mode == GS_HBM_OFF) {
		GS_DCS_BUF_ADD_CMD(dev, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00);
		GS_DCS_BUF_ADD_CMD(dev, 0x6F, 0x11);
		GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, 0xB2, 0x01, 0x01, 0x43);
	} else {
		GS_DCS_BUF_ADD_CMD(dev, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00);
		GS_DCS_BUF_ADD_CMD(dev, 0x6F, 0x11);
		GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, 0xB2, 0x00, 0x00, 0x41);
	}

	ct3d_update_irc(ctx, hbm_mode, vrefresh);

	ctx->hbm_mode = hbm_mode;
	dev_info(dev, "hbm_on=%d hbm_ircoff=%d\n", GS_IS_HBM_ON(ctx->hbm_mode),
		 GS_IS_HBM_ON_IRC_OFF(ctx->hbm_mode));
}

static void ct3d_mode_set(struct gs_panel *ctx,
			     const struct gs_panel_mode *pmode)
{
	ct3d_change_frequency(ctx, pmode);
}

static void ct3d_get_panel_rev(struct gs_panel *ctx, u32 id)
{
	/* extract command 0xDB */
	const u8 build_code = (id & 0xFF00) >> 8;
	const u8 main = (build_code & 0xE0) >> 3;
	const u8 sub = (build_code & 0x0C) >> 2;

	gs_panel_get_panel_rev(ctx, main | sub);
}

static int ct3d_read_id(struct gs_panel *ctx)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	struct device *dev = ctx->dev;
	char buf[CT3D_DDIC_ID_LEN] = {0};
	int ret;

	GS_DCS_WRITE_CMD(dev, 0xFF, 0xAA, 0x55, 0xA5, 0x81);
	ret = mipi_dsi_dcs_read(dsi, 0xF2, buf, CT3D_DDIC_ID_LEN);
	if (ret != CT3D_DDIC_ID_LEN) {
		dev_warn(dev, "Unable to read DDIC id (%d)\n", ret);
		goto done;
	} else {
		ret = 0;
	}

	bin2hex(ctx->panel_id, buf, CT3D_DDIC_ID_LEN);
done:
	GS_DCS_WRITE_CMD(dev, 0xFF, 0xAA, 0x55, 0xA5, 0x00);
	return ret;
}

static const struct gs_display_underrun_param underrun_param = {
	.te_idle_us = 350,
	.te_var = 1,
};

/* Truncate 8-bit signed value to 6-bit signed value */
#define TO_6BIT_SIGNED(v) ((v) & 0x3F)

static const struct drm_dsc_config ct3d_dsc_cfg = {
	.first_line_bpg_offset = 13,
	.rc_range_params = {
		{0, 0, 0},
		{0, 0, 0},
		{0, 0, 0},
		{0, 0, 0},
		{0, 0, 0},
		{0, 0, 0},
		{0, 0, 0},
		{0, 0, 0},
		{0, 0, 0},
		{4, 10, TO_6BIT_SIGNED(-10)},
		{5, 10, TO_6BIT_SIGNED(-10)},
		{5, 11, TO_6BIT_SIGNED(-10)},
		{5, 11, TO_6BIT_SIGNED(-12)},
		{8, 12, TO_6BIT_SIGNED(-12)},
		{12, 13, TO_6BIT_SIGNED(-12)},
	},
	/* Used DSC v1.2 */
	.dsc_version_major = 1,
	.dsc_version_minor = 2,
	.slice_count = 2,
	.slice_height = 24,
};

#define CT3D_DSC { \
		.enabled = true, \
		.dsc_count = 1, \
		.cfg = &ct3d_dsc_cfg, \
	}

static const struct gs_panel_mode_array ct3d_modes = {
	.num_modes = 2,
	.modes = {
		{
			.mode = {
				.name = "1080x2424@60:60",
				DRM_MODE_TIMING(60, 1080, 32, 12, 16, 2424, 12, 4, 15),
				/* aligned to bootloader setting */
				.type = DRM_MODE_TYPE_PREFERRED,
				.width_mm = WIDTH_MM,
				.height_mm = HEIGHT_MM,
			},
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.te_usec = 8604,
				.bpc = 8,
				.dsc = CT3D_DSC,
				.underrun_param = &underrun_param,
			},
			.te2_timing = {
				.rising_edge = 0,
				.falling_edge = 45,
			},
		},
		{
			.mode = {
				.name = "1080x2424@120:120",
				DRM_MODE_TIMING(120, 1080, 32, 12, 16, 2424, 12, 4, 15),
				.width_mm = WIDTH_MM,
				.height_mm = HEIGHT_MM,
			},
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.te_usec = 274,
				.bpc = 8,
				.dsc = CT3D_DSC,
				.underrun_param = &underrun_param,
			},
			.te2_timing = {
				.rising_edge = 0,
				.falling_edge = 45,
			},
		},
	},
};

static const struct gs_panel_mode_array ct3d_lp_modes = {
	.num_modes = 1,
	.modes = {
		{
			.mode = {
				.name = "1080x2424@30:30",
				DRM_MODE_TIMING(30, 1080, 32, 12, 16, 2424, 12, 4, 15),
				.type = DRM_MODE_TYPE_DRIVER,
				.width_mm = WIDTH_MM,
				.height_mm = HEIGHT_MM,
			},
			.gs_mode = {
				.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
				.vblank_usec = 120,
				.bpc = 8,
				.dsc = CT3D_DSC,
				.underrun_param = &underrun_param,
				.is_lp_mode = true,
			},
		},
	},
};

static void ct3d_debugfs_init(struct drm_panel *panel, struct dentry *root)
{
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

	gs_panel_debugfs_create_cmdset(csroot, &ct3d_init_cmdset, "init");
	dput(csroot);
panel_out:
	dput(panel_root);
}

static void ct3d_panel_init(struct gs_panel *ctx)
{
	ct3d_dimming_frame_setting(ctx, CT3D_DIMMING_FRAME);
}

static int ct3d_panel_probe(struct mipi_dsi_device *dsi)
{
	struct ct3d_panel *spanel;

	spanel = devm_kzalloc(&dsi->dev, sizeof(*spanel), GFP_KERNEL);
	if (!spanel)
		return -ENOMEM;

	spanel->is_hbm2_enabled = false;
	return gs_dsi_panel_common_init(dsi, &spanel->base);
}

static const struct drm_panel_funcs ct3d_drm_funcs = {
	.disable = ct3d_disable,
	.unprepare = gs_panel_unprepare,
	.prepare = gs_panel_prepare,
	.enable = ct3d_enable,
	.get_modes = gs_panel_get_modes,
	.debugfs_init = ct3d_debugfs_init,
};

static int ct3d_panel_config(struct gs_panel *ctx);

static const struct gs_panel_funcs ct3d_gs_funcs = {
	.set_brightness = ct3d_set_brightness,
	.set_lp_mode = gs_panel_set_lp_mode_helper,
	.set_nolp_mode = ct3d_set_nolp_mode,
	.set_binned_lp = gs_panel_set_binned_lp_helper,
	.set_hbm_mode = ct3d_set_hbm_mode,
	.set_dimming = ct3d_set_dimming,
	.is_mode_seamless = gs_panel_is_mode_seamless_helper,
	.mode_set = ct3d_mode_set,
	.panel_init = ct3d_panel_init,
	.panel_config = ct3d_panel_config,
	.get_panel_rev = ct3d_get_panel_rev,
	.get_te2_edges = gs_panel_get_te2_edges_helper,
	.set_te2_edges = gs_panel_set_te2_edges_helper,
	.update_te2 = ct3d_update_te2,
	.read_id = ct3d_read_id,
	.atomic_check = ct3d_atomic_check,
	.pre_update_ffc = ct3d_pre_update_ffc,
	.update_ffc = ct3d_update_ffc,
};

static const struct gs_brightness_configuration ct3d_btr_configs[] = {
	{
		.panel_rev = PANEL_REV_LATEST,
		.default_brightness = 1816,
		.brt_capability = {
			.normal = {
				.nits = {
					.min = 2,
					.max = 1200,
				},
				.level = {
					.min = 1,
					.max = 3628,
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
					.min = 3629,
					.max = 3939,
				},
				.percentage = {
					.min = 67,
					.max = 100,
				},
			},
		},
	},
};

static struct gs_panel_brightness_desc ct3d_brightness_desc = {
	.max_luminance = 10000000,
	.max_avg_luminance = 1200000,
	.min_luminance = 5,
};

static struct gs_panel_reg_ctrl_desc ct3d_reg_ctrl_desc = {
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

struct gs_panel_desc google_ct3d = {
	.data_lane_cnt = 4,
	/* supported HDR format bitmask : 1(DOLBY_VISION), 2(HDR10), 3(HLG) */
	.hdr_formats = BIT(2) | BIT(3),
	.brightness_desc = &ct3d_brightness_desc,
	.modes = &ct3d_modes,
	.off_cmdset = &ct3d_off_cmdset,
	.lp_modes = &ct3d_lp_modes,
	.lp_cmdset = &ct3d_lp_cmdset,
	.binned_lp = ct3d_binned_lp,
	.num_binned_lp = ARRAY_SIZE(ct3d_binned_lp),
	.has_off_binned_lp_entry = true,
	.reg_ctrl_desc = &ct3d_reg_ctrl_desc,
	.panel_func = &ct3d_drm_funcs,
	.gs_panel_func = &ct3d_gs_funcs,
	.default_dsi_hs_clk_mbps = MIPI_DSI_FREQ_MBPS_DEFAULT,
	.reset_timing_ms = {1, 1, 20},
	.refresh_on_lp = true,
};

static int ct3d_panel_config(struct gs_panel *ctx)
{
	int ret;
	/* b/300383405 Currently, we can't support multiple
	 *  displays in `display_layout_configuration.xml`.
	 */
	/* gs_panel_model_init(ctx, PROJECT, 0); */

	ret = gs_panel_update_brightness_desc(&ct3d_brightness_desc, ct3d_btr_configs,
				  ARRAY_SIZE(ct3d_btr_configs), ctx->panel_rev);

	return ret;
}

static const struct of_device_id gs_panel_of_match[] = {
	{ .compatible = "google,gs-ct3d", .data = &google_ct3d },
	{ }
};
MODULE_DEVICE_TABLE(of, gs_panel_of_match);

static struct mipi_dsi_driver gs_panel_driver = {
	.probe = ct3d_panel_probe,
	.remove = gs_dsi_panel_common_remove,
	.driver = {
		.name = "panel-gs-ct3d",
		.of_match_table = gs_panel_of_match,
	},
};
module_mipi_dsi_driver(gs_panel_driver);

MODULE_AUTHOR("Gil Liu <gilliu@google.com>");
MODULE_DESCRIPTION("MIPI-DSI based Google ct3d panel driver");
MODULE_LICENSE("Dual MIT/GPL");
