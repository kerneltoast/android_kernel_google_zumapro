/*
 * Misc utility routines for WL and Apps
 * This header file housing the define and function prototype use by
 * both the wl driver, coex fw, tools & Apps.
 *
 * Copyright (C) 2024, Broadcom.
 *
 *      Unless you and Broadcom execute a separate written software license
 * agreement governing use of this software, this software is licensed to you
 * under the terms of the GNU General Public License version 2 (the "GPL"),
 * available at http://www.broadcom.com/licenses/GPLv2.php, with the
 * following added to such license:
 *
 *      As a special exception, the copyright holders of this software give you
 * permission to link this software with independent modules, and to copy and
 * distribute the resulting executable under terms of your choice, provided that
 * you also meet, for each linked independent module, the terms and conditions of
 * the license of that module.  An independent module is a module which is not
 * derived from this software.  The special exception does not apply to any
 * modifications of the software.
 *
 *
 * <<Broadcom-WL-IPTag/Dual:>>
 */

#ifndef	_bcmwifi_channels_shared_h_
#define	_bcmwifi_channels_shared_h_

/** 80MHz channels in 6GHz band */
#define WF_NUM_6G_80M_CHANS 14

/** 160MHz channels in 6GHz band */
#define WF_NUM_6G_160M_CHANS 7  /* TBD */

/** 320MHz channels in 6GHz band */
#define WF_NUM_6G_320M_CHANS 6
/* valid channels: 0,1,2,4,5,6, channel 3 invalid as given in
 * wf_chspec_6G_id320_to_ch
 */
#define WF_NUM_6G_320M_CHAN_ID_MIN 0
#define WF_NUM_6G_320M_CHAN_ID_MAX 6

#ifdef BCMWIFI_BW320MHZ
/*
 * Returns center channel for a contiguous chanspec and
 * INVCHANNEL for non-contiguous chanspec.
 */
uint8 wf_chspec_center_channel(chanspec_t chspec);
#else /* BCMWIFI_BW320MHZ */
#define wf_chspec_center_channel(chspec) CHSPEC_CHANNEL(chspec)
#endif /* BCMWIFI_BW320MHZ */

uint8 wf_chspec_6G_id320_to_ch(uint8 chan_320MHz_id);
uint8 wf_chspec_320_id2cch(chanspec_t chanspec);

#endif	/* _bcmwifi_channels_shared_h_ */
