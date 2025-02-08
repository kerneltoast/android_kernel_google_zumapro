/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2023 Google LLC
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#ifndef _GS_DISPLAY_MODE_H_
#define _GS_DISPLAY_MODE_H_

#include <linux/version.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 19, 0)
#include <drm/display/drm_dsc.h>
#else
#include <drm/drm_dsc.h>
#endif

/* customized DRM mode type and flags */
#define DRM_MODE_TYPE_VRR DRM_MODE_TYPE_USERDEF
#define DRM_MODE_FLAG_NS DRM_MODE_FLAG_CLKDIV2
#define DRM_MODE_FLAG_TE_FREQ_X1 DRM_MODE_FLAG_PHSYNC
#define DRM_MODE_FLAG_TE_FREQ_X2 DRM_MODE_FLAG_NHSYNC
#define DRM_MODE_FLAG_TE_FREQ_X4 DRM_MODE_FLAG_PVSYNC
#define DRM_MODE_FLAG_TE_FREQ_MASK \
	(DRM_MODE_FLAG_TE_FREQ_X1 | DRM_MODE_FLAG_TE_FREQ_X2 | DRM_MODE_FLAG_TE_FREQ_X4)

// BTS need takes operation rate into account
#define DRM_MODE_FLAG_BTS_OP_RATE DRM_MODE_FLAG_NVSYNC
#define IS_BTS2OPRATE_MODE(t) ((t) & DRM_MODE_FLAG_BTS_OP_RATE)

/**
 * DRM_H_TIMING() - fills in horizontal timing in struct drm_display_mode
 * @HDISPLAY: Horizontal active region
 * @HFP: Horizontal front porch
 * @HSA: Horizontal sync
 * @HBP: Horizontal back porch
 *
 * This macro autocalculates and/or fills in the .hdisplay, .hsync_start,
 * .hsync_end, and .htotal timing parameters in the struct drm_display_mode
 * structure. It is meant to be used in the structure definition.
 */
#define DRM_H_TIMING(HDISPLAY, HFP, HSA, HBP) \
	.hdisplay = HDISPLAY,                 \
	.hsync_start = HDISPLAY + HFP,        \
	.hsync_end = HDISPLAY + HFP + HSA,    \
	.htotal = HDISPLAY + HFP + HSA + HBP

/**
 * DRM_V_TIMING() - fills in vertical timing in struct drm_display_mode
 * @VDISPLAY: Vertical active region
 * @VFP: Vertical front porch
 * @VSA: Vertical sync
 * @VBP: Vertical back porch
 *
 * This macro autocalculates and/or fills in the .vdisplay, .vsync_start,
 * .vsync_end, and .vtotal timing parameters in the struct drm_display_mode
 * structure. It is meant to be used in the structure definition.
 */
#define DRM_V_TIMING(VDISPLAY, VFP, VSA, VBP) \
	.vdisplay = VDISPLAY,                 \
	.vsync_start = VDISPLAY + VFP,        \
	.vsync_end = VDISPLAY + VFP + VSA,    \
	.vtotal = VDISPLAY + VFP + VSA + VBP

/**
 * DRM_MODE_TIMING() - fills in timing parameters in struct drm_display_mode
 * @REFRESH_FREQ: Image refresh frequency, in Hz
 * @HDISPLAY: Horizontal active region
 * @HFP: Horizontal front porch
 * @HSA: Horizontal sync
 * @HBP: Horizontal back porch
 * @VDISPLAY: Vertical active region
 * @VFP: Vertical front porch
 * @VSA: Vertical sync
 * @VBP: Vertical back porch
 *
 * This macro calculates the pixel clock for use in the struct drm_display_mode
 * structure, as well as the horizontal and vertical timing parameters (by way
 * of the DRM_H_TIMING() and DRM_V_TIMING() macros).
 *
 * Context: This macro may not handle fractional refresh rates correctly, and is
 *          vulnerable to rounding errors. Please double-check the resulting
 *          .clock member against a known target value, especially for lower
 *          framerates!
 */
#define DRM_MODE_TIMING(REFRESH_FREQ, HDISPLAY, HFP, HSA, HBP, VDISPLAY, VFP, VSA, VBP)       \
	.clock = ((HDISPLAY + HFP + HSA + HBP) * (VDISPLAY + VFP + VSA + VBP) * REFRESH_FREQ) \
		 / 1000,                                                                      \
	DRM_H_TIMING(HDISPLAY, HFP, HSA, HBP),                                                \
	DRM_V_TIMING(VDISPLAY, VFP, VSA, VBP)

/**
 * struct gs_display_dsc - Information about a mode's DSC parameters
 * @enabled: Whether DSC is enabled for this mode
 * @dsc_count: Number of encoders to be used by DPU (TODO:b/283964743)
 * @cfg: Configuration structure describing bulk of algorithm
 * @delay_reg_init_us: Hack for DPU delaying mode switch (TODO:b/283966795)
 *
 * Though most of the description of Display Stream Compression algorithms falls
 * within the bounds of the `struct drm_dsc_config`, this structure captures a
 * few other parameters surrounding the DSC configuration for a display mode
 * that we find useful to adjust (or refer to).
 */
struct gs_display_dsc {
	bool enabled;
	unsigned int dsc_count;

	const struct drm_dsc_config *cfg;

	unsigned int delay_reg_init_us;
};

/**
 * struct gs_display_underrun_param - Parameters to calculate underrun_lp_ref
 */
struct gs_display_underrun_param {
	/** @te_idle_us: te idle (us) to calculate underrun_lp_ref */
	unsigned int te_idle_us;
	/** @te_var: te variation (percentage) to calculate underrun_lp_ref */
	unsigned int te_var;
};

/**
 * struct gs_display_mode - gs display specific info
 */
struct gs_display_mode {
	/** @dsc: DSC parameters for the selected mode */
	struct gs_display_dsc dsc;

	/** @mode_flags: DSI mode flags from drm_mipi_dsi.h */
	unsigned long mode_flags;

	/** @vblank_usec: parameter to calculate bts */
	unsigned int vblank_usec;

	/** @te_usec: command mode: TE pulse time */
	unsigned int te_usec;

	/** @bpc: display bits per component */
	unsigned int bpc;

	/** @underrun_param: parameters to calculate underrun_lp_ref when hs_clock changes */
	const struct gs_display_underrun_param *underrun_param;

	/** @is_lp_mode: boolean, if true it means this mode is a Low Power mode */
	bool is_lp_mode;

	/**
	 * @sw_trigger:
	 *
	 * Force frame transfer to be triggered by sw instead of based on TE.
	 * This is only applicable for DSI command mode, SW trigger is the
	 * default for Video mode.
	 */
	bool sw_trigger;
};

#endif // _GS_DISPLAY_MODE_H_
