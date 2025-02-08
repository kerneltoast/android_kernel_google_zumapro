/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2023 Google LLC
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#include "gs_panel/dcs_helper.h"

#include <linux/delay.h>
#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 19, 0)
#include <drm/display/drm_dsc_helper.h>
#else
#include <drm/drm_dsc.h>
#endif
#include <drm/drm_mipi_dsi.h>
#include <video/mipi_display.h>

#include "gs_panel_internal.h"

void gs_dsi_send_cmdset_flags(struct mipi_dsi_device *dsi, const struct gs_dsi_cmdset *cmdset,
			      u32 panel_rev, u32 flags)
{
	const struct gs_dsi_cmd *c;
	const struct gs_dsi_cmd *last_cmd = NULL;
	const u32 async_mask = GS_PANEL_CMD_SET_BATCH | GS_PANEL_CMD_SET_QUEUE;
	u16 dsi_flags = 0;

	if (!cmdset || !cmdset->num_cmd)
		return;

	/* shouldn't have both queue and batch set together */
	WARN_ON((flags & async_mask) == async_mask);

	if (flags & GS_PANEL_CMD_SET_IGNORE_VBLANK)
		dsi_flags |= GS_DSI_MSG_IGNORE_VBLANK;

	/* if not batched or queued, all commands should be sent out immediately */
	if (flags & async_mask)
		dsi_flags |= GS_DSI_MSG_QUEUE;

	c = &cmdset->cmds[cmdset->num_cmd - 1];
	if (!c->panel_rev) {
		last_cmd = c;
	} else {
		for (; c >= cmdset->cmds; c--) {
			if (c->panel_rev & panel_rev) {
				last_cmd = c;
				break;
			}
		}
	}

	/* no commands to transfer */
	if (!last_cmd)
		return;

	for (c = cmdset->cmds; c <= last_cmd; c++) {
		u32 delay_ms = c->delay_ms;

		if (panel_rev && !(c->panel_rev & panel_rev))
			continue;

		if ((c == last_cmd) && !(flags & GS_PANEL_CMD_SET_QUEUE))
			dsi_flags &= ~GS_DSI_MSG_QUEUE;

		gs_dsi_dcs_write_buffer(dsi, c->cmd, c->cmd_len, dsi_flags);
		if (delay_ms)
			usleep_range(delay_ms * 1000, delay_ms * 1000 + 10);
	}
}
EXPORT_SYMBOL_GPL(gs_dsi_send_cmdset_flags);

void gs_dsi_send_cmdset(struct mipi_dsi_device *dsi, const struct gs_dsi_cmdset *cmdset,
			u32 panel_rev)
{
	gs_dsi_send_cmdset_flags(dsi, cmdset, panel_rev, 0);
}
EXPORT_SYMBOL_GPL(gs_dsi_send_cmdset);

ssize_t gs_dsi_dcs_transfer(struct mipi_dsi_device *dsi, u8 type, const void *data, size_t len,
			    u16 flags)
{
	const struct mipi_dsi_host_ops *ops = dsi->host->ops;
	struct mipi_dsi_msg msg = {
		.channel = dsi->channel,
		.tx_buf = data,
		.tx_len = len,
		.type = type,
	};

	if (!ops || !ops->transfer)
		return -ENOSYS;

	msg.flags = flags;
	if (dsi->mode_flags & MIPI_DSI_MODE_LPM)
		msg.flags |= MIPI_DSI_MSG_USE_LPM;

	return ops->transfer(dsi->host, &msg);
}

static void gs_dcs_write_print_err(struct device *dev, const void *cmd, size_t len, ssize_t ret)
{
	dev_err(dev, "failed to write cmd (%ld)\n", ret);
	print_hex_dump(KERN_ERR, "command: ", DUMP_PREFIX_NONE, 16, 1, cmd, len, false);
}

ssize_t gs_dsi_dcs_write_buffer(struct mipi_dsi_device *dsi, const void *data, size_t len,
				u16 flags)
{
	ssize_t ret;
	u8 type;

	switch (len) {
	case 0:
		/* allow flag only messages to dsim */
		type = 0;
		break;

	case 1:
		type = MIPI_DSI_DCS_SHORT_WRITE;
		break;

	case 2:
		type = MIPI_DSI_DCS_SHORT_WRITE_PARAM;
		break;

	default:
		type = MIPI_DSI_DCS_LONG_WRITE;
		break;
	}

	ret = gs_dsi_dcs_transfer(dsi, type, data, len, flags);
	if (ret < 0)
		gs_dcs_write_print_err(&dsi->dev, data, len, ret);
	return ret;
}
EXPORT_SYMBOL_GPL(gs_dsi_dcs_write_buffer);

#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 19, 0)) || IS_ENABLED(CONFIG_DRM_DISPLAY_DP_HELPER)
int gs_dcs_write_dsc_config(struct device *dev, const struct drm_dsc_config *dsc_cfg)
{
	struct drm_dsc_picture_parameter_set pps;
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(dev);
	int ret;

	drm_dsc_pps_payload_pack(&pps, dsc_cfg);
	ret = mipi_dsi_picture_parameter_set(dsi, &pps);
	if (ret < 0) {
		dev_err(dev, "failed to write pps(%d)\n", ret);
	}
	return ret;
}
EXPORT_SYMBOL_GPL(gs_dcs_write_dsc_config);
#endif
