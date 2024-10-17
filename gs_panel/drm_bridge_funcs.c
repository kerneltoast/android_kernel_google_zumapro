/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2023 Google LLC
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#include "gs_panel_internal.h"

#include <linux/of_gpio.h>
#include <linux/sysfs.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#include "gs_drm/gs_drm_connector.h"
#include "gs_panel/gs_panel.h"
#include "trace/panel_trace.h"

#define bridge_to_gs_panel(b) container_of((b), struct gs_panel, bridge)

#ifndef DISPLAY_PANEL_INDEX_PRIMARY
#define DISPLAY_PANEL_INDEX_PRIMARY 0
#endif
#ifndef DISPLAY_PANEL_INDEX_SECONDARY
#define DISPLAY_PANEL_INDEX_SECONDARY 1
#endif

static unsigned long get_backlight_state_from_panel(struct backlight_device *bl,
						    enum gs_panel_state panel_state)
{
	unsigned long state = bl->props.state;

	switch (panel_state) {
	case GPANEL_STATE_NORMAL:
		state &= ~(BL_STATE_STANDBY | BL_STATE_LP);
		break;
	case GPANEL_STATE_LP:
		state &= ~(BL_STATE_STANDBY);
		state |= BL_STATE_LP;
		break;
	case GPANEL_STATE_MODESET: /* no change */
		break;
	case GPANEL_STATE_OFF:
	case GPANEL_STATE_BLANK:
	default:
		state &= ~(BL_STATE_LP);
		state |= BL_STATE_STANDBY;
		break;
	}

	return state;
}

void gs_panel_set_backlight_state(struct gs_panel *ctx, enum gs_panel_state panel_state)
{
	struct backlight_device *bl = ctx->bl;
	unsigned long state;
	bool state_changed = false;

	if (!bl)
		return;

	mutex_lock(&ctx->bl_state_lock); /*TODO(b/267170999): BL*/

	state = get_backlight_state_from_panel(bl, panel_state);
	if (state != bl->props.state) {
		bl->props.state = state;
		state_changed = true;
	}

	mutex_unlock(&ctx->bl_state_lock); /*TODO(b/267170999): BL*/

	if (state_changed) {
		notify_panel_mode_changed(ctx);
		dev_dbg(ctx->dev, "%s: panel:%d, bl:0x%x\n", __func__, panel_state,
			bl->props.state);
	}
}

static const char *gs_panel_get_sysfs_name(struct gs_panel *ctx)
{
	switch (ctx->gs_connector->panel_index) {
	case DISPLAY_PANEL_INDEX_PRIMARY:
		return "primary-panel";
	case DISPLAY_PANEL_INDEX_SECONDARY:
		return "secondary-panel";
	default:
		dev_warn(ctx->dev, "Unsupported panel_index value %d\n",
			 ctx->gs_connector->panel_index);
		return "primary-panel";
	}
}

void gs_panel_node_attach(struct gs_drm_connector *gs_connector)
{
	struct gs_panel *ctx = gs_connector_to_panel(gs_connector);
	struct drm_connector *connector = &gs_connector->base;
	const char *sysfs_name;
	struct drm_bridge *bridge;
	int ret;

	if (unlikely(!ctx)) {
		WARN(1, "%s: failed to get gs_panel\n", __func__);
		return;
	}

	/* Create sysfs links from connector to panel */
	ret = sysfs_create_link(&gs_connector->kdev->kobj, &ctx->dev->kobj, "panel");
	if (ret)
		dev_warn(ctx->dev, "unable to link connector platform dev to panel (%d)\n", ret);

	ret = sysfs_create_link(&connector->kdev->kobj, &ctx->dev->kobj, "panel");
	if (ret)
		dev_warn(ctx->dev, "unable to link connector drm dev to panel (%d)\n", ret);

	/* debugfs entries */
	gs_panel_create_debugfs_entries(ctx, connector->debugfs_entry);

	bridge = &ctx->bridge;
	sysfs_name = gs_panel_get_sysfs_name(ctx);

	ret = sysfs_create_link(&bridge->dev->dev->kobj, &ctx->dev->kobj, sysfs_name);
	if (ret)
		dev_warn(ctx->dev, "unable to link %s sysfs (%d)\n", sysfs_name, ret);
	else
		dev_dbg(ctx->dev, "succeed to link %s sysfs\n", sysfs_name);
}

static int gs_panel_bridge_attach(struct drm_bridge *bridge, enum drm_bridge_attach_flags flags)
{
	struct gs_panel *ctx = bridge_to_gs_panel(bridge);
	struct device *dev = ctx->dev;
	struct gs_drm_connector *gs_connector = get_gs_drm_connector_parent(ctx);
	struct drm_connector *connector = &gs_connector->base;
	int ret;

	/* Initialize connector, attach properties, and register */
	ret = gs_panel_initialize_gs_connector(ctx, bridge->dev, gs_connector);
	if (ret) {
		return ret;
	}

	ret = drm_connector_attach_encoder(connector, bridge->encoder);
	if (ret) {
		dev_warn(dev, "%s attaching encoder returned nonzero code (%d)\n", __func__, ret);
	}

	if (gs_panel_has_func(ctx, commit_done))
		ctx->gs_connector->needs_commit = true;

	if (connector->dev->mode_config.poll_enabled)
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 17, 0)
		drm_kms_helper_connector_hotplug_event(connector);
#else
		drm_kms_helper_hotplug_event(connector->dev);
#endif

	return 0;
}

static void gs_panel_bridge_detach(struct drm_bridge *bridge)
{
	struct gs_panel *ctx = bridge_to_gs_panel(bridge);
	struct drm_connector *connector = &ctx->gs_connector->base;
	const char *sysfs_name = gs_panel_get_sysfs_name(ctx);

	sysfs_remove_link(&bridge->dev->dev->kobj, sysfs_name);

	/* TODO(tknelms): debugfs removal */
	sysfs_remove_link(&connector->kdev->kobj, "panel");
	/* TODO(tknelms): evaluate what needs to be done to clean up connector */
	drm_connector_unregister(connector);
	drm_connector_cleanup(&ctx->gs_connector->base);
}

static void gs_panel_bridge_enable(struct drm_bridge *bridge,
				   struct drm_bridge_state *old_bridge_state)
{
	struct gs_panel *ctx = bridge_to_gs_panel(bridge);
	const struct drm_connector_state *conn_state = ctx->gs_connector->base.state;
	struct gs_drm_connector_state *gs_conn_state = to_gs_connector_state(conn_state);
	bool need_update_backlight = false;
	bool is_active;
	const bool is_lp_mode = ctx->current_mode && ctx->current_mode->gs_mode.is_lp_mode;

	mutex_lock(&ctx->mode_lock); /*TODO(b/267170999): MODE*/
	if (ctx->panel_state == GPANEL_STATE_HANDOFF) {
		is_active = !gs_panel_first_enable(ctx);
	} else if (ctx->panel_state == GPANEL_STATE_HANDOFF_MODESET) {
		if (!gs_panel_first_enable(ctx)) {
			ctx->panel_state = GPANEL_STATE_MODESET;
			mutex_unlock(&ctx->mode_lock); /*TODO(b/267170999): MODE*/
			drm_panel_disable(&ctx->base);
			mutex_lock(&ctx->mode_lock); /*TODO(b/267170999): MODE*/
		}
		is_active = false;
	} else {
		is_active = gs_is_panel_active(ctx);
	}

	/* avoid turning on panel again if already enabled (ex. while booting or self refresh) */
	if (!is_active) {
		drm_panel_enable(&ctx->base);
		need_update_backlight = true;
	}
	ctx->panel_state = is_lp_mode ? GPANEL_STATE_LP : GPANEL_STATE_NORMAL;

	if (gs_panel_has_func(ctx, update_ffc) &&
	    (!ctx->idle_data.self_refresh_active || gs_conn_state->dsi_hs_clk_changed))
		ctx->desc->gs_panel_func->update_ffc(ctx, gs_conn_state->dsi_hs_clk_mbps);

	if (ctx->idle_data.self_refresh_active) {
		dev_dbg(ctx->dev, "self refresh state : %s\n", __func__);

		ctx->idle_data.self_refresh_active = false;
		panel_update_idle_mode_locked(ctx, false);
	} else {
		gs_panel_set_backlight_state(ctx, ctx->panel_state);
		if (ctx->panel_state == GPANEL_STATE_NORMAL)
			gs_panel_update_te2(ctx);
	}

	if (is_lp_mode && gs_panel_has_func(ctx, set_post_lp_mode))
		ctx->desc->gs_panel_func->set_post_lp_mode(ctx);

	mutex_unlock(&ctx->mode_lock); /*TODO(b/267170999): MODE*/

	if (need_update_backlight && ctx->bl) {
		backlight_update_status(ctx->bl);
	}

	if (!is_active && gs_panel_has_func(ctx, run_normal_mode_work)) {
		dev_dbg(ctx->dev, "%s: schedule normal_mode_work\n", __func__);
		schedule_delayed_work(&ctx->normal_mode_work,
				      msecs_to_jiffies(ctx->normal_mode_work_delay_ms));
	}
}

static void gs_panel_check_mipi_sync_timing(struct drm_crtc *crtc,
					    const struct gs_panel_mode *current_mode,
					    struct gs_panel *ctx)
{
	/*TODO(b/279519827): implement mipi sync timing*/
}

static void bridge_mode_set_enter_lp_mode(struct gs_panel *ctx, const struct gs_panel_mode *pmode,
					  bool is_active)
{
	if (!gs_panel_has_func(ctx, set_lp_mode))
		return;
	if (is_active) {
		/*TODO(b/279521693) _gs_panel_disable_normal_feat_locked(ctx);*/
		ctx->desc->gs_panel_func->set_lp_mode(ctx, pmode);
		ctx->panel_state = GPANEL_STATE_LP;

		if (gs_panel_has_func(ctx, run_normal_mode_work)) {
			dev_dbg(ctx->dev, "%s: cancel normal_mode_work\n", __func__);
			cancel_delayed_work(&ctx->normal_mode_work);
		}
	}
	if (!ctx->regulator.post_vddd_lp_enabled)
		gs_panel_set_vddd_voltage(ctx, true);
	else
		ctx->regulator.need_post_vddd_lp = true;
}

static void bridge_mode_set_leave_lp_mode(struct gs_panel *ctx, const struct gs_panel_mode *pmode,
					  bool is_active)
{
	gs_panel_set_vddd_voltage(ctx, false);
	if (is_active && gs_panel_has_func(ctx, set_nolp_mode)) {
		ctx->desc->gs_panel_func->set_nolp_mode(ctx, pmode);
		ctx->panel_state = GPANEL_STATE_NORMAL;
		/*TODO(b/279521693): lhbm_on_delay_frames*/

		if (gs_panel_has_func(ctx, run_normal_mode_work)) {
			dev_dbg(ctx->dev, "%s: schedule normal_mode_work\n", __func__);
			schedule_delayed_work(&ctx->normal_mode_work,
					      msecs_to_jiffies(ctx->normal_mode_work_delay_ms));
		}
	}
	ctx->current_binned_lp = NULL;

	gs_panel_set_backlight_state(ctx, is_active ? GPANEL_STATE_NORMAL :
						      GPANEL_STATE_OFF);
}

static void bridge_mode_set_normal(struct gs_panel *ctx, const struct gs_panel_mode *pmode,
				 const struct gs_panel_mode *old_mode)
{
	struct drm_connector_state *connector_state = ctx->gs_connector->base.state;
	struct drm_crtc *crtc = connector_state->crtc;
	struct gs_drm_connector_state *gs_connector_state = to_gs_connector_state(connector_state);
	const bool is_active = gs_is_panel_active(ctx);
	const bool was_lp_mode = old_mode && old_mode->gs_mode.is_lp_mode;

	if ((GS_MIPI_CMD_SYNC_REFRESH_RATE & gs_connector_state->mipi_sync) && old_mode)
		gs_panel_check_mipi_sync_timing(crtc, old_mode, ctx);
	if (!gs_is_local_hbm_disabled(ctx) && ctx->desc->lhbm_desc &&
	    !ctx->desc->lhbm_desc->no_lhbm_rr_constraints)
		dev_warn(ctx->dev, "do mode change (`%s`) unexpectedly when LHBM is ON\n",
			 pmode->mode.name);
	ctx->desc->gs_panel_func->mode_set(ctx, pmode);

	if (was_lp_mode)
		gs_panel_set_backlight_state(ctx, is_active ? GPANEL_STATE_NORMAL :
							      GPANEL_STATE_OFF);
	else if (ctx->bl)
		notify_panel_mode_changed(ctx);
}

static void bridge_mode_set_update_timestamps(struct gs_panel *ctx,
					      const struct gs_panel_mode *pmode,
					      const struct gs_panel_mode *old_mode,
					      bool come_out_lp_mode)
{
	struct drm_connector_state *connector_state = ctx->gs_connector->base.state;
	struct drm_crtc *crtc = connector_state->crtc;
	struct gs_drm_connector_state *gs_connector_state = to_gs_connector_state(connector_state);

	if (!old_mode)
		return;
	if ((drm_mode_vrefresh(&pmode->mode) == drm_mode_vrefresh(&old_mode->mode)) &&
		((gs_drm_mode_te_freq(&pmode->mode) == gs_drm_mode_te_freq(&old_mode->mode))))
		return;

	/* save the context in order to predict TE width in
	 * gs_panel_check_mipi_sync_timing
	 */
	ctx->timestamps.last_rr_switch_ts = ktime_get();
	ctx->te2.last_rr = gs_drm_mode_te_freq(&old_mode->mode);
	ctx->te2.last_rr_te_gpio_value = gpio_get_value(gs_connector_state->te_gpio);
	ctx->te2.last_rr_te_counter = drm_crtc_vblank_count(crtc);
	/* TODO(tknelms)
	if (funcs && funcs->base && funcs->base->get_te_usec)
		ctx->te2.last_rr_te_usec =
			funcs->base->get_te_usec(ctx, old_mode);
	else
		ctx->te2.last_rr_te_usec = old_mode->gs_mode.te_usec;
	*/
	if (come_out_lp_mode)
		ctx->timestamps.last_lp_exit_ts = ctx->timestamps.last_rr_switch_ts;
	sysfs_notify(&ctx->dev->kobj, NULL, "refresh_rate");
}

static void gs_panel_bridge_mode_set(struct drm_bridge *bridge, const struct drm_display_mode *mode,
				     const struct drm_display_mode *adjusted_mode)
{
	struct gs_panel *ctx = bridge_to_gs_panel(bridge);
	struct device *dev = ctx->dev;
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	const struct gs_panel_mode *pmode = gs_panel_get_mode(ctx, mode);
	const struct gs_panel_funcs *funcs = ctx->desc->gs_panel_func;
	const struct gs_panel_mode *old_mode;
	bool need_update_backlight = false;
	bool come_out_lp_mode = false;

	if (WARN_ON(!pmode))
		return;

	mutex_lock(&ctx->mode_lock); /*TODO(b/267170999): MODE*/
	old_mode = ctx->current_mode;

	if (old_mode == pmode) {
		mutex_unlock(&ctx->mode_lock); /*TODO(b/267170999): MODE*/
		return;
	}

	if (ctx->panel_state == GPANEL_STATE_HANDOFF) {
		dev_warn(dev, "mode change at boot to %s\n", adjusted_mode->name);
		ctx->panel_state = GPANEL_STATE_HANDOFF_MODESET;
	}

	dev_dbg(dev, "changing display mode to %dx%d@%d\n", pmode->mode.hdisplay,
		pmode->mode.vdisplay, drm_mode_vrefresh(&pmode->mode));

	dsi->mode_flags = pmode->gs_mode.mode_flags;
	ctx->timestamps.last_mode_set_ts = ktime_get();

	PANEL_ATRACE_BEGIN(__func__);
	if (funcs) {
		const bool is_active = gs_is_panel_active(ctx);
		const bool was_lp_mode = old_mode && old_mode->gs_mode.is_lp_mode;
		const bool is_lp_mode = pmode->gs_mode.is_lp_mode;
		bool state_changed = false;

		if (is_lp_mode) {
			bridge_mode_set_enter_lp_mode(ctx, pmode, is_active);
			if (is_active)
				need_update_backlight = true;
		} else if (was_lp_mode && !is_lp_mode) {
			ctx->regulator.need_post_vddd_lp = false;
			bridge_mode_set_leave_lp_mode(ctx, pmode, is_active);
			if (is_active) {
				state_changed = true;
				need_update_backlight = true;
				come_out_lp_mode = true;
			}
		} else if (gs_panel_has_func(ctx, mode_set)) {
			if (is_active) {
				bridge_mode_set_normal(ctx, pmode, old_mode);
				state_changed = true;
			} else
				dev_warn(
					ctx->dev,
					"don't do mode change (`%s`) when panel isn't in interactive mode\n",
					pmode->mode.name);
		}
		ctx->current_mode = pmode;
		if (state_changed) {
			if (!is_lp_mode)
				gs_panel_update_te2(ctx);
		}
	} else {
		ctx->current_mode = pmode;
	}

	bridge_mode_set_update_timestamps(ctx, pmode, old_mode, come_out_lp_mode);

	if (pmode->gs_mode.is_lp_mode && gs_panel_has_func(ctx, set_post_lp_mode))
		funcs->set_post_lp_mode(ctx);

	mutex_unlock(&ctx->mode_lock); /*TODO(b/267170999): MODE*/

	if (need_update_backlight && ctx->bl)
		backlight_update_status(ctx->bl);

	PANEL_ATRACE_INT("panel_fps", drm_mode_vrefresh(mode));
	PANEL_ATRACE_END(__func__);
}

static void gs_panel_bridge_disable(struct drm_bridge *bridge,
				    struct drm_bridge_state *old_bridge_state)
{
	struct gs_panel *ctx = bridge_to_gs_panel(bridge);
	struct device *dev = ctx->dev;
	const struct drm_connector_state *conn_state = ctx->gs_connector->base.state;
	struct gs_drm_connector_state *gs_conn_state = to_gs_connector_state(conn_state);
	struct drm_crtc_state *crtc_state = !conn_state->crtc ? NULL : conn_state->crtc->state;
	const bool self_refresh_active = crtc_state && crtc_state->self_refresh_active;

	if (self_refresh_active && !gs_conn_state->blanked_mode) {
		mutex_lock(&ctx->mode_lock); /*TODO(b/267170999): MODE*/
		dev_dbg(dev, "self refresh state : %s\n", __func__);

		ctx->idle_data.self_refresh_active = true;
		panel_update_idle_mode_locked(ctx, false);
		mutex_unlock(&ctx->mode_lock); /*TODO(b/267170999): MODE*/

		if (ctx->regulator.post_vddd_lp_enabled && ctx->regulator.need_post_vddd_lp) {
			gs_panel_set_vddd_voltage(ctx, true);
			ctx->regulator.need_post_vddd_lp = false;
		}

		if (gs_panel_has_func(ctx, pre_update_ffc) &&
		    (gs_conn_state->dsi_hs_clk_changed || gs_conn_state->pending_dsi_hs_clk_mbps))
			ctx->desc->gs_panel_func->pre_update_ffc(ctx);
	} else {
		if (gs_conn_state->blanked_mode) {
			/* blanked mode takes precedence over normal modeset */
			ctx->panel_state = GPANEL_STATE_BLANK;
		} else if (crtc_state && crtc_state->mode_changed &&
			   drm_atomic_crtc_effectively_active(crtc_state)) {
			ctx->panel_state = GPANEL_STATE_MODESET;
		} else if (ctx->force_power_on) {
			/* force blank state instead of power off */
			ctx->panel_state = GPANEL_STATE_BLANK;
		} else {
			ctx->panel_state = GPANEL_STATE_OFF;
			ctx->mode_in_progress = MODE_DONE;

			if (gs_panel_has_func(ctx, run_normal_mode_work)) {
				dev_dbg(dev, "%s: cancel normal_mode_work\n", __func__);
				cancel_delayed_work(&ctx->normal_mode_work);
			}
		}

		drm_panel_disable(&ctx->base);
	}
}

static void gs_panel_bridge_pre_enable(struct drm_bridge *bridge,
				       struct drm_bridge_state *old_bridge_state)
{
	struct gs_panel *ctx = bridge_to_gs_panel(bridge);

	if (ctx->panel_state == GPANEL_STATE_BLANK) {
		if (gs_panel_has_func(ctx, panel_reset))
			ctx->desc->gs_panel_func->panel_reset(ctx);
	} else if (!gs_is_panel_enabled(ctx))
		drm_panel_prepare(&ctx->base);
}

static void gs_panel_set_partial(struct gs_display_partial *partial,
				 const struct gs_panel_mode *pmode, bool is_partial)
{
	const struct gs_display_dsc *dsc = &pmode->gs_mode.dsc;
	const struct drm_display_mode *mode = &pmode->mode;

	partial->enabled = is_partial;
	if (!partial->enabled)
		return;

	if (dsc->enabled && dsc->cfg) {
		partial->min_width = DIV_ROUND_UP(mode->hdisplay, dsc->cfg->slice_count);
		partial->min_height = dsc->cfg->slice_height;
	} else {
		partial->min_width = MIN_WIN_BLOCK_WIDTH;
		partial->min_height = MIN_WIN_BLOCK_HEIGHT;
	}
}

/**
 * gs_panel_is_mode_seamless() - check if mode transition can be done seamlessly
 * @ctx: Reference to panel data
 * @mode: Proposed display mode
 *
 * Checks whether the panel can transition to the new mode seamlessly without
 * having to turn the display off before the mode change.
 *
 * In most cases, this is only possible if only the clocks and refresh rates are
 * changing.
 *
 * Return: true if seamless transition possible, false otherwise
 */
static bool gs_panel_is_mode_seamless(const struct gs_panel *ctx, const struct gs_panel_mode *mode)
{
	if (!gs_panel_has_func(ctx, is_mode_seamless))
		return false;
	return ctx->desc->gs_panel_func->is_mode_seamless(ctx, mode);
}

static int gs_drm_connector_check_mode(struct gs_panel *ctx,
				       struct drm_connector_state *connector_state,
				       struct drm_crtc_state *crtc_state)
{
	struct gs_drm_connector_state *gs_connector_state = to_gs_connector_state(connector_state);
	const struct gs_panel_mode *pmode = gs_panel_get_mode(ctx, &crtc_state->mode);
	bool is_video_mode;

	if (!pmode) {
		dev_warn(ctx->dev, "invalid mode %s\n", crtc_state->mode.name);
		return -EINVAL;
	}

	is_video_mode = (pmode->gs_mode.mode_flags & MIPI_DSI_MODE_VIDEO) != 0;

	/* self refresh is only supported in command mode */
	connector_state->self_refresh_aware = !is_video_mode;

	if (crtc_state->connectors_changed || !gs_is_panel_active(ctx))
		gs_connector_state->seamless_possible = false;
	else
		gs_connector_state->seamless_possible = gs_panel_is_mode_seamless(ctx, pmode);

	gs_connector_state->gs_mode = pmode->gs_mode;
	gs_panel_set_partial(&gs_connector_state->partial, pmode, ctx->desc->is_partial);

	return 0;
}

/*
 * this atomic check is called after adjusted mode is populated, so it's safe to modify
 * adjusted_mode if needed at this point
 */
static int gs_panel_bridge_atomic_check(struct drm_bridge *bridge,
					struct drm_bridge_state *bridge_state,
					struct drm_crtc_state *new_crtc_state,
					struct drm_connector_state *conn_state)
{
	struct gs_panel *ctx = bridge_to_gs_panel(bridge);
	struct drm_atomic_state *state = new_crtc_state->state;
	const struct drm_display_mode *current_mode = &ctx->current_mode->mode;
	int ret;

	if (unlikely(!new_crtc_state))
		return 0;

	if (unlikely(!current_mode)) {
		dev_warn(ctx->dev, "%s: failed to get current mode, skip mode check\n", __func__);
	} else {
		struct drm_display_mode *target_mode = &new_crtc_state->adjusted_mode;
		struct gs_drm_connector_state *gs_conn_state = to_gs_connector_state(conn_state);
		int current_vrefresh = drm_mode_vrefresh(current_mode);
		int target_vrefresh = drm_mode_vrefresh(target_mode);
		int current_bts_fps = gs_drm_mode_bts_fps(current_mode);
		int target_bts_fps = gs_drm_mode_bts_fps(target_mode);

		int clock;

		/* if resolution changing */
		if (current_mode->hdisplay != target_mode->hdisplay &&
		    current_mode->vdisplay != target_mode->vdisplay) {
			/* if refresh rate changing */
			if (current_vrefresh != target_vrefresh ||
			    current_bts_fps != target_bts_fps) {
				/*
				 * While switching resolution and refresh rate (from high to low) in
				 * the same commit, the frame transfer time will become longer due
				 * to BTS update. In the case, frame done time may cross to the next
				 * vsync, which will hit DDIC’s constraint and cause the noises.
				 * Keep the current BTS (higher one) for a few frames to avoid
				 * the problem.
				 */
				if (current_bts_fps > target_bts_fps) {
					target_mode->clock = gs_bts_fps_to_drm_mode_clock(
						target_mode, current_bts_fps);
					if (target_mode->clock != new_crtc_state->mode.clock) {
						new_crtc_state->mode_changed = true;
						dev_dbg(ctx->dev,
							"%s: keep mode (%s) clock %dhz on rrs\n",
							__func__, target_mode->name,
							current_bts_fps);
					}
					clock = target_mode->clock;
				}

				ctx->mode_in_progress = MODE_RES_AND_RR_IN_PROGRESS;
			/* else refresh rate not changing */
			} else {
				ctx->mode_in_progress = MODE_RES_IN_PROGRESS;
			}
		/* else resolution not changing */
		} else {
			if (ctx->mode_in_progress == MODE_RES_AND_RR_IN_PROGRESS &&
			    new_crtc_state->adjusted_mode.clock != new_crtc_state->mode.clock) {
				new_crtc_state->mode_changed = true;
				new_crtc_state->adjusted_mode.clock = new_crtc_state->mode.clock;
				clock = new_crtc_state->mode.clock;
				dev_dbg(ctx->dev, "%s: restore mode (%s) clock after rrs\n",
					__func__, new_crtc_state->mode.name);
			}

			if ((current_vrefresh != target_vrefresh) ||
			    (current_bts_fps != target_bts_fps))
				ctx->mode_in_progress = MODE_RR_IN_PROGRESS;
			else
				ctx->mode_in_progress = MODE_DONE;
		}

		/* debug output */
		if (current_mode->hdisplay != target_mode->hdisplay ||
		    current_mode->vdisplay != target_mode->vdisplay ||
		    current_vrefresh != target_vrefresh || current_bts_fps != target_bts_fps)
			dev_dbg(ctx->dev,
				"%s: current %dx%d@%d(bts %d), target %dx%d@%d(bts %d), type %d\n",
				__func__, current_mode->hdisplay, current_mode->vdisplay,
				current_vrefresh, current_bts_fps, target_mode->hdisplay,
				target_mode->vdisplay, target_vrefresh, target_bts_fps,
				ctx->mode_in_progress);

		/*
		 * We may transfer the frame for the first TE after switching to higher
		 * op_hz. In this case, the DDIC read speed will become higher while
		 * the DPU write speed will remain the same, so underruns would happen.
		 * Use higher BTS can avoid the issue. Also consider the clock from RRS
		 * and select the higher one.
		 */
		if ((gs_conn_state->pending_update_flags & GS_FLAG_OP_RATE_UPDATE) &&
		    gs_conn_state->operation_rate > ctx->op_hz) {
			target_mode->clock =
				gs_bts_fps_to_drm_mode_clock(target_mode, ctx->peak_bts_fps);
			/* use the higher clock to avoid underruns */
			if (target_mode->clock < clock)
				target_mode->clock = clock;

			if (target_mode->clock != new_crtc_state->mode.clock) {
				new_crtc_state->mode_changed = true;
				ctx->boosted_for_op_hz = true;
				dev_dbg(ctx->dev, "%s: raise mode clock %dhz on op_hz %d\n",
					__func__, ctx->peak_bts_fps, gs_conn_state->operation_rate);
			}
		} else if (ctx->boosted_for_op_hz &&
			   new_crtc_state->adjusted_mode.clock != new_crtc_state->mode.clock) {
			new_crtc_state->mode_changed = true;
			ctx->boosted_for_op_hz = false;
			/* use the higher clock to avoid underruns */
			if (new_crtc_state->mode.clock < clock)
				new_crtc_state->adjusted_mode.clock = clock;
			else
				new_crtc_state->adjusted_mode.clock = new_crtc_state->mode.clock;

			dev_dbg(ctx->dev, "%s: restore mode clock after op_hz\n", __func__);
		}
	}

	if (gs_panel_has_func(ctx, atomic_check)) {
		ret = ctx->desc->gs_panel_func->atomic_check(ctx, state);
		if (ret)
			return ret;
	}

	if (!drm_atomic_crtc_needs_modeset(new_crtc_state))
		return 0;

	if (ctx->panel_state == GPANEL_STATE_HANDOFF) {
		struct drm_crtc_state *old_crtc_state =
			drm_atomic_get_old_crtc_state(state, new_crtc_state->crtc);

		if (!old_crtc_state->enable)
			old_crtc_state->self_refresh_active = true;
	}

	return gs_drm_connector_check_mode(ctx, conn_state, new_crtc_state);
}

static void gs_panel_bridge_post_disable(struct drm_bridge *bridge,
					 struct drm_bridge_state *old_bridge_state)
{
	struct gs_panel *ctx = bridge_to_gs_panel(bridge);

	/* fully power off only if panel is in full off mode */
	if (!gs_is_panel_enabled(ctx))
		drm_panel_unprepare(&ctx->base);

	gs_panel_set_backlight_state(ctx, ctx->panel_state);
}

static const struct drm_bridge_funcs gs_panel_bridge_funcs = {
	.attach = gs_panel_bridge_attach,
	.detach = gs_panel_bridge_detach,
	.atomic_enable = gs_panel_bridge_enable,
	.atomic_disable = gs_panel_bridge_disable,
	.atomic_check = gs_panel_bridge_atomic_check,
	.atomic_pre_enable = gs_panel_bridge_pre_enable,
	.atomic_post_disable = gs_panel_bridge_post_disable,
	.atomic_duplicate_state = drm_atomic_helper_bridge_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_bridge_destroy_state,
	.atomic_reset = drm_atomic_helper_bridge_reset,
	.mode_set = gs_panel_bridge_mode_set,
};

const struct drm_bridge_funcs *get_panel_drm_bridge_funcs(void)
{
	return &gs_panel_bridge_funcs;
}
