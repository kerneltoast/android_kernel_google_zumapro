/* SPDX-License-Identifier: MIT */

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

/* DSC1.1 SCR V4 */
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
	.initial_dec_delay = 594,
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
	.scale_decrement_interval = 7,
	.scale_increment_interval = 2241,
	.nfl_bpg_offset = 308,
	.slice_bpg_offset = 258,
	.final_offset = 4336,
	.vbr_enable = false,
	.slice_chunk_size = 540,
	.dsc_version_minor = 1,
	.dsc_version_major = 1,
	.native_422 = false,
	.native_420 = false,
	.second_line_bpg_offset = 0,
	.nsl_bpg_offset = 0,
	.second_line_offset_adj = 0,
};

#define CT3C_WRCTRLD_DIMMING_BIT    0x08
#define CT3C_WRCTRLD_BCTRL_BIT      0x20

static const u8 test_key_enable[] = { 0xF0, 0x5A, 0x5A };
static const u8 test_key_disable[] = { 0xF0, 0xA5, 0xA5 };
static const u8 ltps_update[] = { 0xF7, 0x0F };
static const u8 pixel_off[] = { 0x22 };

static const struct gs_dsi_cmd ct3c_off_cmds[] = {
	GS_DSI_CMD(MIPI_DCS_SET_DISPLAY_OFF),
	GS_DSI_DELAY_CMD(120, MIPI_DCS_ENTER_SLEEP_MODE),
};
static DEFINE_GS_CMDSET(ct3c_off);

static const struct gs_dsi_cmd ct3c_lp_cmds[] = {
	GS_DSI_REV_CMD(PANEL_REV_GE(PANEL_REV_EVT1_1),
		MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x24),
};
static DEFINE_GS_CMDSET(ct3c_lp);

static const struct gs_dsi_cmd ct3c_lp_low_cmds[] = {
	/* Proto 1.0 */
	GS_DSI_REV_CMDLIST(PANEL_REV_PROTO1, test_key_enable),
	GS_DSI_REV_CMD(PANEL_REV_PROTO1, 0x91, 0x01), /* NEW Gamma IP Bypass */
	GS_DSI_REV_CMD(PANEL_REV_PROTO1, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x25), /* AOD 10 nit */
	GS_DSI_REV_CMDLIST(PANEL_REV_PROTO1, test_key_disable),

	/* Proto 1.1, EVT 1.0 */
	GS_DSI_REV_CMD(PANEL_REV_PROTO1_1 | PANEL_REV_EVT1,
		MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x25), /* AOD 10 nit */

	/* EVT 1.1 and later */
	GS_DSI_REV_CMD(PANEL_REV_GE(PANEL_REV_EVT1_1),
		MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x01, 0x7E),
};

static const struct gs_dsi_cmd ct3c_lp_high_cmds[] = {
	/* Proto 1.0 */
	GS_DSI_REV_CMDLIST(PANEL_REV_PROTO1, test_key_enable),
	GS_DSI_REV_CMD(PANEL_REV_PROTO1, 0x91, 0x01), /* NEW Gamma IP Bypass */
	GS_DSI_REV_CMD(PANEL_REV_PROTO1, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x24), /* AOD 50 nit */
	GS_DSI_REV_CMDLIST(PANEL_REV_PROTO1, test_key_disable),

	/* Proto 1.1, EVT 1.0 */
	GS_DSI_REV_CMD(PANEL_REV_PROTO1_1 | PANEL_REV_EVT1,
		MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x24), /* AOD 50 nit */

	/* EVT 1.1 and later */
	GS_DSI_REV_CMD(PANEL_REV_GE(PANEL_REV_EVT1_1),
		MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x03, 0x1A),
};

static const struct gs_binned_lp ct3c_binned_lp[] = {
	/* low threshold 40 nits */
	BINNED_LP_MODE_TIMING("low", 717, ct3c_lp_low_cmds,
			      12, 12 + 50),
	BINNED_LP_MODE_TIMING("high", 3427, ct3c_lp_high_cmds,
			      12, 12 + 50),
};

static const struct gs_dsi_cmd ct3c_init_cmds[] = {
	/* TE on */
	GS_DSI_CMD(MIPI_DCS_SET_TEAR_ON),

	GS_DSI_CMDLIST(test_key_enable),
	/* TE Width Settings */
	GS_DSI_REV_CMD(PANEL_REV_GE(PANEL_REV_EVT1_1), 0xB9, 0x01),
	/* FREQ CON Set */
	GS_DSI_CMD(0xB0, 0x00, 0x27, 0xF2),
	GS_DSI_CMD(0xF2, 0x02),
	/* TE2 setting */
	GS_DSI_CMD(0xB0, 0x00, 0x69, 0xCB),
	GS_DSI_CMD(0xCB, 0x10, 0x00, 0x30), /* 60HS TE2 ON */
	GS_DSI_CMD(0xB0, 0x00, 0xE9, 0xCB),
	GS_DSI_CMD(0xCB, 0x10, 0x00, 0x30), /* 120HS TE2 ON */
	GS_DSI_CMD(0xB0, 0x01, 0x69, 0xCB),
	GS_DSI_CMD(0xCB, 0x10, 0x00, 0x2D), /* AOD TE2 ON */

	/* TSP Sync set (only for P1.0, P1.1) */
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_EVT1), 0xB0, 0x00, 0x0D, 0xB9),
	GS_DSI_REV_CMD(PANEL_REV_LT(PANEL_REV_EVT1), 0xB9, 0xB1, 0xA1),
	GS_DSI_CMDLIST(ltps_update),
	/* ELVSS Caloffset Set (only for P1.1) */
	GS_DSI_REV_CMD(PANEL_REV_PROTO1_1, 0xB0, 0x00, 0x02, 0x63),
	GS_DSI_REV_CMD(PANEL_REV_PROTO1_1, 0x63, 0x15, 0x0A, 0x15, 0x0A, 0x15, 0x0A),
	GS_DSI_CMDLIST(test_key_disable),

	/* CASET: 1080 */
	GS_DSI_CMD(MIPI_DCS_SET_COLUMN_ADDRESS, 0x00, 0x00, 0x04, 0x37),

	/* PASET: 2424 */
	GS_DSI_CMD(MIPI_DCS_SET_PAGE_ADDRESS, 0x00, 0x00, 0x09, 0x77),
};
static DEFINE_GS_CMDSET(ct3c_init);

/**
 * struct ct3c_panel - panel specific runtime info
 *
 * This struct maintains ct3c panel specific runtime info, any fixed details about panel
 * should most likely go into struct gs_panel_desc
 */
struct ct3c_panel {
	/** @base: base panel struct */
	struct gs_panel base;
	/**
	 * @is_pixel_off: pixel-off command is sent to panel. Only sending normal-on or resetting
	 *		  panel can recover to normal mode after entering pixel-off state.
	 */
	bool is_pixel_off;
};
#define to_spanel(ctx) container_of(ctx, struct ct3c_panel, base)

static void ct3c_proto_change_frequency(struct gs_panel *ctx, const struct gs_panel_mode *pmode)
{
	struct device *dev = ctx->dev;
	u32 vrefresh = drm_mode_vrefresh(&pmode->mode);

	if (unlikely(!ctx))
		return;

	if (vrefresh != 60 && vrefresh != 120) {
		dev_warn(dev, "%s: invalid refresh rate %uhz\n", __func__, vrefresh);
		return;
	}

	if (vrefresh > ctx->op_hz) {
		dev_err(dev, "invalid freq setting: op_hz=%u, vrefresh=%u\n",
				ctx->op_hz, vrefresh);
		return;
	}

	GS_DCS_BUF_ADD_CMDLIST(dev, test_key_enable);
	GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x27, 0xF2);

	if (ctx->op_hz == 60) {
		GS_DCS_BUF_ADD_CMD(dev, 0xF2, 0x82);
		GS_DCS_BUF_ADD_CMD(dev, 0x60, 0x00);
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x0E, 0xF2);
		GS_DCS_BUF_ADD_CMD(dev, 0xF2, 0x00, 0x0C);
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x4C, 0xF6);
		GS_DCS_BUF_ADD_CMD(dev, 0xF6, 0x21, 0x0E);
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x88, 0xCB);
		GS_DCS_BUF_ADD_CMD(dev, 0xCB, 0x14, 0x13);
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x8E, 0xCB);
		GS_DCS_BUF_ADD_CMD(dev, 0xCB, 0x14, 0x13);
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0xA6, 0xCB);
		GS_DCS_BUF_ADD_CMD(dev, 0xCB, 0x03, 0x0A, 0x0F, 0x11);
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0xBF, 0xCB);
		GS_DCS_BUF_ADD_CMD(dev,
			0xCB, 0x06, 0x33, 0xF8, 0x06, 0x47, 0xD8, 0x06, 0x33, 0xD8, 0x06, 0x47);
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x01, 0xFD, 0xCB);
		GS_DCS_BUF_ADD_CMD(dev, 0xCB, 0x1B, 0x1B);
	} else {
		GS_DCS_BUF_ADD_CMD(dev, 0xF2, 0x02);
		GS_DCS_BUF_ADD_CMD(dev, 0x60, (vrefresh == 120) ? 0x00 : 0x08);
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x07, 0xF2);
		if (vrefresh == 120) {
			GS_DCS_BUF_ADD_CMD(dev, 0xF2, 0x00, 0x0C);
		} else {
			GS_DCS_BUF_ADD_CMD(dev, 0xF2, 0x09, 0x9C);
		}
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x4C, 0xF6);
		GS_DCS_BUF_ADD_CMD(dev, 0xF6, 0x43, 0x1C);
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x88, 0xCB);
		GS_DCS_BUF_ADD_CMD(dev, 0xCB, 0x27, 0x26);
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x8E, 0xCB);
		GS_DCS_BUF_ADD_CMD(dev, 0xCB, 0x27, 0x26);
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0xA6, 0xCB);
		GS_DCS_BUF_ADD_CMD(dev, 0xCB, 0x07, 0x14, 0x20, 0x22);
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0xBF, 0xCB);
		GS_DCS_BUF_ADD_CMD(dev,
			0xCB, 0x0B, 0x19, 0xF8, 0x0B, 0x8D, 0xD8, 0x0B, 0x19, 0xD8, 0x0B, 0x8D);
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x01, 0xFD, 0xCB);
		GS_DCS_BUF_ADD_CMD(dev, 0xCB, 0x36, 0x36);
	}

	GS_DCS_BUF_ADD_CMDLIST(dev, ltps_update);
	GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, test_key_disable);

	dev_info(dev, "%s: change to %uHz, op_hz=%u\n", __func__, vrefresh, ctx->op_hz);
}

static void ct3c_freq_change_command(struct gs_panel *ctx, const u32 vrefresh)
{
	struct device *dev = ctx->dev;

	GS_DCS_BUF_ADD_CMDLIST(dev, test_key_enable);

	/* EM Off Change */
	if (vrefresh == 60) {
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x01, 0xD4, 0x65);
		GS_DCS_BUF_ADD_CMD(dev, 0x65, 0x13, 0x20, 0x11, 0x38, 0x11, 0x38, 0x11, 0x38,
			0x11, 0x38, 0x10, 0x1D, 0x0E, 0x9B, 0x0D, 0x18,
			0x0B, 0x94, 0x0A, 0x15, 0x08, 0x97, 0x07, 0x15,
			0x02, 0x90, 0x02, 0x90, 0x01, 0x48, 0x01, 0x48);
	} else {
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x01, 0xB4, 0x65);
		GS_DCS_BUF_ADD_CMD(dev, 0x65, 0x09, 0x90, 0x08, 0x9C, 0x08, 0x9C, 0x08, 0x9C,
			0x08, 0x9C, 0x08, 0x0E, 0x07, 0x4D, 0x06, 0x8C,
			0x05, 0xCA, 0x05, 0x0B, 0x04, 0x4C, 0x03, 0x8A,
			0x01, 0x48, 0x01, 0x48, 0x00, 0xA4, 0x00, 0xA4);
	}
	GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x2A, 0x6A);

	/* Gamma change */
	GS_DCS_BUF_ADD_CMD(dev, 0x6A, 0x00, 0x00, 0x00);

	/* Frequency and Porch Change */
	if (vrefresh == 60) {
		GS_DCS_BUF_ADD_CMD(dev, 0x60, 0x00, 0x00);
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x0E, 0xF2);
		GS_DCS_BUF_ADD_CMD(dev, 0xF2, 0x09, 0x9C);
	} else {
		GS_DCS_BUF_ADD_CMD(dev, 0x60, 0x08, 0x00);
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x00, 0x07, 0xF2);
		GS_DCS_BUF_ADD_CMD(dev, 0xF2, 0x00, 0x0C);
	}

	GS_DCS_BUF_ADD_CMDLIST(dev, ltps_update);
	GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, test_key_disable);
}

static void ct3c_te_change_command(struct gs_panel *ctx, const u32 vrefresh)
{
	struct device *dev = ctx->dev;

	GS_DCS_BUF_ADD_CMDLIST(dev, test_key_enable);
	GS_DCS_BUF_ADD_CMD(dev, 0xB9, 0x01);
	GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, test_key_disable);
}

static void ct3c_evt_change_frequency(struct gs_panel *ctx, const struct gs_panel_mode *pmode)
{
	u32 vrefresh = drm_mode_vrefresh(&pmode->mode);
	if (!ctx || ((vrefresh != 60) && (vrefresh != 120)))
		return;

	ct3c_freq_change_command(ctx, vrefresh);
	ct3c_te_change_command(ctx, vrefresh);

	dev_info(ctx->dev, "%s: change to %uHz\n", __func__, vrefresh);
	return;
}

static void ct3c_change_frequency(struct gs_panel *ctx, const struct gs_panel_mode *pmode)
{
	struct device *dev = ctx->dev;

	u32 vrefresh = drm_mode_vrefresh(&pmode->mode);

	if (ctx->panel_rev == PANEL_REV_PROTO1) {
		ct3c_proto_change_frequency(ctx, pmode);
	} else if (ctx->panel_rev == PANEL_REV_EVT1) {
		ct3c_evt_change_frequency(ctx, pmode);
	} else {
		if (!ctx || ((vrefresh != 60) && (vrefresh != 120)))
			return;

		GS_DCS_BUF_ADD_CMDLIST(dev, test_key_enable);
		GS_DCS_BUF_ADD_CMD(dev, 0x60, (vrefresh == 120) ? 0x08 : 0x00);
		GS_DCS_BUF_ADD_CMDLIST(dev, ltps_update);
		GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, test_key_disable);

		dev_info(dev, "%s: change to %uHz\n", __func__, vrefresh);
		return;
	}
}

static int ct3c_set_op_hz(struct gs_panel *ctx, unsigned int hz)
{
	const struct gs_panel_mode *pmode = ctx->current_mode;
	const unsigned int vrefresh = drm_mode_vrefresh(&ctx->current_mode->mode);

	if ((vrefresh > hz) || ((hz != 60) && (hz != 120))) {
		dev_err(ctx->dev, "invalid op_hz=%u for vrefresh=%u\n",
			hz, vrefresh);
		return -EINVAL;
	}

	ctx->op_hz = hz;

	if (ctx->panel_rev == PANEL_REV_PROTO1) {
		ct3c_change_frequency(ctx, pmode);
		dev_info(ctx->dev, "set op_hz at %u\n", hz);
	} else {
		dev_info(ctx->dev, "Panel rev %d always operates at op_hz=120\n", ctx->panel_rev);
	}

	return 0;
}

static void ct3c_update_wrctrld(struct gs_panel *ctx)
{
	u8 val = CT3C_WRCTRLD_BCTRL_BIT;

	if (ctx->dimming_on)
		val |= CT3C_WRCTRLD_DIMMING_BIT;

	dev_dbg(ctx->dev,
		"%s(wrctrld:0x%x, hbm_on: %d, dimming_on: %d)\n",
		__func__, val, GS_IS_HBM_ON(ctx->hbm_mode),
		ctx->dimming_on);

	GS_DCS_BUF_ADD_CMD_AND_FLUSH(ctx->dev, MIPI_DCS_WRITE_CONTROL_DISPLAY, val);
}

static int ct3c_set_brightness(struct gs_panel *ctx, u16 br)
{
	struct device *dev = ctx->dev;
	struct ct3c_panel *spanel = to_spanel(ctx);
	u16 brightness;
	u32 max_brightness;

	if (ctx->current_mode && ctx->current_mode->gs_mode.is_lp_mode) {
		const struct gs_panel_funcs *funcs;

		/* don't stay at pixel-off state in AOD, or black screen is possibly seen */
		if (spanel->is_pixel_off) {
			GS_DCS_WRITE_CMD(dev, MIPI_DCS_ENTER_NORMAL_MODE);
			spanel->is_pixel_off = false;
		}
		funcs = ctx->desc->gs_panel_func;
		if (funcs && funcs->set_binned_lp)
			funcs->set_binned_lp(ctx, br);
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

static void ct3c_set_hbm_mode(struct gs_panel *ctx, enum gs_hbm_mode mode)
{
	struct device *dev = ctx->dev;

	ctx->hbm_mode = mode;

	GS_DCS_BUF_ADD_CMDLIST(dev, test_key_enable);

	if (ctx->panel_rev == PANEL_REV_PROTO1) {
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x01, 0x18, 0x68);
		/* FGZ mode enable (IRC off) / FLAT gamma (default, IRC on)  */
		GS_DCS_BUF_ADD_CMD(dev, 0x68, GS_IS_HBM_ON_IRC_OFF(ctx->hbm_mode) ? 0x82 : 0x00);

		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x01, 0x19, 0x68);
		GS_DCS_BUF_ADD_CMD(dev, 0x68, 0x00, 0x00, 0x00, 0x96, 0xFA, 0x0C, 0x80, 0x00,
			0x00, 0x0A, 0xD5, 0xFF, 0x94, 0x00, 0x00); /* FGZ mode */
	} else {
		GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x01, 0x22, 0x68);
		if (GS_IS_HBM_ON_IRC_OFF(ctx->hbm_mode)) {
			/* FGZ Mode ON */
			if (ctx->panel_rev == PANEL_REV_PROTO1_1) {
				GS_DCS_BUF_ADD_CMD(dev, 0x68, 0x2D, 0xF1, 0xFF, 0x94);
			} else if (ctx->panel_rev == PANEL_REV_EVT1) {
				GS_DCS_BUF_ADD_CMD(dev, 0x68, 0x40, 0x00, 0xFF, 0x9C);
			} else {
				GS_DCS_BUF_ADD_CMD(dev, 0x68, 0x28, 0xED, 0xFF, 0x94);
			}
		} else
			GS_DCS_BUF_ADD_CMD(dev, 0x68, 0x00, 0x00, 0xFF, 0x90); /* FGZ Mode OFF */
	}

	GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, test_key_disable);

	dev_info(dev, "hbm_on=%d hbm_ircoff=%d.\n", GS_IS_HBM_ON(ctx->hbm_mode),
		 GS_IS_HBM_ON_IRC_OFF(ctx->hbm_mode));
}

static void ct3c_set_dimming_on(struct gs_panel *gs_panel, bool dimming_on)
{
	const struct gs_panel_mode *pmode = gs_panel->current_mode;
	gs_panel->dimming_on = dimming_on;

	if (pmode->gs_mode.is_lp_mode) {
		dev_warn(gs_panel->dev, "in lp mode, skip to update\n");
		return;
	}

	ct3c_update_wrctrld(gs_panel);
}

static void ct3c_mode_set(struct gs_panel *ctx, const struct gs_panel_mode *pmode)
{
	ct3c_change_frequency(ctx, pmode);
}

static void ct3c_debugfs_init(struct drm_panel *panel, struct dentry *root)
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

	gs_panel_debugfs_create_cmdset(csroot, &ct3c_init_cmdset, "init");

	dput(csroot);
panel_out:
	dput(panel_root);
#endif
}

static void ct3c_get_panel_rev(struct gs_panel *ctx, u32 id)
{
	/* extract command 0xDB */
	u8 build_code = (id & 0xFF00) >> 8;
	u8 main = (build_code & 0xE0) >> 3;
	u8 sub = (build_code & 0x0C) >> 2;
	u8 rev = main | sub;

	/* proto panel will have a +1 revision compared to device */
	if (main == 0)
		rev--;

	gs_panel_get_panel_rev(ctx, rev);
}

static void ct3c_set_lp_mode(struct gs_panel *ctx,
					const struct gs_panel_mode *pmode)
{
	gs_panel_set_lp_mode_helper(ctx, pmode);
	ct3c_te_change_command(ctx, drm_mode_vrefresh(&ctx->current_mode->mode));
}

static void ct3c_set_nolp_mode(struct gs_panel *ctx, const struct gs_panel_mode *pmode)
{
	struct device *dev = ctx->dev;
	const struct gs_panel_mode *current_mode = ctx->current_mode;
	unsigned int vrefresh = current_mode ? drm_mode_vrefresh(&current_mode->mode) : 30;
	unsigned int te_usec = current_mode ? current_mode->gs_mode.te_usec : 1109;

	if (!gs_is_panel_active(ctx))
		return;

	GS_DCS_BUF_ADD_CMD(dev, MIPI_DCS_SET_DISPLAY_OFF);

	/* AOD Mode Off Setting */
	GS_DCS_BUF_ADD_CMDLIST(dev, test_key_enable);
	GS_DCS_BUF_ADD_CMD(dev, 0x91, 0x02);
	GS_DCS_BUF_ADD_CMD(dev, 0x53, 0x20);
	GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, test_key_disable);

	/* backlight control and dimming */
	ct3c_update_wrctrld(ctx);
	ct3c_change_frequency(ctx, pmode);

	PANEL_ATRACE_BEGIN("ct3c_wait_one_vblank");
	gs_panel_wait_for_vsync_done(ctx, te_usec,
			GS_VREFRESH_TO_PERIOD_USEC(vrefresh));

	/* Additional sleep time to account for TE variability */
	usleep_range(1000, 1010);
	PANEL_ATRACE_END("ct3c_wait_one_vblank");

	GS_DCS_BUF_ADD_CMD_AND_FLUSH(dev, MIPI_DCS_SET_DISPLAY_ON);

	dev_info(dev, "exit LP mode\n");
}

static void ct3c_10bit_set(struct gs_panel *ctx)
{
	struct device *dev = ctx->dev;

	GS_DCS_BUF_ADD_CMDLIST(dev, test_key_enable);
	GS_DCS_BUF_ADD_CMD(dev, 0xB0, 0x28, 0xF2);
	GS_DCS_BUF_ADD_CMD(dev, 0xF2, 0xCC);  /* 10bit */
	GS_DCS_BUF_ADD_CMDLIST_AND_FLUSH(dev, test_key_disable);
}

static int ct3c_enable(struct drm_panel *panel)
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

	ct3c_10bit_set(ctx);

	/* initial command */
	gs_panel_send_cmdset(ctx, &ct3c_init_cmdset);

	/* frequency */
	ct3c_change_frequency(ctx, pmode);

	/* DSC related configuration */
	mipi_dsi_compression_mode(to_mipi_dsi_device(dev), true);
	gs_dcs_write_dsc_config(dev, &pps_config);
	/* DSC Enable */
	GS_DCS_BUF_ADD_CMD(dev, 0xC2, 0x14);
	GS_DCS_BUF_ADD_CMD(dev, 0x9D, 0x01);

	/* dimming and HBM */
	ct3c_update_wrctrld(ctx);

	if (pmode->gs_mode.is_lp_mode)
		gs_panel_set_lp_mode_helper(ctx, pmode);

	/* display on */
	GS_DCS_WRITE_CMD(dev, MIPI_DCS_SET_DISPLAY_ON);

	return 0;
}

static int ct3c_panel_probe(struct mipi_dsi_device *dsi)
{
	struct ct3c_panel *spanel;

	spanel = devm_kzalloc(&dsi->dev, sizeof(*spanel), GFP_KERNEL);
	if (!spanel)
		return -ENOMEM;

	/*
		When refresh rate = 120Hz, op rate = 120 (120Hz HS)
		When refresh rate = 60Hz, op rate = 60 (60Hz NS) / 120 (60Hz HS)
		Default settings: 60Hz HS
	*/
	spanel->base.op_hz = 120;
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

#define CT3C_DSC {\
	.enabled = true,\
	.dsc_count = 1,\
	.cfg = &pps_config,\
}

static const struct gs_panel_mode_array ct3c_modes = {
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
				.te_usec = 8615,
				.bpc = 8,
				.dsc = CT3C_DSC,
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
				.te_usec = 276, /* b/273191882 */
				.bpc = 8,
				.dsc = CT3C_DSC,
				.underrun_param = &underrun_param,
			},
		},
	},
};

const struct brightness_capability ct3c_brightness_capability = {
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

static const struct gs_panel_mode_array ct3c_lp_mode = {
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
				.dsc = CT3C_DSC,
				.underrun_param = &underrun_param,
				.is_lp_mode = true,
			},
		},
	},
};

static const struct drm_panel_funcs ct3c_drm_funcs = {
	.disable = gs_panel_disable,
	.unprepare = gs_panel_unprepare,
	.prepare = gs_panel_prepare,
	.enable = ct3c_enable,
	.get_modes = gs_panel_get_modes,
	.debugfs_init = ct3c_debugfs_init,
};

static const struct gs_panel_funcs ct3c_gs_funcs = {
	.set_brightness = ct3c_set_brightness,
	.set_lp_mode = ct3c_set_lp_mode,
	.set_nolp_mode = ct3c_set_nolp_mode,
	.set_binned_lp = gs_panel_set_binned_lp_helper,
	.set_dimming = ct3c_set_dimming_on,
	.set_hbm_mode = ct3c_set_hbm_mode,
	.set_op_hz = ct3c_set_op_hz,
	.is_mode_seamless = gs_panel_is_mode_seamless_helper,
	.mode_set = ct3c_mode_set,
	.get_panel_rev = ct3c_get_panel_rev,
	.read_id = gs_panel_read_slsi_ddic_id,
};

const struct gs_panel_brightness_desc ct3c_brightness_desc = {
	.max_brightness = 3818,
	.min_brightness = 2,
	.max_luminance = 10000000,
	.max_avg_luminance = 1200000,
	.min_luminance = 5,
	.default_brightness = 1290,    /* 140 nits */
	.brt_capability = &ct3c_brightness_capability,
};

const struct gs_panel_reg_ctrl_desc ct3c_reg_ctrl_desc = {
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

const struct gs_panel_desc google_ct3c = {
	.data_lane_cnt = 4,
	/* supported HDR format bitmask : 1(DOLBY_VISION), 2(HDR10), 3(HLG) */
	.hdr_formats = BIT(2) | BIT(3),
	.brightness_desc = &ct3c_brightness_desc,
	.modes = &ct3c_modes,
	.off_cmdset = &ct3c_off_cmdset,
	.lp_modes = &ct3c_lp_mode,
	.lp_cmdset = &ct3c_lp_cmdset,
	.binned_lp = ct3c_binned_lp,
	.num_binned_lp = ARRAY_SIZE(ct3c_binned_lp),
	.reg_ctrl_desc = &ct3c_reg_ctrl_desc,
	.panel_func = &ct3c_drm_funcs,
	.gs_panel_func = &ct3c_gs_funcs,
	.reset_timing_ms = {-1, 1, 1},
};

static const struct of_device_id gs_panel_of_match[] = {
	{ .compatible = "google,gs-ct3c", .data = &google_ct3c },
	{ }
};
MODULE_DEVICE_TABLE(of, gs_panel_of_match);

static struct mipi_dsi_driver gs_panel_driver = {
	.probe = ct3c_panel_probe,
	.remove = gs_dsi_panel_common_remove,
	.driver = {
		.name = "panel-gs-ct3c",
		.of_match_table = gs_panel_of_match,
	},
};
module_mipi_dsi_driver(gs_panel_driver);

MODULE_AUTHOR("Safayat Ullah <safayat@google.com>");
MODULE_DESCRIPTION("MIPI-DSI based Google ct3c panel driver");
MODULE_LICENSE("Dual MIT/GPL");
